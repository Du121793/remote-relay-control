#include "http_service.h"

#include "cmsis_os.h"

#include "board_relay.h"
#include "adc_service.h"
#include "mqtt_link_service.h"
#include "time_sync_service.h"
#include "control_proto.h"
#include "control_queue.h"
#include "control_service.h"
#include "cluster_service.h"
#include "discovery_service.h"
#include "relay_protection.h"
#include "scheduler.h"
#include "relay_domain.h"
#include "../Config/device_config.h"

#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip.h"

#include "FreeRTOS.h"
#include "portable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward: config-sync AES-GCM decrypt helper (defined in control_queue.c) */
size_t control_queue_decrypt_body(const char *enc_body, size_t enc_len,
                                   const uint8_t key[16],
                                   char *plain_out, size_t plain_out_size);

#define HTTP_TCP_PORT               80U
#define HTTP_CONN_POOL_SIZE         2U    /* 2 connections sufficient with streaming */
#define HTTP_REQUEST_BUFFER_SIZE    1536U  /* reduced from 2048 — max form body < 1KB */
#define HTTP_BODY_BUFFER_SIZE       12288U /* reduced from 16384 — JSON API fits in 12KB */
#define HTTP_AUTH_TOKEN_SIZE        16U
#define HTTP_AUTH_SESSION_TTL_MS    (15UL * 60UL * 1000UL)
#define HTTP_AUTH_SESSION_CAPACITY  8U
#define HTTP_AUTH_SCOPE_CONTROL     0U
#define HTTP_AUTH_SCOPE_SERVICE     1U
#define HTTP_CONNECTION_IDLE_TIMEOUT_MS 30000UL
#define HTTP_AUTH_PASSWORD          "admin"
#define HTTP_BACKEND_SYNC_FALLBACK_MS (5UL * 60UL * 1000UL)

extern struct netif gnetif;
extern UART_HandleTypeDef huart3;
extern RNG_HandleTypeDef hrng;

typedef struct
{
  struct tcp_pcb *pcb;
  uint8_t active;
  uint8_t close_pending;
  uint16_t request_length;
  uint16_t expected_length;
  uint32_t last_activity_ms;
  
  /* State for progressive sending of large payloads */
  const unsigned char *send_data;
  size_t send_total;
  size_t send_progress;
  char request_buffer[HTTP_REQUEST_BUFFER_SIZE];
  char response_buffer[HTTP_BODY_BUFFER_SIZE];
} http_service_connection_t;

typedef struct
{
  uint32_t token;
  uint32_t expires_at_ms;
  uint8_t active;
  uint8_t scope;
} http_service_auth_session_t;

typedef struct
{
  http_service_auth_session_t sessions[HTTP_AUTH_SESSION_CAPACITY];
  uint32_t sessions_issued;
  uint32_t login_failures;
} http_service_auth_state_t;

static struct tcp_pcb *g_http_listener_pcb = NULL;
static http_service_stats_t g_http_service_stats;
static http_service_connection_t g_http_connections[HTTP_CONN_POOL_SIZE];
static http_service_auth_state_t g_http_auth_state;
static uint8_t g_http_service_started = 0U;
static uint8_t g_http_last_link_up = 0U;
static volatile uint8_t g_http_poll_pending = 0U;
static uint32_t g_http_poll_skip_count = 0U;
static uint32_t g_http_backend_sync_last_ms = 0U;
static uint32_t g_http_backend_sync_member_signature = 0U;
static uint8_t g_http_backend_sync_active = 0U;
static uint8_t g_http_backend_sync_cursor = 0U;
static mqtt_link_stats_t g_http_mqtt_stats_snapshot;
static void http_service_tcpip_step(void *arg);
static void http_service_uart_write(const char *text);
static void http_service_log_status(const char *text);
static void http_service_log_error_value(const char *prefix, int value);
static uint32_t http_service_active_connection_count(void);
static void http_service_close(struct tcp_pcb *tpcb, http_service_connection_t *connection);
static http_service_connection_t *http_service_alloc_connection(struct tcp_pcb *tpcb);
static void http_service_release_connection(http_service_connection_t *connection);
static err_t http_service_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t http_service_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t http_service_poll_close(void *arg, struct tcp_pcb *tpcb);
static uint8_t http_service_extract_form_text(const char *body, const char *key, char *buffer, size_t buffer_size);
static uint8_t http_service_extract_header_text(const char *request, const char *header_name, char *buffer, size_t buffer_size);
static uint32_t http_service_parse_content_length(const char *request);
static uint8_t http_service_get_session_scope(const char *request, uint8_t *scope);
static uint32_t http_service_issue_session_token(void);
static uint32_t http_service_create_session(uint8_t scope);
static void http_service_revoke_session(const char *request);
static uint32_t http_service_active_control_session_count(void);
static void http_service_format_session_json(char *buffer, size_t buffer_size, uint8_t authenticated);
static void http_service_format_login_json(char *buffer, size_t buffer_size, uint32_t token);
static void http_service_format_logout_json(char *buffer, size_t buffer_size);
static void http_service_format_cluster_config_json(char *buffer, size_t buffer_size);
static void http_service_format_cluster_nodes_json(char *buffer, size_t buffer_size);
static void http_service_format_relay_config_json(char *buffer, size_t buffer_size);
static err_t http_service_handle_status_request(http_service_connection_t *connection);
static uint8_t http_service_get_master_ip_text(char *buffer, size_t buffer_size, uint8_t *is_local_master);
static uint8_t http_service_is_master_control_allowed(char *master_ip_text, size_t buffer_size);
static void http_service_request_backend_sync(void);
static void http_service_sync_backend_config_to_cluster_async(void);
static uint32_t http_service_backend_sync_member_signature(void);
static size_t http_service_bounded_text_len(const char *text, size_t max_len);
static void http_service_json_escape_copy(char *dst, size_t dst_size, const char *src, size_t src_max_len);

#include "../../Web/web_assets.h"

static void http_service_uart_write(const char *text)
{
  size_t len;

  if ((text == NULL) || (huart3.Instance == NULL))
  {
    return;
  }

  len = strlen(text);
  if (len > 0U)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)len, 100U);
  }
}

static void http_service_log_status(const char *text)
{
  http_service_uart_write(text);
}

static void http_service_log_error_value(const char *prefix, int value)
{
  char message[64];

  (void)snprintf(message, sizeof(message), "%s%d\r\n", prefix, value);
  http_service_uart_write(message);
}

static uint32_t http_service_active_connection_count(void)
{
  uint32_t count = 0U;
  uint32_t index;

  for (index = 0U; index < HTTP_CONN_POOL_SIZE; index++)
  {
    if (g_http_connections[index].active != 0U)
    {
      count++;
    }
  }

  return count;
}

static size_t http_service_bounded_text_len(const char *text, size_t max_len)
{
  size_t len = 0U;

  if (text == NULL)
  {
    return 0U;
  }

  while ((len < max_len) && (text[len] != '\0'))
  {
    len++;
  }

  return len;
}

static void http_service_json_escape_copy(char *dst, size_t dst_size, const char *src, size_t src_max_len)
{
  size_t src_len;
  size_t si;
  size_t di = 0U;

  if ((dst == NULL) || (dst_size == 0U))
  {
    return;
  }

  dst[0] = '\0';
  if (src == NULL)
  {
    return;
  }

  src_len = http_service_bounded_text_len(src, src_max_len);
  for (si = 0U; si < src_len; ++si)
  {
    unsigned char ch = (unsigned char)src[si];
    const char *replacement = NULL;

    switch (ch)
    {
      case '\"': replacement = "\\\""; break;
      case '\\': replacement = "\\\\"; break;
      case '\b': replacement = "\\b"; break;
      case '\f': replacement = "\\f"; break;
      case '\n': replacement = "\\n"; break;
      case '\r': replacement = "\\r"; break;
      case '\t': replacement = "\\t"; break;
      default: break;
    }

    if (replacement != NULL)
    {
      size_t ri;
      for (ri = 0U; replacement[ri] != '\0'; ++ri)
      {
        if ((di + 1U) >= dst_size)
        {
          dst[di] = '\0';
          return;
        }
        dst[di++] = replacement[ri];
      }
      continue;
    }

    if (ch < 0x20U)
    {
      if ((di + 6U) >= dst_size)
      {
        dst[di] = '\0';
        return;
      }
      (void)snprintf(dst + di, dst_size - di, "\\u%04x", (unsigned int)ch);
      di += 6U;
      continue;
    }

    if ((di + 1U) >= dst_size)
    {
      dst[di] = '\0';
      return;
    }
    dst[di++] = (char)ch;
  }

  dst[di] = '\0';
}

static http_service_connection_t *http_service_alloc_connection(struct tcp_pcb *tpcb)
{
  uint32_t index;

  for (index = 0U; index < HTTP_CONN_POOL_SIZE; index++)
  {
    if (g_http_connections[index].active == 0U)
    {
      memset(&g_http_connections[index], 0, sizeof(g_http_connections[index]));
      g_http_connections[index].active = 1U;
      g_http_connections[index].pcb = tpcb;
      g_http_connections[index].last_activity_ms = HAL_GetTick();
      return &g_http_connections[index];
    }
  }

  return NULL;
}

static void http_service_release_connection(http_service_connection_t *connection)
{
  if (connection == NULL)
  {
    return;
  }

  memset(connection, 0, sizeof(*connection));
}

static void http_service_close(struct tcp_pcb *tpcb, http_service_connection_t *connection)
{
  err_t err;
  char message[64];

  if (tpcb == NULL)
  {
    http_service_release_connection(connection);
    return;
  }

  /* Stop receiving new data */
  tcp_recv(tpcb, NULL);
  
  err = tcp_close(tpcb);
  if (err == ERR_OK)
  {
    /* Close succeeded, safely clear everything */
    tcp_arg(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);
    tcp_sent(tpcb, NULL);
    http_service_release_connection(connection);
    (void)snprintf(message, sizeof(message), "[http] close ok active=%lu\r\n",
                   (unsigned long)http_service_active_connection_count());
    http_service_uart_write(message);
  }
  else
  {
    /* tcp_close failed (usually ERR_MEM because send queue is full). 
       We must keep the connection struct and poll/sent callbacks active 
       so that we can retry closing later when the queue drains. */
    if (connection != NULL)
    {
       connection->close_pending = 1U;
    }
    http_service_log_error_value("[http] close defer err=", err);
  }
}

static void http_service_close_listener(void)
{
  err_t err;

  if (g_http_listener_pcb == NULL)
  {
    return;
  }

  http_service_log_status("[http] listener down\r\n");
  tcp_arg(g_http_listener_pcb, NULL);
  tcp_accept(g_http_listener_pcb, NULL);
  err = tcp_close(g_http_listener_pcb);
  if (err != ERR_OK)
  {
    tcp_abort(g_http_listener_pcb);
  }
  g_http_listener_pcb = NULL;
}

static uint8_t http_service_parse_u32_value(const char *text, uint32_t *value_out)
{
  unsigned long value;
  char *endptr = NULL;

  if ((text == NULL) || (value_out == NULL) || (*text == '\0'))
  {
    return 0U;
  }

  value = strtoul(text, &endptr, 0);
  if ((*endptr != '\0') || (value > 0xFFFFFFFFUL))
  {
    return 0U;
  }

  *value_out = (uint32_t)value;
  return 1U;
}

static const char *http_service_find_form_value(const char *body, const char *key)
{
  const char *match = body;
  size_t key_length;

  if ((body == NULL) || (key == NULL) || (key[0] == '\0'))
  {
    return NULL;
  }

  key_length = strlen(key);
  while ((match = strstr(match, key)) != NULL)
  {
    if (((match == body) || (match[-1] == '&')) && (match[key_length] == '='))
    {
      return match + key_length + 1U;
    }
    match += key_length;
  }

  return NULL;
}

static int http_service_hex_value(char value)
{
  if ((value >= '0') && (value <= '9'))
  {
    return value - '0';
  }
  if ((value >= 'A') && (value <= 'F'))
  {
    return value - 'A' + 10;
  }
  if ((value >= 'a') && (value <= 'f'))
  {
    return value - 'a' + 10;
  }
  return -1;
}

static uint8_t http_service_extract_form_value(const char *body, const char *key, uint32_t *value_out)
{
  const char *value_text;
  char token[16];
  size_t index = 0U;

  if ((body == NULL) || (key == NULL) || (value_out == NULL))
  {
    return 0U;
  }

  value_text = http_service_find_form_value(body, key);
  if (value_text == NULL)
  {
    return 0U;
  }

  while ((*value_text != '\0') && (*value_text != '&') && (index < (sizeof(token) - 1U)))
  {
    token[index++] = *value_text++;
  }
  token[index] = '\0';

  return http_service_parse_u32_value(token, value_out);
}

static uint8_t http_service_extract_form_text(const char *body, const char *key, char *buffer, size_t buffer_size)
{
  const char *value_text;
  size_t index = 0U;

  if ((body == NULL) || (key == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  value_text = http_service_find_form_value(body, key);
  if (value_text == NULL)
  {
    return 0U;
  }

  while ((*value_text != '\0') && (*value_text != '&') && (*value_text != '\r') &&
         (*value_text != '\n') && (index < (buffer_size - 1U)))
  {
    if (*value_text == '+')
    {
      buffer[index++] = ' ';
      value_text++;
    }
    else if ((*value_text == '%') &&
             (value_text[1] != '\0') &&
             (value_text[2] != '\0'))
    {
      int high = http_service_hex_value(value_text[1]);
      int low = http_service_hex_value(value_text[2]);
      if ((high < 0) || (low < 0))
      {
        return 0U;
      }
      buffer[index++] = (char)((high << 4) | low);
      value_text += 3;
    }
    else
    {
      buffer[index++] = *value_text++;
    }
  }
  buffer[index] = '\0';

  return (index > 0U) ? 1U : 0U;
}

static uint8_t http_service_extract_header_text(const char *request, const char *header_name, char *buffer, size_t buffer_size)
{
  const char *cursor;
  const char *header_end;
  const char *line_end;
  const char *value_text;
  size_t header_name_length;
  size_t index = 0U;

  if ((request == NULL) || (header_name == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  header_end = strstr(request, "\r\n\r\n");
  if (header_end == NULL)
  {
    return 0U;
  }

  cursor = strstr(request, "\r\n");
  if (cursor == NULL)
  {
    return 0U;
  }
  cursor += 2;
  header_name_length = strlen(header_name);
  value_text = NULL;
  while (cursor < header_end)
  {
    line_end = strstr(cursor, "\r\n");
    if ((line_end == NULL) || (line_end > header_end))
    {
      line_end = header_end;
    }
    if (((size_t)(line_end - cursor) >= header_name_length) &&
        (strncmp(cursor, header_name, header_name_length) == 0))
    {
      value_text = cursor + header_name_length;
      break;
    }
    cursor = line_end + 2;
  }
  if (value_text == NULL)
  {
    return 0U;
  }

  while ((*value_text == ' ') || (*value_text == '\t'))
  {
    value_text++;
  }

  while ((*value_text != '\0') && (*value_text != '\r') && (*value_text != '\n') &&
         (index < (buffer_size - 1U)))
  {
    buffer[index++] = *value_text++;
  }
  buffer[index] = '\0';
  return (index > 0U) ? 1U : 0U;
}

static uint32_t http_service_parse_content_length(const char *request)
{
  char content_length_text[16];
  uint32_t value = 0U;

  if ((request == NULL) ||
      !http_service_extract_header_text(request, "Content-Length:", content_length_text, sizeof(content_length_text)))
  {
    return 0U;
  }

  if (http_service_parse_u32_value(content_length_text, &value) == 0U)
  {
    return 0U;
  }

  return value;
}

static uint32_t http_service_issue_session_token(void)
{
  uint32_t seed;

  if ((hrng.Instance != NULL) &&
      (HAL_RNG_GenerateRandomNumber(&hrng, &seed) == HAL_OK) &&
      (seed != 0U))
  {
    return seed;
  }

  seed = HAL_GetTick();

  seed ^= board_relay_snapshot() << 8;
  seed ^= (g_http_service_stats.requests + 1U) * 2654435761UL;
  seed ^= (g_http_auth_state.sessions_issued + 1U) * 2246822519UL;
  seed ^= 0xA5C39E17UL;
  if (seed == 0U)
  {
    seed = 0x13579BDFUL;
  }

  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed;
}

static uint8_t http_service_get_session_scope(const char *request, uint8_t *scope)
{
  char token_text[HTTP_AUTH_TOKEN_SIZE];
  uint32_t token_value;
  uint32_t now;
  uint32_t index;

  if (!http_service_extract_header_text(request, "X-Auth-Token:", token_text, sizeof(token_text)))
  {
    return 0U;
  }

  if (!http_service_parse_u32_value(token_text, &token_value))
  {
    return 0U;
  }

  now = HAL_GetTick();
  for (index = 0U; index < HTTP_AUTH_SESSION_CAPACITY; index++)
  {
    http_service_auth_session_t *session = &g_http_auth_state.sessions[index];

    if (session->active == 0U)
    {
      continue;
    }
    if ((int32_t)(now - session->expires_at_ms) >= 0)
    {
      memset(session, 0, sizeof(*session));
      continue;
    }
    if (token_value == session->token)
    {
      if (scope != NULL)
      {
        *scope = session->scope;
      }
      return 1U;
    }
  }

  return 0U;
}

static uint32_t http_service_create_session(uint8_t scope)
{
  uint32_t now = HAL_GetTick();
  uint32_t selected = (scope == HTTP_AUTH_SCOPE_SERVICE) ? 1U : 0U;
  uint32_t selected_remaining = 0xFFFFFFFFUL;
  uint32_t index;
  uint32_t token;

  if (scope == HTTP_AUTH_SCOPE_CONTROL)
  {
    memset(&g_http_auth_state.sessions[0], 0, sizeof(g_http_auth_state.sessions[0]));
  }
  else
  {
    for (index = 1U; index < HTTP_AUTH_SESSION_CAPACITY; index++)
    {
      http_service_auth_session_t *session = &g_http_auth_state.sessions[index];

      if ((session->active == 0U) || ((int32_t)(now - session->expires_at_ms) >= 0))
      {
        selected = index;
        break;
      }
      if ((session->expires_at_ms - now) < selected_remaining)
      {
        selected = index;
        selected_remaining = session->expires_at_ms - now;
      }
    }
  }

  do
  {
    token = http_service_issue_session_token();
    for (index = 0U; index < HTTP_AUTH_SESSION_CAPACITY; index++)
    {
      if ((g_http_auth_state.sessions[index].active != 0U) &&
          (g_http_auth_state.sessions[index].token == token))
      {
        token = 0U;
        break;
      }
    }
  } while (token == 0U);

  g_http_auth_state.sessions[selected].token = token;
  g_http_auth_state.sessions[selected].expires_at_ms = now + HTTP_AUTH_SESSION_TTL_MS;
  g_http_auth_state.sessions[selected].active = 1U;
  g_http_auth_state.sessions[selected].scope = scope;
  g_http_auth_state.sessions_issued++;
  return token;
}

static void http_service_revoke_session(const char *request)
{
  char token_text[HTTP_AUTH_TOKEN_SIZE];
  uint32_t token_value;
  uint32_t index;

  if (!http_service_extract_header_text(request, "X-Auth-Token:", token_text, sizeof(token_text)) ||
      !http_service_parse_u32_value(token_text, &token_value))
  {
    return;
  }

  for (index = 0U; index < HTTP_AUTH_SESSION_CAPACITY; index++)
  {
    if ((g_http_auth_state.sessions[index].active != 0U) &&
        (g_http_auth_state.sessions[index].token == token_value))
    {
      memset(&g_http_auth_state.sessions[index], 0, sizeof(g_http_auth_state.sessions[index]));
      return;
    }
  }
}

static uint32_t http_service_active_control_session_count(void)
{
  http_service_auth_session_t *session = &g_http_auth_state.sessions[0];
  uint32_t now = HAL_GetTick();

  if ((session->active != 0U) && ((int32_t)(now - session->expires_at_ms) >= 0))
  {
    memset(session, 0, sizeof(*session));
  }
  return ((session->active != 0U) && (session->scope == HTTP_AUTH_SCOPE_CONTROL)) ? 1U : 0U;
}

static err_t http_service_send_payload_chunk(http_service_connection_t *connection)
{
  struct tcp_pcb *tpcb;
  u16_t space;
  u16_t send_len;
  err_t err = ERR_OK;

  if (connection == NULL || connection->pcb == NULL)
  {
    return ERR_VAL;
  }

  tpcb = connection->pcb;

  if (connection->send_data == NULL || connection->send_progress >= connection->send_total)
  {
    return ERR_OK;
  }

  if (tcp_sndqueuelen(tpcb) >= (TCP_SND_QUEUELEN - 1U))
  {
    return ERR_OK;
  }

  space = tcp_sndbuf(tpcb);
  if (space == 0U)
  {
    return ERR_OK;
  }

  {
    size_t remaining = connection->send_total - connection->send_progress;
    if (remaining > 0xFFFFUL)
    {
      remaining = 0xFFFFUL;
    }
    send_len = (u16_t)remaining;
  }
  if (send_len > space)
  {
    send_len = space;
  }

  if (send_len > tcp_mss(tpcb))
  {
    send_len = tcp_mss(tpcb);
  }

  if (send_len == 0U)
  {
    return ERR_OK;
  }

  err = tcp_write(tpcb,
                  connection->send_data + connection->send_progress,
                  send_len,
                  TCP_WRITE_FLAG_COPY);
  if (err == ERR_MEM)
  {
    http_service_log_error_value("[http] chunk mem=", err);
    return ERR_OK;
  }
  if (err != ERR_OK)
  {
    http_service_log_error_value("[http] chunk err=", err);
    return err;
  }

  connection->send_progress += send_len;
  connection->last_activity_ms = HAL_GetTick();

  (void)tcp_output(tpcb);

  /* If we queued everything, mark ready for close */
  if (connection->send_data != NULL && connection->send_progress >= connection->send_total)
  {
      connection->close_pending = 1U;
  }

  return ERR_OK;
}

static err_t http_service_send_payload_streaming(http_service_connection_t *connection,
                                                 const char *status_line,
                                                 const char *content_type,
                                                 const unsigned char *body,
                                                 size_t body_len)
{
  char header[512];
  int header_len;
  err_t err;

  if ((connection == NULL) || (connection->pcb == NULL) || (status_line == NULL) || (content_type == NULL) || (body == NULL))
  {
    return ERR_ARG;
  }

  header_len = snprintf(header, sizeof(header),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %lu\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Headers: Content-Type, X-Auth-Token\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "\r\n",
                        status_line,
                        content_type,
                        (unsigned long)body_len);
  if ((header_len <= 0) || ((size_t)header_len >= sizeof(header)))
  {
    return ERR_BUF;
  }

  err = tcp_write(connection->pcb, header, (u16_t)header_len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    http_service_log_error_value("[http] stream header err=", err);
    /* If the header fails to queue, we must abort the stream */
    return err;
  }
  
  connection->send_data = body;
  connection->send_total = body_len;
  connection->send_progress = 0;
  connection->close_pending = 0U; /* Clear flag just in case */
  connection->last_activity_ms = HAL_GetTick();

  (void)http_service_send_payload_chunk(connection);
  return ERR_OK; 
}

static err_t http_service_send_payload(http_service_connection_t *connection,
                                       const char *status_line,
                                       const char *content_type,
                                       const char *body)
{
   /* For small JSON payloads, we can copy the body into TCP buffers. */
  char header[512];
  size_t body_len;
  int header_len;
  err_t err;
  struct tcp_pcb *tpcb;

  if (connection == NULL || connection->pcb == NULL || status_line == NULL || content_type == NULL || body == NULL)
  {
    return ERR_ARG;
  }
  
  tpcb = connection->pcb;
  body_len = strlen(body);
  header_len = snprintf(header, sizeof(header),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %lu\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Headers: Content-Type, X-Auth-Token\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "\r\n",
                        status_line,
                        content_type,
                        (unsigned long)body_len);
  if ((header_len <= 0) || ((size_t)header_len >= sizeof(header)))
  {
    return ERR_BUF;
  }

  err = tcp_write(tpcb, header, (u16_t)header_len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    http_service_log_error_value("[http] body header err=", err);
    return err;
  }

  /* Use progressive streaming for payloads larger than ~1 KB to avoid
     tcp_write ERR_MEM when the body exceeds tcp_sndbuf.  The sent callback
     drives http_service_send_payload_chunk() to push remaining data.
     For status / config JSON this is safe because the body pointer
     (connection->response_buffer) lives inside the connection struct and
     is not freed until the connection is released. */
  if (body_len > 1024U)
  {
    connection->send_data = (const unsigned char *)body;
    connection->send_total = body_len;
    connection->send_progress = 0;
    connection->close_pending = 0U;
    connection->last_activity_ms = HAL_GetTick();
    (void)http_service_send_payload_chunk(connection);
    return ERR_OK;
  }

  err = tcp_write(tpcb, body, (u16_t)body_len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    http_service_log_error_value("[http] body payload err=", err);
    return err;
  }

  /* Clear chunking state and mark close pending for short payloads */
  connection->send_data = NULL;
  connection->send_total = 0;
  connection->send_progress = 0;
  connection->close_pending = 1U;
  connection->last_activity_ms = HAL_GetTick();

  err = tcp_output(tpcb);
  if (err != ERR_OK)
  {
    http_service_log_error_value("[http] body output err=", err);
  }
  return err;
}

static void http_service_format_status_json(char *buffer, size_t buffer_size)
{
  char ip_text[16];
  char mask_text[16];
  char gw_text[16];
  static control_service_stats_t control_stats;
  static control_queue_stats_t control_queue_stats;
  static discovery_service_stats_t discovery_stats;
  mqtt_link_stats_t *mqtt_stats = &g_http_mqtt_stats_snapshot;
  static cluster_service_state_t cluster_state;
  static adc_service_state_t adc_state;
  static time_sync_service_stats_t time_sync_stats;
  size_t heap_free = xPortGetFreeHeapSize();
  size_t heap_min = xPortGetMinimumEverFreeHeapSize();
  uint32_t http_active = 0U;
  uint32_t http_close_pending = 0U;
  uint32_t index;
  uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
  uint8_t dhcp_bound = dhcp_supplied_address(&gnetif) ? 1U : 0U;

  control_service_get_stats(&control_stats);
  control_queue_get_stats(&control_queue_stats);
  discovery_service_get_stats(&discovery_stats);
  mqtt_link_service_get_stats(mqtt_stats);
  cluster_service_get_state(&cluster_state);
  adc_service_get_state(&adc_state);
  time_sync_service_get_stats(&time_sync_stats);
  (void)ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip_text, sizeof(ip_text));
  (void)ip4addr_ntoa_r(netif_ip4_netmask(&gnetif), mask_text, sizeof(mask_text));
  (void)ip4addr_ntoa_r(netif_ip4_gw(&gnetif), gw_text, sizeof(gw_text));

  for (index = 0U; index < HTTP_CONN_POOL_SIZE; index++)
  {
    if (g_http_connections[index].active != 0U)
    {
      http_active++;
      if (g_http_connections[index].close_pending != 0U)
      {
        http_close_pending++;
      }
    }
  }

  (void)snprintf(buffer, buffer_size,
                 "{"
                 "\"ok\":true,"
                 "\"uptime_ms\":%lu,"
                 "\"link_up\":%u,"
                 "\"dhcp_bound\":%u,"
                 "\"ip\":\"%s\","
                 "\"netmask\":\"%s\","
                 "\"gateway\":\"%s\","
                 "\"relay_mask\":%lu,"
                 "\"adc\":{\"initialized\":%u,\"channel_count\":%u,\"available_mask\":%lu,\"vref_valid\":%u,\"vref_raw\":%u,\"vdda_millivolts\":%lu,\"valid\":%u,\"ct_valid\":%u,\"signal_present\":%u,\"raw\":%u,\"raw_min\":%u,\"raw_max\":%u,\"raw_average\":%u,\"raw_rms\":%u,\"millivolts\":%u,\"rms_millivolts\":%u,\"current_milliamps\":%lu,\"sample_rate_hz\":%lu,\"window_samples\":%lu,\"sample_count\":%lu,\"batch_count\":%lu,\"error_count\":%lu,\"dma_overrun_count\":%lu,\"saturation_count\":%lu,\"last_sample_ms\":%lu,\"last_hal_status\":%lu},"
                 "\"cluster\":{\"enabled\":%u,\"name\":\"%s\",\"role\":%u,\"join_mode\":%u,\"self_node_id\":%lu,\"master_node_id\":%lu,\"term\":%lu,\"master_eligible\":%u,\"health_score\":%lu,\"health_lateness_ms\":%lu,\"health_bad_streak\":%lu,\"unix_time\":%lu,\"time_valid\":%u,\"time_source\":%u,\"time_quality\":%u,\"time_sync_count\":%lu,\"time_seq\":%lu,\"gateway_epoch\":%lu,\"gateway_node_id\":%lu,\"last_time_offset_seconds\":%ld,\"rtc_lse_ready\":%u,\"rtc_bdcr\":%lu,\"rtc_lse_drive\":%lu,\"rtc_lse_bypass\":%u},"
                 "\"time_sync\":{\"active\":%u,\"waiting_backend\":%u,\"last_source\":%u,\"ntp_attempts\":%lu,\"ntp_successes\":%lu,\"ntp_failures\":%lu,\"backend_requests\":%lu,\"backend_successes\":%lu,\"backend_failures\":%lu,\"last_sync_ms\":%lu,\"next_attempt_ms\":%lu,\"last_error\":%lu},"
                 "\"ports\":{\"control\":41000,\"discovery\":40000,\"http\":80},"
                 "\"rtos\":{\"heap_free\":%lu,\"heap_min\":%lu},"
                 "\"control\":{\"connections\":%lu,\"commands\":%lu,\"errors\":%lu,\"listener_up\":%lu},"
                 "\"control_queue\":{\"submitted\":%lu,\"executed\":%lu,\"dropped\":%lu,\"queue_full\":%lu,\"busy_rejected\":%lu,\"depth\":%lu,\"high_watermark\":%lu,\"capacity\":%lu},"
                 "\"discovery\":{\"rx\":%lu,\"tx_attempts\":%lu,\"announce\":%lu,\"tx_failures\":%lu,\"discover\":%lu,\"last_tx_err\":%ld,\"last_payload_len\":%lu},"
                 "\"auth\":{\"required\":1,\"active_session\":%u,\"sessions_issued\":%lu,\"login_failures\":%lu},"
                 "\"http\":{\"connections\":%lu,\"requests\":%lu,\"errors\":%lu,\"active\":%lu,\"close_pending\":%lu,\"listener_up\":%lu},"
                 "\"cloud\":{\"configured\":%u,\"enabled\":%u,\"should_run\":%u,\"connected\":%u,\"transport\":%u,\"broker_port\":%u,\"connect_attempts\":%lu,\"connect_failures\":%lu,\"published_hello\":%lu,\"published_heartbeat\":%lu,\"published_status\":%lu,\"published_fault_reports\":%lu,\"received_commands\":%lu,\"command_errors\":%lu,\"published_acks\":%lu,\"ack_queue_dropped\":%lu,\"ack_queue_depth\":%lu,\"last_error\":%lu,\"status_dirty_count\":%lu,\"ca_fingerprint_configured\":%u,\"binding_secret_configured\":%u},"
                 "\"lwip\":{\"mem_avail\":%lu,\"mem_used\":%lu,\"mem_err\":%lu,\"tcp_pcb_used\":%lu,\"tcp_pcb_max\":%lu,\"tcp_seg_used\":%lu,\"tcp_seg_max\":%lu,\"pbuf_used\":%lu,\"pbuf_max\":%lu,\"sys_timeout_used\":%lu,\"sys_timeout_max\":%lu,\"sys_timeout_err\":%lu,\"eth_tx\":%lu,\"eth_tx_err\":%lu,\"eth_tx_free\":%lu,\"eth_tx_reclaim\":%lu,\"eth_tx_reclaim_schedule_err\":%lu,\"eth_last_tx_hal_status\":%lu,\"eth_last_tx_hal_error\":%lu,\"eth_last_tx_dma_error\":%lu,\"eth_rx\":%lu,\"eth_rx_null\":%lu,\"eth_rx_alloc_fail\":%lu,\"eth_rx_alloc_recover\":%lu,\"eth_rx_alloc_stalled\":%lu,\"eth_last_rx_ms\":%lu,\"eth_dma_status\":%lu,\"eth_rx_build_pending\":%lu}"
                 "}",
                 (unsigned long)HAL_GetTick(),
                 (unsigned int)link_up,
                 (unsigned int)dhcp_bound,
                 ip_text,
                 mask_text,
                 gw_text,
                 (unsigned long)board_relay_snapshot(),
                 (unsigned int)adc_state.initialized,
                 (unsigned int)adc_state.channel_count,
                 (unsigned long)adc_state.available_mask,
                 (unsigned int)adc_state.vref_valid,
                 (unsigned int)adc_state.vref_raw_average,
                 (unsigned long)adc_state.vdda_millivolts,
                 (unsigned int)adc_state.valid,
                 (unsigned int)adc_state.ct_valid,
                 (unsigned int)adc_state.signal_present,
                 (unsigned int)adc_state.raw_latest,
                 (unsigned int)adc_state.raw_min,
                 (unsigned int)adc_state.raw_max,
                 (unsigned int)adc_state.raw_average,
                 (unsigned int)adc_state.raw_rms,
                 (unsigned int)adc_state.millivolts,
                 (unsigned int)adc_state.ac_rms_millivolts,
                 (unsigned long)adc_state.current_milliamps,
                 (unsigned long)adc_state.sample_rate_hz,
                 (unsigned long)adc_state.window_samples,
                 (unsigned long)adc_state.sample_count,
                 (unsigned long)adc_state.batch_count,
                 (unsigned long)adc_state.error_count,
                 (unsigned long)adc_state.dma_overrun_count,
                 (unsigned long)adc_state.saturation_count,
                 (unsigned long)adc_state.last_sample_ms,
                 (unsigned long)adc_state.last_hal_status,
                 (unsigned int)cluster_state.cluster_enabled,
                 cluster_state.cluster_name,
                 (unsigned int)cluster_state.role,
                 (unsigned int)cluster_state.join_mode,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned long)cluster_state.master_node_id,
                 (unsigned long)cluster_state.term,
                 (unsigned int)cluster_state.master_eligible,
                 (unsigned long)cluster_state.health_score,
                 (unsigned long)cluster_state.health_lateness_ms,
                 (unsigned long)cluster_state.health_bad_streak,
                 (unsigned long)cluster_state.unix_time,
                 (unsigned int)cluster_state.time_valid,
                 (unsigned int)cluster_state.time_source,
                 (unsigned int)cluster_state.time_quality,
                 (unsigned long)cluster_state.time_sync_count,
                 (unsigned long)cluster_state.time_seq,
                 (unsigned long)cluster_state.gateway_epoch,
                 (unsigned long)cluster_state.gateway_node_id,
                 (long)cluster_state.last_time_offset_seconds,
                 (unsigned int)cluster_state.rtc_lse_ready,
                 (unsigned long)RCC->BDCR,
                 (unsigned long)((RCC->BDCR & RCC_BDCR_LSEDRV) >>
                                 RCC_BDCR_LSEDRV_Pos),
                 (unsigned int)(((RCC->BDCR & RCC_BDCR_LSEBYP) != 0U)
                                ? 1U
                                : 0U),
                 (unsigned int)time_sync_stats.active,
                 (unsigned int)time_sync_stats.waiting_backend,
                 (unsigned int)time_sync_stats.last_source,
                 (unsigned long)time_sync_stats.ntp_attempts,
                 (unsigned long)time_sync_stats.ntp_successes,
                 (unsigned long)time_sync_stats.ntp_failures,
                 (unsigned long)time_sync_stats.backend_requests,
                 (unsigned long)time_sync_stats.backend_successes,
                 (unsigned long)time_sync_stats.backend_failures,
                 (unsigned long)time_sync_stats.last_sync_ms,
                 (unsigned long)time_sync_stats.next_attempt_ms,
                 (unsigned long)time_sync_stats.last_error,
                 (unsigned long)heap_free,
                 (unsigned long)heap_min,
                 (unsigned long)control_stats.connections,
                 (unsigned long)control_stats.commands,
                 (unsigned long)control_stats.errors,
                 (unsigned long)control_stats.listener_up,
                 (unsigned long)control_queue_stats.submitted,
                 (unsigned long)control_queue_stats.executed,
                 (unsigned long)control_queue_stats.dropped,
                 (unsigned long)control_queue_stats.queue_full,
                 (unsigned long)control_queue_stats.busy_rejected,
                 (unsigned long)control_queue_stats.current_depth,
                 (unsigned long)control_queue_stats.high_watermark,
                 (unsigned long)control_queue_stats.capacity,
                 (unsigned long)discovery_stats.rx_packets,
                 (unsigned long)discovery_stats.tx_attempts,
                 (unsigned long)discovery_stats.announce_packets,
                 (unsigned long)discovery_stats.tx_failures,
                 (unsigned long)discovery_stats.discover_packets,
                 (long)discovery_stats.last_tx_err,
                 (unsigned long)discovery_stats.last_payload_len,
                 (unsigned int)http_service_active_control_session_count(),
                 (unsigned long)g_http_auth_state.sessions_issued,
                 (unsigned long)g_http_auth_state.login_failures,
                 (unsigned long)g_http_service_stats.connections,
                 (unsigned long)g_http_service_stats.requests,
                 (unsigned long)g_http_service_stats.errors,
                 (unsigned long)http_active,
                 (unsigned long)http_close_pending,
                 (unsigned long)((g_http_listener_pcb != NULL) ? 1U : 0U),
                 (unsigned int)mqtt_stats->configured,
                 (unsigned int)mqtt_stats->enabled,
                 (unsigned int)mqtt_stats->should_run,
                 (unsigned int)mqtt_stats->connected,
                 (unsigned int)mqtt_stats->transport,
                 (unsigned int)mqtt_stats->broker_port,
                 (unsigned long)mqtt_stats->connect_attempts,
                 (unsigned long)mqtt_stats->connect_failures,
                 (unsigned long)mqtt_stats->published_hello,
                 (unsigned long)mqtt_stats->published_heartbeat,
                 (unsigned long)mqtt_stats->published_status,
                 (unsigned long)mqtt_stats->published_fault_reports,
                 (unsigned long)mqtt_stats->received_commands,
                 (unsigned long)mqtt_stats->command_errors,
                 (unsigned long)mqtt_stats->published_acks,
                 (unsigned long)mqtt_stats->ack_queue_dropped,
                 (unsigned long)mqtt_stats->ack_queue_depth,
                 (unsigned long)mqtt_stats->last_error,
                 (unsigned long)mqtt_stats->status_dirty_count,
                 (unsigned int)(mqtt_stats->cluster_ca_fingerprint[0] != '\0'),
                 (unsigned int)(mqtt_stats->cluster_binding_secret[0] != '\0'),
#if LWIP_STATS
                 (unsigned long)lwip_stats.mem.avail,
                 (unsigned long)lwip_stats.mem.used,
                 (unsigned long)lwip_stats.mem.err,
                 (unsigned long)MEMP_STATS_GET(used, MEMP_TCP_PCB),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_TCP_PCB),
                 (unsigned long)MEMP_STATS_GET(used, MEMP_TCP_SEG),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_TCP_SEG),
                 (unsigned long)MEMP_STATS_GET(used, MEMP_PBUF),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_PBUF),
#else
                 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL,
#endif
#if MEMP_STATS
                 (unsigned long)MEMP_STATS_GET(used, MEMP_SYS_TIMEOUT),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_SYS_TIMEOUT),
                 (unsigned long)MEMP_STATS_GET(err, MEMP_SYS_TIMEOUT),
#else
                 0UL, 0UL, 0UL,
#endif
                 (unsigned long)g_eth_tx_packets,
                 (unsigned long)g_eth_tx_errors,
                 (unsigned long)g_eth_tx_free_packets,
                 (unsigned long)g_eth_tx_reclaim_calls,
                 (unsigned long)g_eth_tx_reclaim_schedule_errors,
                 (unsigned long)g_eth_last_tx_hal_status,
                 (unsigned long)g_eth_last_tx_hal_error,
                 (unsigned long)g_eth_last_tx_dma_error,
                 (unsigned long)g_eth_rx_packets,
                 (unsigned long)g_eth_rx_null_reads,
                 (unsigned long)g_eth_rx_alloc_failures,
                 (unsigned long)g_eth_rx_alloc_recoveries,
                 (unsigned long)g_eth_rx_alloc_stalled,
                 (unsigned long)g_eth_last_rx_ms,
                 (unsigned long)heth.Instance->DMASR,
                 (unsigned long)heth.RxDescList.RxBuildDescCnt);

  /* Append per-channel ADC data after the main JSON closes */
  {
    size_t pos = strlen(buffer);
    if (pos > 1U && buffer[pos - 1U] == '}')
    {
      int w; uint8_t ch;
      buffer[pos - 1U] = '\0';
      w = snprintf(buffer + pos - 1U, buffer_size - (pos - 1U), ",\"adc_channels\":[");
      if (w > 0) pos = (pos - 1U) + (size_t)w;
      for (ch = 0U; ch < ADC_SERVICE_CHANNEL_COUNT; ++ch)
      {
        adc_service_channel_state_t cs;
        (void)adc_service_get_channel_state(ch, &cs);
        w = snprintf(buffer + pos, buffer_size - pos,
                     "%s{\"ch\":%u,\"ct\":%u,\"mA\":%lu,\"bias\":%u,\"rms\":%u}",
                     (ch == 0U) ? "" : ",", (unsigned int)ch,
                     (unsigned int)cs.ct_valid, (unsigned long)cs.current_milliamps,
                     (unsigned int)cs.millivolts, (unsigned int)cs.ac_rms_millivolts);
        if (w > 0 && (size_t)w < (buffer_size - pos)) pos += (size_t)w;
        else break;
      }
      w = snprintf(buffer + pos, buffer_size - pos, "]}");
    }
  }
}

__attribute__((noinline)) static err_t http_service_handle_status_request(http_service_connection_t *connection)
{
  if (connection == NULL)
  {
    return ERR_ARG;
  }

  http_service_format_status_json(connection->response_buffer, sizeof(connection->response_buffer));
  return http_service_send_payload(connection, "200 OK", "application/json", connection->response_buffer);
}

static void http_service_format_relay_json(char *buffer, size_t buffer_size, uint32_t relay_mask)
{
  (void)snprintf(buffer, buffer_size,
                 "{"
                 "\"ok\":true,"
                 "\"relay_mask\":%lu,"
                 "\"ch1\":%u,\"ch2\":%u,\"ch3\":%u,\"ch4\":%u,"
                 "\"ch5\":%u,\"ch6\":%u,\"ch7\":%u,\"ch8\":%u,"
                 "\"ch9\":%u,\"ch10\":%u,\"ch11\":%u,\"ch12\":%u,"
                 "\"ch13\":%u,\"ch14\":%u,\"ch15\":%u,\"ch16\":%u"
                 "}",
                 (unsigned long)relay_mask,
                 (unsigned int)((relay_mask >> 0) & 0x1U),
                 (unsigned int)((relay_mask >> 1) & 0x1U),
                 (unsigned int)((relay_mask >> 2) & 0x1U),
                 (unsigned int)((relay_mask >> 3) & 0x1U),
                 (unsigned int)((relay_mask >> 4) & 0x1U),
                 (unsigned int)((relay_mask >> 5) & 0x1U),
                 (unsigned int)((relay_mask >> 6) & 0x1U),
                 (unsigned int)((relay_mask >> 7) & 0x1U),
                 (unsigned int)((relay_mask >> 8) & 0x1U),
                 (unsigned int)((relay_mask >> 9) & 0x1U),
                 (unsigned int)((relay_mask >> 10) & 0x1U),
                 (unsigned int)((relay_mask >> 11) & 0x1U),
                 (unsigned int)((relay_mask >> 12) & 0x1U),
                 (unsigned int)((relay_mask >> 13) & 0x1U),
                 (unsigned int)((relay_mask >> 14) & 0x1U),
                 (unsigned int)((relay_mask >> 15) & 0x1U));
}

static void http_service_format_error_json(char *buffer, size_t buffer_size, const char *message)
{
  (void)snprintf(buffer, buffer_size,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 (message != NULL) ? message : "unknown");
}

static void http_service_format_session_json(char *buffer, size_t buffer_size, uint8_t authenticated)
{
  (void)snprintf(buffer, buffer_size,
                 "{"
                 "\"ok\":true,"
                 "\"auth_required\":true,"
                 "\"authenticated\":%u,"
                 "\"active_session\":%u,"
                 "\"ttl_ms\":%lu"
                 "}",
                 (unsigned int)authenticated,
                 (unsigned int)http_service_active_control_session_count(),
                 (unsigned long)HTTP_AUTH_SESSION_TTL_MS);
}

static void http_service_format_login_json(char *buffer, size_t buffer_size, uint32_t token)
{
  (void)snprintf(buffer, buffer_size,
                 "{"
                 "\"ok\":true,"
                 "\"token\":%lu,"
                 "\"expires_in_ms\":%lu"
                 "}",
                 (unsigned long)token,
                 (unsigned long)HTTP_AUTH_SESSION_TTL_MS);
}

static void http_service_format_logout_json(char *buffer, size_t buffer_size)
{
  (void)snprintf(buffer, buffer_size,
                 "{\"ok\":true,\"logged_out\":true}");
}

static uint8_t http_service_get_master_ip_text(char *buffer, size_t buffer_size, uint8_t *is_local_master)
{
  cluster_service_state_t cluster_state;
  const cluster_node_t *master_node;
  ip4_addr_t ip_addr;

  if ((buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  buffer[0] = '\0';
  if (is_local_master != NULL)
  {
    *is_local_master = 0U;
  }

  cluster_service_get_state(&cluster_state);
  if (cluster_state.cluster_enabled == 0U)
  {
    return 0U;
  }

  if ((cluster_state.role == CLUSTER_ROLE_MASTER) ||
      (cluster_state.master_node_id == cluster_state.self_node_id))
  {
    (void)ip4addr_ntoa_r(netif_ip4_addr(&gnetif), buffer, buffer_size);
    if (is_local_master != NULL)
    {
      *is_local_master = 1U;
    }
    return 1U;
  }

  master_node = cluster_service_find_node(cluster_state.master_node_id);
  if ((master_node == NULL) || (master_node->valid == 0U))
  {
    return 0U;
  }

  ip_addr.addr = master_node->ip_addr;
  (void)ip4addr_ntoa_r(&ip_addr, buffer, buffer_size);
  return 1U;
}

static uint8_t http_service_is_master_control_allowed(char *master_ip_text, size_t buffer_size)
{
  cluster_service_state_t cluster_state;
  uint8_t is_local_master = 0U;

  cluster_service_get_state(&cluster_state);
  if (cluster_state.cluster_enabled == 0U)
  {
    if ((master_ip_text != NULL) && (buffer_size > 0U))
    {
      master_ip_text[0] = '\0';
    }
    return 1U;
  }

  if ((cluster_state.role == CLUSTER_ROLE_MASTER) &&
      (cluster_state.master_node_id == cluster_state.self_node_id))
  {
    (void)http_service_get_master_ip_text(master_ip_text, buffer_size, &is_local_master);
    return 1U;
  }

  if (http_service_get_master_ip_text(master_ip_text, buffer_size, &is_local_master) == 0U)
  {
    return 0U;
  }

  return (is_local_master != 0U) ? 1U : 0U;
}

static void http_service_request_backend_sync(void)
{
  g_http_backend_sync_active = 1U;
  g_http_backend_sync_cursor = 0U;
}

static void http_service_sync_backend_config_to_cluster_async(void)
{
  cluster_service_state_t cluster_state;
  const cluster_node_t *nodes;
  device_config_t config_snap; device_config_copy(&config_snap); const device_config_t *config = &config_snap;
  uint32_t count = 0U;
  uint32_t index;
  cluster_service_get_state(&cluster_state);
  if ((cluster_state.role != CLUSTER_ROLE_MASTER) ||
      (config->cluster_enabled == 0U) ||
      (config->backend_key[0] == '\0'))
  {
    g_http_backend_sync_active = 0U;
    g_http_backend_sync_cursor = 0U;
    return;
  }

  nodes = cluster_service_get_nodes(&count);
  (void)count;
  for (index = g_http_backend_sync_cursor;
       index < CLUSTER_SERVICE_NODE_CAPACITY;
       ++index)
  {
    if ((nodes[index].valid == 0U) ||
        (nodes[index].online == 0U) ||
        (nodes[index].ip_addr == 0U) ||
        (nodes[index].role != CLUSTER_ROLE_MEMBER) ||
        (strcmp(nodes[index].cluster_name, config->cluster_name) != 0))
    {
      continue;
    }
    {
      uint32_t expected_tag =
        device_config_compute_auth_tag(config->cluster_password_hash,
                                       nodes[index].unix_time,
                                       nodes[index].node_id,
                                       nodes[index].cluster_name);
      if (nodes[index].auth_tag != expected_tag)
      {
        continue;
      }
    }

    if (control_queue_submit_backend_sync_async(nodes[index].ip_addr,
                                                config) == HAL_OK)
    {
      g_http_backend_sync_cursor = (uint8_t)(index + 1U);
    }
    return;
  }

  g_http_backend_sync_active = 0U;
  g_http_backend_sync_cursor = 0U;
}

static uint32_t http_service_backend_sync_member_signature(void)
{
  cluster_service_state_t cluster_state;
  const cluster_node_t *nodes;
  const device_config_t *config = device_config_get();
  uint32_t count = 0U;
  uint32_t index;
  uint32_t signature = 2166136261UL;
  uint32_t member_count = 0U;
  cluster_service_get_state(&cluster_state);
  if ((cluster_state.role != CLUSTER_ROLE_MASTER) ||
      (config->cluster_enabled == 0U))
  {
    return 0U;
  }

  nodes = cluster_service_get_nodes(&count);
  (void)count;
  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    if ((nodes[index].valid == 0U) ||
        (nodes[index].online == 0U) ||
        (nodes[index].ip_addr == 0U) ||
        (nodes[index].role != CLUSTER_ROLE_MEMBER) ||
        (strcmp(nodes[index].cluster_name, config->cluster_name) != 0))
    {
      continue;
    }
    {
      uint32_t expected_tag =
        device_config_compute_auth_tag(config->cluster_password_hash,
                                       nodes[index].unix_time,
                                       nodes[index].node_id,
                                       nodes[index].cluster_name);
      if (nodes[index].auth_tag != expected_tag)
      {
        continue;
      }
    }

    signature ^= nodes[index].node_id;
    signature *= 16777619UL;
    signature ^= nodes[index].ip_addr;
    signature *= 16777619UL;
    signature ^= nodes[index].auth_tag;
    signature *= 16777619UL;
    member_count++;
  }

  signature ^= member_count;
  signature *= 16777619UL;
  return signature;
}

static void http_service_format_cluster_config_json(char *buffer, size_t buffer_size)
{
  const device_config_t *config = device_config_get();
  static cluster_service_state_t cluster_state;
  static char master_ip[16];
  static char device_name_json[(DEVICE_CONFIG_NAME_MAX_LEN * 2U) + 8U];
  static char cluster_name_json[(DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN * 2U) + 8U];
  static char backend_host_json[(DEVICE_CONFIG_BACKEND_HOST_MAX_LEN * 2U) + 8U];
  static char cloud_broker_host_json[(DEVICE_CONFIG_CLOUD_HOST_MAX_LEN * 2U) + 8U];
  static char cloud_cluster_id_json[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U) + 8U];
  static char cloud_device_id_json[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U) + 8U];
  static char cloud_key_id_json[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U) + 8U];
  static char cloud_cluster_ca_fingerprint_json[(DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN * 2U) + 8U];
  /* binding_secret is no longer returned in GET responses — only
     secret_configured boolean.  The JSON-escape buffer is retained for
     the POST path where the field is still accepted. */
  uint8_t is_local_master = 0U;

  cluster_service_get_state(&cluster_state);
  (void)http_service_get_master_ip_text(master_ip, sizeof(master_ip), &is_local_master);
  http_service_json_escape_copy(device_name_json, sizeof(device_name_json),
                                config->device_name, sizeof(config->device_name));
  http_service_json_escape_copy(cluster_name_json, sizeof(cluster_name_json),
                                config->cluster_name, sizeof(config->cluster_name));
  http_service_json_escape_copy(backend_host_json, sizeof(backend_host_json),
                                config->backend_host, sizeof(config->backend_host));
  http_service_json_escape_copy(cloud_broker_host_json, sizeof(cloud_broker_host_json),
                                config->cloud_broker_host, sizeof(config->cloud_broker_host));
  http_service_json_escape_copy(cloud_cluster_id_json, sizeof(cloud_cluster_id_json),
                                config->cloud_cluster_id, sizeof(config->cloud_cluster_id));
  http_service_json_escape_copy(cloud_device_id_json, sizeof(cloud_device_id_json),
                                config->cloud_device_id, sizeof(config->cloud_device_id));
  http_service_json_escape_copy(cloud_key_id_json, sizeof(cloud_key_id_json),
                                config->cloud_key_id, sizeof(config->cloud_key_id));
  http_service_json_escape_copy(cloud_cluster_ca_fingerprint_json, sizeof(cloud_cluster_ca_fingerprint_json),
                                config->cloud_cluster_ca_fingerprint, sizeof(config->cloud_cluster_ca_fingerprint));
  (void)snprintf(buffer, buffer_size,
                 "{"
                 "\"ok\":true,"
                 "\"device_name\":\"%s\","
                 "\"cluster_enabled\":%u,"
                 "\"auto_join_cluster\":%u,"
                 "\"cluster_name\":\"%s\","
                 "\"password_configured\":%u,"
                 "\"role\":%u,"
                 "\"join_mode\":%u,"
                 "\"self_node_id\":%lu,"
                 "\"master_node_id\":%lu,"
                 "\"term\":%lu,"
                 "\"master_ip\":\"%s\","
                 "\"is_local_master\":%u,"
                 "\"backend_host\":\"%s\","
                 "\"backend_port\":%u,"
                 "\"backend_key_configured\":%u,"
                 "\"cloud_enabled\":%u,"
                 "\"cloud_transport\":%u,"
                 "\"cloud_broker_host\":\"%s\","
                 "\"cloud_broker_port\":%u,"
                 "\"cloud_cluster_id\":\"%s\","
                 "\"cloud_device_id\":\"%s\","
                 "\"cloud_key_id\":\"%s\","
                 "\"cloud_cluster_ca_fingerprint\":\"%s\","
                 "\"cloud_cluster_binding_secret_configured\":%u,"
                 "\"cloud_cluster_ca_cert_configured\":%u"
                 "}",
                 device_name_json,
                 (unsigned int)config->cluster_enabled,
                 (unsigned int)config->auto_join_cluster,
                 cluster_name_json,
                 (unsigned int)(config->cluster_password_hash[0] != 0U ? 1U : 0U),
                 (unsigned int)cluster_state.role,
                 (unsigned int)cluster_state.join_mode,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned long)cluster_state.master_node_id,
                 (unsigned long)cluster_state.term,
                 master_ip,
                 (unsigned int)is_local_master,
                 backend_host_json,
                 (unsigned int)config->backend_port,
                 (unsigned int)(config->backend_key[0] != '\0' ? 1U : 0U),
                 (unsigned int)config->cloud_enabled,
                 (unsigned int)config->cloud_transport,
                 cloud_broker_host_json,
                 (unsigned int)config->cloud_broker_port,
                 cloud_cluster_id_json,
                 cloud_device_id_json,
                 cloud_key_id_json,
                 cloud_cluster_ca_fingerprint_json,
                 (unsigned int)(config->cloud_cluster_binding_secret[0] != '\0' ? 1U : 0U),
                 (unsigned int)(config->cloud_cluster_ca_cert[0] != '\0' ? 1U : 0U));
}

static void http_service_format_cluster_nodes_json(char *buffer, size_t buffer_size)
{
  const cluster_node_t *nodes;
  const device_config_t *config = device_config_get();
  uint32_t count = 0U;
  uint32_t index;
  /* local_auth_tag is no longer a simple FNV1a — nodes now use
     HMAC-SHA256.  The per-node check below recomputes via the shared
     device_config_compute_auth_tag(). */
  size_t used;
  static char ip_text[16];
  static char cluster_name_json[(CLUSTER_SERVICE_CLUSTER_NAME_LEN * 2U) + 8U];
  static char node_name_json[(CLUSTER_SERVICE_DEVICE_NAME_LEN * 2U) + 8U];
  static char cloud_cluster_id_json[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U) + 8U];
  static char cloud_device_id_json[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U) + 8U];
  static char cloud_key_id_json[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U) + 8U];
  static ip4_addr_t ip_addr;

  nodes = cluster_service_get_nodes(&count);
  used = (size_t)snprintf(buffer, buffer_size, "{\"ok\":true,\"nodes\":[");
  if (used >= buffer_size)
  {
    return;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    int written;
    uint8_t same_cluster;
    if (nodes[index].valid == 0U)
    {
      continue;
    }

    {
      uint32_t expected_tag =
        device_config_compute_auth_tag(config->cluster_password_hash,
                                       nodes[index].unix_time,
                                       nodes[index].node_id,
                                       nodes[index].cluster_name);
      same_cluster = ((config->cluster_enabled != 0U) &&
                      (strcmp(nodes[index].cluster_name, config->cluster_name) == 0) &&
                      (nodes[index].auth_tag == expected_tag)) ? 1U : 0U;
    }
    http_service_json_escape_copy(cluster_name_json, sizeof(cluster_name_json),
                                  nodes[index].cluster_name, sizeof(nodes[index].cluster_name));
    http_service_json_escape_copy(node_name_json, sizeof(node_name_json),
                                  nodes[index].node_name, sizeof(nodes[index].node_name));
    http_service_json_escape_copy(cloud_cluster_id_json, sizeof(cloud_cluster_id_json),
                                  nodes[index].cloud_cluster_id, sizeof(nodes[index].cloud_cluster_id));
    http_service_json_escape_copy(cloud_device_id_json, sizeof(cloud_device_id_json),
                                  nodes[index].cloud_device_id, sizeof(nodes[index].cloud_device_id));
    http_service_json_escape_copy(cloud_key_id_json, sizeof(cloud_key_id_json),
                                  nodes[index].cloud_key_id, sizeof(nodes[index].cloud_key_id));
    ip_addr.addr = nodes[index].ip_addr;
    (void)ip4addr_ntoa_r(&ip_addr, ip_text, sizeof(ip_text));
    written = snprintf(buffer + used,
                       buffer_size - used,
                       "%s{\"node_id\":%lu,\"ip\":%lu,\"ip_text\":\"%s\",\"role\":%u,\"term\":%lu,\"load\":%lu,\"relay_mask\":%lu,\"enabled_mask\":%lu,\"adc_valid\":%u,\"adc_ct_valid\":%u,\"adc_signal_present\":%u,\"adc_raw\":%lu,\"adc_millivolts\":%lu,\"adc_min\":%lu,\"adc_max\":%lu,\"adc_average\":%lu,\"adc_raw_rms\":%lu,\"adc_rms_millivolts\":%lu,\"adc_current_milliamps\":%lu,\"adc_sample_rate_hz\":%lu,\"adc_window_samples\":%lu,\"adc_sample_count\":%lu,\"adc_dma_overruns\":%lu,\"adc_saturation_count\":%lu,\"auto\":%u,\"online\":%u,\"device_state\":%u,\"same_cluster\":%u,\"controllable\":%u,\"auth_match\":%u,\"cluster_name\":\"%s\",\"name\":\"%s\",\"cloud_cluster_id\":\"%s\",\"cloud_device_id\":\"%s\",\"cloud_key_id\":\"%s\"}",
                       (used > (size_t)20U) ? "," : "",
                       (unsigned long)nodes[index].node_id,
                       (unsigned long)nodes[index].ip_addr,
                       ip_text,
                       (unsigned int)nodes[index].role,
                       (unsigned long)nodes[index].term,
                       (unsigned long)nodes[index].load_score,
                       (unsigned long)nodes[index].relay_mask,
                       (unsigned long)nodes[index].enabled_mask,
                       (unsigned int)nodes[index].adc_valid,
                       (unsigned int)nodes[index].adc_ct_valid,
                       (unsigned int)nodes[index].adc_signal_present,
                       (unsigned long)nodes[index].adc_raw,
                       (unsigned long)nodes[index].adc_millivolts,
                       (unsigned long)nodes[index].adc_min,
                       (unsigned long)nodes[index].adc_max,
                       (unsigned long)nodes[index].adc_average,
                       (unsigned long)nodes[index].adc_raw_rms,
                       (unsigned long)nodes[index].adc_rms_millivolts,
                       (unsigned long)nodes[index].adc_current_milliamps,
                       (unsigned long)nodes[index].adc_sample_rate_hz,
                       (unsigned long)nodes[index].adc_window_samples,
                       (unsigned long)nodes[index].adc_sample_count,
                       (unsigned long)nodes[index].adc_dma_overruns,
                       (unsigned long)nodes[index].adc_saturation_count,
                       (unsigned int)nodes[index].auto_join,
                       (unsigned int)nodes[index].online,
                       (unsigned int)nodes[index].device_state,
                       (unsigned int)same_cluster,
                       (unsigned int)same_cluster,
                       (unsigned int)same_cluster,
                       cluster_name_json,
                       node_name_json,
                       cloud_cluster_id_json,
                       cloud_device_id_json,
                       cloud_key_id_json);
    if ((written < 0) || ((size_t)written >= (buffer_size - used)))
    {
      break;
    }
    used += (size_t)written;
  }

  if (used < buffer_size)
  {
    (void)snprintf(buffer + used, buffer_size - used, "],\"count\":%lu}", (unsigned long)count);
  }
}

static void http_service_format_relay_config_json(char *buffer, size_t buffer_size)
{
  const device_config_t *config = device_config_get();
  size_t used;
  uint32_t index;
  char relay_name_json[(DEVICE_CONFIG_NAME_MAX_LEN * 2U) + 8U];
  char log_message[64];

  used = (size_t)snprintf(buffer, buffer_size, "{\"ok\":true,\"relays\":[");
  if (used >= buffer_size)
  {
    return;
  }

  for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
  {
    adc_service_protection_state_t protection_state;

    memset(&protection_state, 0, sizeof(protection_state));
    (void)adc_service_get_protection_state((uint8_t)(index + 1U),
                                           &protection_state);
    http_service_json_escape_copy(relay_name_json, sizeof(relay_name_json),
                                  config->relay[index].name, sizeof(config->relay[index].name));
    int written = snprintf(buffer + used,
                           buffer_size - used,
                           "%s{\"channel\":%lu,\"name\":\"%s\",\"enabled\":%u,\"remember_state\":%u,\"boot_default_on\":%u,\"adc_channel\":%u,\"protection_enabled\":%u,\"overcurrent_milliamps\":%lu,\"trip_delay_ms\":%lu,\"reset_hysteresis_milliamps\":%lu,\"protection_above_threshold\":%u,\"protection_tripped\":%u,\"protection_trip_count\":%lu,\"protection_last_trip_ms\":%lu}",
                           (index > 0U) ? "," : "",
                           (unsigned long)(index + 1U),
                           relay_name_json,
                           (unsigned int)config->relay[index].enabled,
                           (unsigned int)config->relay[index].remember_state,
                           (unsigned int)config->relay[index].boot_default_on,
                           (unsigned int)config->current[index].adc_channel,
                           (unsigned int)config->current[index].protection_enabled,
                           (unsigned long)config->current[index].overcurrent_milliamps,
                           (unsigned long)config->current[index].trip_delay_ms,
                           (unsigned long)config->current[index].reset_hysteresis_milliamps,
                           (unsigned int)protection_state.above_threshold,
                           (unsigned int)protection_state.tripped,
                           (unsigned long)protection_state.trip_count,
                           (unsigned long)protection_state.last_trip_ms);
    if ((written < 0) || ((size_t)written >= (buffer_size - used)))
    {
      break;
    }
    used += (size_t)written;
  }

  if (used < buffer_size)
  {
    (void)snprintf(buffer + used, buffer_size - used, "]}");
  }

  (void)snprintf(log_message, sizeof(log_message), "[http] relays bytes=%lu\r\n",
                 (unsigned long)strlen(buffer));
  http_service_uart_write(log_message);
}

static void http_service_apply_command(control_proto_command_t *command, char *buffer, size_t buffer_size)
{
  if ((command == NULL) || (buffer == NULL))
  {
    return;
  }

  if (control_queue_submit_async(command) != HAL_OK)
  {
    g_http_service_stats.errors++;
    http_service_format_error_json(buffer, buffer_size, "relay_queue_full");
    return;
  }

  (void)snprintf(buffer, buffer_size, "{\"ok\":true,\"queued\":1}");
}

static void http_service_apply_remote_command(const cluster_node_t *node,
                                              control_proto_command_t *command,
                                              char *buffer,
                                              size_t buffer_size)
{
  if ((node == NULL) || (command == NULL) || (buffer == NULL))
  {
    return;
  }

  if (control_queue_submit_remote_async(node->ip_addr, node->node_id, command) != HAL_OK)
  {
    g_http_service_stats.errors++;
    http_service_format_error_json(buffer, buffer_size, "remote_command_failed");
    return;
  }

  (void)snprintf(buffer, buffer_size, "{\"ok\":true,\"queued\":1}");
}

__attribute__((noinline)) static err_t http_service_handle_request(http_service_connection_t *connection,
                                                                   const char *request)
{
  static char method[8];
  static char path[64];
  char *response_body = connection->response_buffer;
  const size_t response_body_size = HTTP_BODY_BUFFER_SIZE;
  const char *body;
  uint32_t value;
  uint8_t session_scope = 0xFFU;
  uint8_t authenticated = http_service_get_session_scope(request, &session_scope);
  uint8_t control_authenticated =
    (authenticated != 0U) && (session_scope == HTTP_AUTH_SCOPE_CONTROL);
  static control_proto_command_t command;
  static char password_text[32];
  static char service_key_text[DEVICE_CONFIG_BACKEND_KEY_MAX_LEN];
  static char device_name[DEVICE_CONFIG_NAME_MAX_LEN];
  static char cluster_name[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN];
  static char relay_name[DEVICE_CONFIG_NAME_MAX_LEN];
  static char master_ip_text[16];
  const cluster_node_t *remote_node;
  const device_config_t *active_config = device_config_get();
  uint32_t local_auth_tag = device_config_password_tag(active_config);
  static cluster_service_state_t cluster_state;

  if ((connection == NULL) || (connection->pcb == NULL) || (request == NULL))
  {
    return ERR_ARG;
  }

  if (sscanf(request, "%7s %63s", method, path) != 2)
  {
    http_service_format_error_json(response_body, response_body_size, "bad_request");
    return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
  }

  if (strcmp(method, "OPTIONS") == 0)
  {
    response_body[0] = '\0';
    return http_service_send_payload(connection, "204 No Content", "text/plain", response_body);
  }

  body = strstr(request, "\r\n\r\n");
  if (body != NULL)
  {
    body += 4;
  }
  else
  {
    body = "";
  }

  cluster_service_get_state(&cluster_state);

  if ((strcmp(method, "GET") == 0) &&
      ((strcmp(path, "/") == 0) || (strcmp(path, "/index.html") == 0)))
  {
    return http_service_send_payload_streaming(connection, "200 OK", "text/html; charset=utf-8", WEB_INDEX_HTML, (size_t)WEB_INDEX_HTML_LEN);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/session") == 0))
  {
    http_service_format_session_json(response_body, response_body_size, authenticated);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/relay") == 0))
  {
    http_service_format_relay_json(response_body, response_body_size, board_relay_snapshot());
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/config/cluster") == 0))
  {
    static char cfg_log[96];

    if (!authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    http_service_format_cluster_config_json(response_body, response_body_size);
    (void)snprintf(cfg_log, sizeof(cfg_log), "[http] cfg len=%lu\r\n",
                   (unsigned long)strlen(response_body));
    http_service_uart_write(cfg_log);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/cluster/nodes") == 0))
  {
    http_service_format_cluster_nodes_json(response_body, response_body_size);
    return http_service_send_payload_streaming(
      connection,
      "200 OK",
      "application/json",
      (const unsigned char *)response_body,
      strlen(response_body));
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/config/relays") == 0))
  {
    if (!authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    http_service_format_relay_config_json(response_body, response_body_size);
    return http_service_send_payload_streaming(
      connection,
      "200 OK",
      "application/json",
      (const unsigned char *)response_body,
      strlen(response_body));
  }


  /* ── Relay protection API ─────────────────────────────────── */
  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/protection") == 0))
  {
    size_t pos;
    int w; uint8_t ch;
    relay_protection_global_summary_t gs;
    relay_protection_get_global_summary(&gs);

    pos = (size_t)snprintf(response_body, response_body_size,
      "{\"ok\":true,\"boot_safety_active\":%u,\"boot_delay_until_ms\":%lu,"
      "\"restart_count\":%lu,\"fault_locked_any\":%u,\"fault_lock_mask\":%lu,"
      "\"channels\":[",
      (unsigned int)gs.boot_safety_active,
      (unsigned long)gs.boot_delay_until_ms,
      (unsigned long)gs.restart_count,
      (unsigned int)gs.fault_locked_any,
      (unsigned long)relay_protection_fault_lock_mask());
    for (ch = 0U; ch < 16U; ++ch)
    {
      relay_protection_summary_t ps;
      (void)relay_protection_get_summary((uint8_t)(ch + 1U), &ps);
      w = snprintf(response_body + pos, response_body_size - pos,
        "%s{\"ch\":%u,\"phase\":%u,\"fault_level\":%u,\"fault_reason\":%u,\"learning_complete\":%u,"
        "\"protection_enabled\":%u,\"tripped\":%u,\"fault_locked\":%u,"
        "\"current_ma\":%lu,\"reference_ma\":%lu,\"baseline_ma\":%lu,"
        "\"aging_offset_ma\":%lu,\"normal_low_ma\":%lu,\"normal_high_ma\":%lu,"
        "\"startup_peak_ma\":%lu,\"startup_duration_ms\":%lu,"
        "\"trip_count\":%lu,\"cooldown_until_ms\":%lu,"
        "\"learning_samples\":%u,\"ct_degraded\":%u,\"adc_saturated\":%u,"
        "\"device_strategy\":%u,\"off_since_ms\":%lu,\"stuck_pending\":%u}",
        (ch == 0U) ? "" : ",",
        (unsigned int)(ch + 1U),
        (unsigned int)ps.phase,
        (unsigned int)ps.fault_level,
        (unsigned int)ps.fault_reason,
        (unsigned int)ps.learning_complete,
        (unsigned int)ps.protection_enabled,
        (unsigned int)ps.tripped,
        (unsigned int)ps.fault_locked,
        (unsigned long)ps.current_milliamps,
        (unsigned long)ps.current_reference_ma,
        (unsigned long)ps.initial_health_baseline_ma,
        (unsigned long)ps.aging_offset_ma,
        (unsigned long)ps.normal_low_ma,
        (unsigned long)ps.normal_high_ma,
        (unsigned long)ps.startup_peak_ma,
        (unsigned long)ps.startup_duration_ms,
        (unsigned long)ps.trip_count,
        (unsigned long)ps.cooldown_until_ms,
        (unsigned int)ps.learning_samples_collected,
        (unsigned int)ps.ct_degraded,
        (unsigned int)ps.adc_saturated,
        (unsigned int)ps.device_strategy,
        (unsigned long)ps.off_since_ms,
        (unsigned int)ps.stuck_check_pending);
      if (w > 0 && (size_t)w < (response_body_size - pos)) pos += (size_t)w; else break;
    }
    (void)snprintf(response_body + pos, response_body_size - pos, "]}");
    return http_service_send_payload_streaming(connection, "200 OK", "application/json",
      (const unsigned char *)response_body, strlen(response_body));
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/protection/force-stop") == 0))
  {
    uint32_t channel;
    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "channel", &channel) ||
        channel == 0U || channel > 16U)
    {
      http_service_format_error_json(response_body, response_body_size, "bad_channel");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }
    relay_protection_force_stop((uint8_t)channel);
    (void)snprintf(response_body, response_body_size,
      "{\"ok\":true,\"force_stop\":%lu}", (unsigned long)channel);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/protection/clear-learning") == 0))
  {
    uint32_t channel;
    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "channel", &channel) ||
        channel == 0U || channel > 16U)
    {
      http_service_format_error_json(response_body, response_body_size, "bad_channel");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }
    relay_protection_clear_learning((uint8_t)channel);
    (void)snprintf(response_body, response_body_size,
      "{\"ok\":true,\"learning_cleared\":%lu}", (unsigned long)channel);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/protection/unlock") == 0))
  {
    uint32_t channel;
    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "channel", &channel) ||
        channel == 0U || channel > 16U)
    {
      http_service_format_error_json(response_body, response_body_size, "bad_channel");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }
    relay_protection_unlock((uint8_t)channel);
    (void)snprintf(response_body, response_body_size,
      "{\"ok\":true,\"unlocked\":%lu}", (unsigned long)channel);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/config/protection") == 0))
  {
    relay_protection_user_config_t cfg;
    uint32_t val;
    if (!control_authenticated) {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    relay_protection_get_user_config(&cfg);
    if (http_service_extract_form_value(body, "cooldown_seconds", &val)) cfg.cooldown_seconds = val;
    if (http_service_extract_form_value(body, "retry_delay_seconds", &val)) cfg.retry_delay_seconds = val;
    if (http_service_extract_form_value(body, "max_retry_count", &val)) cfg.max_retry_count = val;
    if (http_service_extract_form_value(body, "boot_delay_seconds", &val)) cfg.boot_delay_seconds = val;
    if (http_service_extract_form_value(body, "learning_samples", &val)) cfg.learning_samples = val;
    if (http_service_extract_form_value(body, "aging_update_hours", &val)) cfg.aging_update_hours = val;
    if (http_service_extract_form_value(body, "aging_cap_percent", &val)) cfg.aging_cap_percent = val;
    if (http_service_extract_form_value(body, "stall_multiplier_x10", &val)) cfg.stall_timeout_multiplier_x10 = val;
    relay_protection_set_user_config(&cfg);
    (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/config/protection") == 0))
  {
    relay_protection_user_config_t cfg;
    relay_protection_get_user_config(&cfg);
    (void)snprintf(response_body, response_body_size,
      "{\"ok\":true,\"cooldown_seconds\":%lu,\"retry_delay_seconds\":%lu,"
      "\"max_retry_count\":%lu,\"boot_delay_seconds\":%lu,"
      "\"learning_samples\":%lu,\"aging_update_hours\":%lu,"
      "\"aging_cap_percent\":%lu,\"stall_multiplier_x10\":%lu}",
      (unsigned long)cfg.cooldown_seconds, (unsigned long)cfg.retry_delay_seconds,
      (unsigned long)cfg.max_retry_count, (unsigned long)cfg.boot_delay_seconds,
      (unsigned long)cfg.learning_samples, (unsigned long)cfg.aging_update_hours,
      (unsigned long)cfg.aging_cap_percent, (unsigned long)cfg.stall_timeout_multiplier_x10);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/protection/strategy") == 0))
  {
    uint32_t channel, strategy;
    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "channel", &channel) ||
        channel == 0U || channel > 16U)
    {
      http_service_format_error_json(response_body, response_body_size, "bad_channel");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "strategy", &strategy) || strategy > 1U)
    {
      http_service_format_error_json(response_body, response_body_size, "bad_strategy");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }
    relay_protection_set_strategy((uint8_t)channel, (uint8_t)strategy);
    (void)snprintf(response_body, response_body_size,
      "{\"ok\":true,\"channel\":%lu,\"strategy\":%lu}", (unsigned long)channel, (unsigned long)strategy);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/login") == 0))
  {
    uint32_t session_token;
    uint8_t session_scope = HTTP_AUTH_SCOPE_CONTROL;

    if (!http_service_extract_form_text(body, "password", password_text, sizeof(password_text)) ||
        (strcmp(password_text, HTTP_AUTH_PASSWORD) != 0))
    {
      g_http_auth_state.login_failures++;
      http_service_format_error_json(response_body, response_body_size, "bad_password");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    if (http_service_extract_form_text(body, "scope", password_text, sizeof(password_text)) &&
        (strcmp(password_text, "service") == 0))
    {
      if (!http_service_extract_form_text(body,
                                          "service_key",
                                          service_key_text,
                                          sizeof(service_key_text)) ||
          (active_config->backend_key[0] == '\0') ||
          (strcmp(service_key_text, active_config->backend_key) != 0))
      {
        g_http_auth_state.login_failures++;
        http_service_format_error_json(response_body, response_body_size, "bad_service_key");
        return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
      }
      session_scope = HTTP_AUTH_SCOPE_SERVICE;
    }
    session_token = http_service_create_session(session_scope);
    http_service_format_login_json(response_body, response_body_size, session_token);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/logout") == 0))
  {
    http_service_revoke_session(request);
    http_service_format_logout_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/relay-mask") == 0))
  {
    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_is_master_control_allowed(master_ip_text, sizeof(master_ip_text)))
    {
      (void)snprintf(response_body, response_body_size,
                     "{\"ok\":false,\"error\":\"not_master\",\"master_ip\":\"%s\"}", master_ip_text);
      return http_service_send_payload(connection, "409 Conflict", "application/json", response_body);
    }

    memset(&command, 0, sizeof(command));
    command.type = CONTROL_PROTO_CMD_SET_RELAY_MASK;
    command.error = CONTROL_PROTO_ERR_NONE;

    if (!http_service_extract_form_value(body, "mask", &value) || (value > BOARD_RELAY_ALL_MASK))
    {
      http_service_format_error_json(response_body, response_body_size, "bad_mask");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    command.mask = value;
    http_service_apply_command(&command, response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/relay-channel") == 0))
  {
    uint32_t channel;
    uint32_t relay_value;

    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_is_master_control_allowed(master_ip_text, sizeof(master_ip_text)))
    {
      (void)snprintf(response_body, response_body_size,
                     "{\"ok\":false,\"error\":\"not_master\",\"master_ip\":\"%s\"}", master_ip_text);
      return http_service_send_payload(connection, "409 Conflict", "application/json", response_body);
    }

    memset(&command, 0, sizeof(command));
    command.type = CONTROL_PROTO_CMD_SET_RELAY;
    command.error = CONTROL_PROTO_ERR_NONE;

    if (!http_service_extract_form_value(body, "channel", &channel) ||
        !http_service_extract_form_value(body, "value", &relay_value) ||
        (channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT) || (relay_value > 1U))
    {
      http_service_format_error_json(response_body, response_body_size, "bad_channel_or_value");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    command.channel = (uint8_t)channel;
    command.value = (uint8_t)relay_value;
    http_service_apply_command(&command, response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/cluster/node-relay") == 0))
  {
    uint32_t channel;
    uint32_t relay_value;
    uint32_t node_id;

    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_is_master_control_allowed(master_ip_text, sizeof(master_ip_text)))
    {
      (void)snprintf(response_body, response_body_size,
                     "{\"ok\":false,\"error\":\"not_master\",\"master_ip\":\"%s\"}", master_ip_text);
      return http_service_send_payload(connection, "409 Conflict", "application/json", response_body);
    }

    memset(&command, 0, sizeof(command));
    command.type = CONTROL_PROTO_CMD_SET_RELAY;
    command.error = CONTROL_PROTO_ERR_NONE;

    if (!http_service_extract_form_value(body, "node_id", &node_id) ||
        !http_service_extract_form_value(body, "channel", &channel) ||
        !http_service_extract_form_value(body, "value", &relay_value) ||
        (channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT) || (relay_value > 1U))
    {
      http_service_format_error_json(response_body, response_body_size, "bad_remote_request");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    remote_node = cluster_service_find_node(node_id);
    if ((remote_node == NULL) ||
        (strcmp(remote_node->cluster_name, active_config->cluster_name) != 0))
    {
      http_service_format_error_json(response_body, response_body_size, "remote_node_unavailable");
      return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
    }
    {
      uint32_t expected_tag =
        device_config_compute_auth_tag(active_config->cluster_password_hash,
                                       remote_node->unix_time,
                                       remote_node->node_id,
                                       remote_node->cluster_name);
      if (remote_node->auth_tag != expected_tag)
      {
        http_service_format_error_json(response_body, response_body_size, "remote_node_unavailable");
        return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
      }
    }

    command.channel = (uint8_t)channel;
    command.value = (uint8_t)relay_value;
    http_service_apply_remote_command(remote_node, &command, response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/config/cluster") == 0))
  {
    static device_config_t config;
    const device_config_t *current_config = device_config_get();
    static char backend_host[DEVICE_CONFIG_BACKEND_HOST_MAX_LEN];
    static char backend_port_text[16];
    static char backend_key[DEVICE_CONFIG_BACKEND_KEY_MAX_LEN];
    static char cloud_broker_host[DEVICE_CONFIG_CLOUD_HOST_MAX_LEN];
    static char cloud_broker_port_text[16];
    static char cloud_cluster_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
    static char cloud_device_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
    static char cloud_key_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
    static char cloud_cluster_ca_fingerprint[DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN];
    static char cloud_cluster_binding_secret[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN];
    uint8_t cluster_password_supplied = 0U;
    uint8_t cluster_identity_changed = 0U;
    uint8_t cluster_runtime_changed = 0U;
    uint8_t cluster_cloud_sync_only = 0U;

    if (!authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    config = *current_config;

    /* Decrypt config-sync body if the master encrypted it with the
       cluster password.  The body starts with _enc=1&_data=<base64>. */
    if ((strncmp(body, "_enc=1&_data=", 13) == 0) &&
        (current_config->cluster_password_hash[0] != 0U))
    {
      static char enc_body[2048];
      static char plain_body[2048];
      const char *data_ptr = body + 13;
      size_t di = 0U;
      while (*data_ptr && *data_ptr != '&' && di < sizeof(enc_body))
      {
        enc_body[di++] = *data_ptr++;
      }
      enc_body[di] = '\0';
      /* Base64url decode */
      {
        uint32_t acc = 0U; uint8_t bits = 0U; size_t out_len = 0U;
        for (size_t si = 0U; si < di && out_len < sizeof(enc_body); ++si)
        {
          char c = enc_body[si];
          int val = -1;
          if (c >= 'A' && c <= 'Z') val = c - 'A';
          else if (c >= 'a' && c <= 'z') val = c - 'a' + 26;
          else if (c >= '0' && c <= '9') val = c - '0' + 52;
          else if (c == '-') val = 62;
          else if (c == '_') val = 63;
          if (val < 0) continue;
          acc = (acc << 6) | (uint32_t)val;
          bits += 6;
          if (bits >= 8)
          {
            bits -= 8;
            enc_body[out_len++] = (uint8_t)(acc >> bits);
            acc &= (1U << bits) - 1U;
          }
        }
        di = out_len;
      }
      {
        uint8_t key[16];
        size_t plain_len;
        memcpy(key, current_config->cluster_password_hash, sizeof(key));
        plain_len = control_queue_decrypt_body(enc_body, di, key,
                                               plain_body, sizeof(plain_body));
        if (plain_len > 0U && plain_len < sizeof(plain_body))
        {
          plain_body[plain_len] = '\0';
          body = plain_body;
        }
      }
    }

    if (http_service_extract_form_text(body, "device_name", device_name, sizeof(device_name)))
    {
      memset(config.device_name, 0, sizeof(config.device_name));
      strncpy(config.device_name, device_name, sizeof(config.device_name) - 1U);
    }

    if (http_service_extract_form_text(body, "cluster_name", cluster_name, sizeof(cluster_name)))
    {
      memset(config.cluster_name, 0, sizeof(config.cluster_name));
      strncpy(config.cluster_name, cluster_name, sizeof(config.cluster_name) - 1U);
      /* Keep cloud_cluster_id in sync with cluster_name */
      memset(config.cloud_cluster_id, 0, sizeof(config.cloud_cluster_id));
      strncpy(config.cloud_cluster_id, cluster_name, sizeof(config.cloud_cluster_id) - 1U);
    }

    if (http_service_extract_form_value(body, "cluster_enabled", &value) && (value <= 1U))
    {
      config.cluster_enabled = (uint8_t)value;
    }

    if (http_service_extract_form_value(body, "auto_join_cluster", &value) && (value <= 1U))
    {
      config.auto_join_cluster = (uint8_t)value;
    }

    if (http_service_extract_form_text(body, "cluster_password", password_text, sizeof(password_text)))
    {
      cluster_password_supplied = 1U;
      if (password_text[0] != '\0')
      {
        device_config_hash_password_text(password_text, config.cluster_password_hash);
      }
      else
      {
        memset(config.cluster_password_hash, 0, sizeof(config.cluster_password_hash));
      }
    }

    if (http_service_extract_form_text(body, "backend_host", backend_host, sizeof(backend_host)))
    {
      memset(config.backend_host, 0, sizeof(config.backend_host));
      strncpy(config.backend_host, backend_host, sizeof(config.backend_host) - 1U);
    }

    if (http_service_extract_form_text(body, "backend_port", backend_port_text, sizeof(backend_port_text)))
    {
      uint32_t backend_port_value = 0U;
      if (http_service_parse_u32_value(backend_port_text, &backend_port_value) &&
          (backend_port_value <= 65535U))
      {
        config.backend_port = (uint16_t)backend_port_value;
      }
    }

    if (http_service_extract_form_text(body, "backend_key", backend_key, sizeof(backend_key)))
    {
      memset(config.backend_key, 0, sizeof(config.backend_key));
      strncpy(config.backend_key, backend_key, sizeof(config.backend_key) - 1U);
    }

    if (http_service_extract_form_value(body, "cloud_enabled", &value) && (value <= 1U))
    {
      config.cloud_enabled = (uint8_t)value;
    }

    if (http_service_extract_form_value(body, "cloud_transport", &value) && (value <= 255U))
    {
      config.cloud_transport = (uint8_t)value;
    }

    if (http_service_extract_form_value(body, "cloud_cluster_sync_only", &value) && (value <= 1U))
    {
      cluster_cloud_sync_only = (uint8_t)value;
    }

    if (http_service_extract_form_text(body, "cloud_broker_host", cloud_broker_host, sizeof(cloud_broker_host)))
    {
      memset(config.cloud_broker_host, 0, sizeof(config.cloud_broker_host));
      strncpy(config.cloud_broker_host, cloud_broker_host, sizeof(config.cloud_broker_host) - 1U);
    }

    if (http_service_extract_form_text(body, "cloud_broker_port", cloud_broker_port_text, sizeof(cloud_broker_port_text)))
    {
      uint32_t cloud_broker_port_value = 0U;
      if (http_service_parse_u32_value(cloud_broker_port_text, &cloud_broker_port_value) &&
          (cloud_broker_port_value <= 65535U))
      {
        config.cloud_broker_port = (uint16_t)cloud_broker_port_value;
      }
    }

    /* cloud_cluster_id / cloud_device_id / cloud_key_id are read-only —
       they are auto-generated from the MCU UID at factory-defaults time
       and the cluster name.  The user cannot override them. */

    if (http_service_extract_form_text(body, "cloud_cluster_ca_fingerprint", cloud_cluster_ca_fingerprint, sizeof(cloud_cluster_ca_fingerprint)))
    {
      memset(config.cloud_cluster_ca_fingerprint, 0, sizeof(config.cloud_cluster_ca_fingerprint));
      strncpy(config.cloud_cluster_ca_fingerprint, cloud_cluster_ca_fingerprint, sizeof(config.cloud_cluster_ca_fingerprint) - 1U);
    }

    if (http_service_extract_form_text(body, "cloud_cluster_binding_secret", cloud_cluster_binding_secret, sizeof(cloud_cluster_binding_secret)))
    {
      memset(config.cloud_cluster_binding_secret, 0, sizeof(config.cloud_cluster_binding_secret));
      strncpy(config.cloud_cluster_binding_secret, cloud_cluster_binding_secret, sizeof(config.cloud_cluster_binding_secret) - 1U);
    }

    {
      static char cloud_cluster_ca_cert_body[DEVICE_CONFIG_CA_CERT_MAX_LEN];
      if (http_service_extract_form_text(body, "cloud_cluster_ca_cert", cloud_cluster_ca_cert_body, sizeof(cloud_cluster_ca_cert_body)))
      {
        memset(config.cloud_cluster_ca_cert, 0, sizeof(config.cloud_cluster_ca_cert));
        strncpy(config.cloud_cluster_ca_cert, cloud_cluster_ca_cert_body, sizeof(config.cloud_cluster_ca_cert) - 1U);
      }
    }

    if (config.device_name[0] == '\0')
    {
      http_service_format_error_json(response_body, response_body_size, "bad_device_name");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    if ((config.cluster_enabled != 0U) && (config.cluster_name[0] == '\0'))
    {
      http_service_format_error_json(response_body, response_body_size, "bad_cluster_name");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    if (strncmp(config.cluster_name,
                current_config->cluster_name,
                sizeof(config.cluster_name)) != 0)
    {
      cluster_identity_changed = 1U;
    }

    if ((cluster_password_supplied != 0U) &&
        (memcmp(config.cluster_password_hash,
                current_config->cluster_password_hash,
                sizeof(config.cluster_password_hash)) != 0))
    {
      cluster_identity_changed = 1U;
    }

    if ((config.cluster_enabled == 0U) || (cluster_identity_changed != 0U))
    {
      memset(config.backend_key, 0, sizeof(config.backend_key));
    }

    if ((config.cluster_enabled != current_config->cluster_enabled) ||
        (config.auto_join_cluster != current_config->auto_join_cluster) ||
        (strncmp(config.cluster_name,
                 current_config->cluster_name,
                 sizeof(config.cluster_name)) != 0) ||
        ((cluster_password_supplied != 0U) &&
         (memcmp(config.cluster_password_hash,
                 current_config->cluster_password_hash,
                 sizeof(config.cluster_password_hash)) != 0)))
    {
      cluster_runtime_changed = 1U;
    }

    if (config.backend_host[0] == '\0')
    {
      http_service_format_error_json(response_body, response_body_size, "bad_backend_host");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    if (config.backend_port == 0U)
    {
      http_service_format_error_json(response_body, response_body_size, "bad_backend_port");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    if (config.cloud_enabled != 0U)
    {
      if (config.cloud_broker_host[0] == '\0')
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_broker_host");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }

      if (config.cloud_broker_port == 0U)
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_broker_port");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }

      if (config.cloud_cluster_id[0] == '\0')
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_cluster_id");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }

      if ((cluster_cloud_sync_only == 0U) && (config.cloud_device_id[0] == '\0'))
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_device_id");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }

      if ((cluster_cloud_sync_only == 0U) && (config.cloud_key_id[0] == '\0'))
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_key_id");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }

      if (config.cloud_cluster_ca_fingerprint[0] == '\0')
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_cluster_ca_fingerprint");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }

      if (config.cloud_cluster_binding_secret[0] == '\0')
      {
        http_service_format_error_json(response_body, response_body_size, "bad_cloud_cluster_binding_secret");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }
    }

    if ((memcmp(&config, current_config, sizeof(config)) != 0) &&
        (device_config_update_and_save(&config) == 0U))
    {
      (void)snprintf(response_body,
                     response_body_size,
                     "{\"ok\":false,\"error\":\"config_save_failed\",\"flash_error\":%lu}",
                     (unsigned long)device_config_last_flash_error());
      return http_service_send_payload(connection,
                                       "500 Internal Server Error",
                                       "application/json",
                                       response_body);
    }

    if (cluster_runtime_changed != 0U)
    {
      cluster_service_reload_from_config();
      discovery_service_force_announce();
    }
    http_service_request_backend_sync();
    http_service_format_cluster_config_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/config/relay") == 0))
  {
    static device_config_t config;
    uint32_t channel;

    if (!authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "channel", &channel) ||
        (channel == 0U) || (channel > DEVICE_CONFIG_RELAY_COUNT))
    {
      http_service_format_error_json(response_body, response_body_size, "bad_channel");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    config = *device_config_get();
    if (http_service_extract_form_text(body, "name", relay_name, sizeof(relay_name)))
    {
      memset(config.relay[channel - 1U].name, 0, sizeof(config.relay[channel - 1U].name));
      strncpy(config.relay[channel - 1U].name, relay_name, sizeof(config.relay[channel - 1U].name) - 1U);
    }
    if (http_service_extract_form_value(body, "enabled", &value) && (value <= 1U))
    {
      config.relay[channel - 1U].enabled = (uint8_t)value;
    }
    if (http_service_extract_form_value(body, "remember_state", &value) && (value <= 1U))
    {
      config.relay[channel - 1U].remember_state = (uint8_t)value;
    }
    if (http_service_extract_form_value(body, "boot_default_on", &value) && (value <= 1U))
    {
      config.relay[channel - 1U].boot_default_on = (uint8_t)value;
    }
    if (http_service_extract_form_value(body, "adc_channel", &value))
    {
      if ((value >= ADC_SERVICE_CHANNEL_COUNT) &&
          (value != DEVICE_CONFIG_ADC_CHANNEL_UNASSIGNED))
      {
        http_service_format_error_json(response_body, response_body_size,
                                       "bad_adc_channel");
        return http_service_send_payload(connection, "400 Bad Request",
                                         "application/json", response_body);
      }
      config.current[channel - 1U].adc_channel = (uint8_t)value;
    }
    if (http_service_extract_form_value(body, "protection_enabled", &value))
    {
      if (value > 1U)
      {
        http_service_format_error_json(response_body, response_body_size,
                                       "bad_protection_enabled");
        return http_service_send_payload(connection, "400 Bad Request",
                                         "application/json", response_body);
      }
      config.current[channel - 1U].protection_enabled = (uint8_t)value;
    }
    if (http_service_extract_form_value(body, "overcurrent_milliamps", &value))
    {
      if ((value < 100U) || (value > 100000U))
      {
        http_service_format_error_json(response_body, response_body_size,
                                       "bad_overcurrent_threshold");
        return http_service_send_payload(connection, "400 Bad Request",
                                         "application/json", response_body);
      }
      config.current[channel - 1U].overcurrent_milliamps = value;
    }
    if (http_service_extract_form_value(body, "trip_delay_ms", &value))
    {
      if (value > 600000U)
      {
        http_service_format_error_json(response_body, response_body_size,
                                       "bad_trip_delay");
        return http_service_send_payload(connection, "400 Bad Request",
                                         "application/json", response_body);
      }
      config.current[channel - 1U].trip_delay_ms = value;
    }
    if (http_service_extract_form_value(body, "reset_hysteresis_milliamps", &value))
    {
      if (value > 100000U)
      {
        http_service_format_error_json(response_body, response_body_size,
                                       "bad_reset_hysteresis");
        return http_service_send_payload(connection, "400 Bad Request",
                                         "application/json", response_body);
      }
      config.current[channel - 1U].reset_hysteresis_milliamps = value;
    }

    if (config.relay[channel - 1U].name[0] == '\0')
    {
      http_service_format_error_json(response_body, response_body_size, "bad_relay_name");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }
    if ((config.current[channel - 1U].protection_enabled != 0U) &&
        (config.current[channel - 1U].adc_channel ==
         DEVICE_CONFIG_ADC_CHANNEL_UNASSIGNED))
    {
      http_service_format_error_json(response_body, response_body_size,
                                     "protection_requires_adc_channel");
      return http_service_send_payload(connection, "400 Bad Request",
                                       "application/json", response_body);
    }
    if (config.current[channel - 1U].reset_hysteresis_milliamps >=
        config.current[channel - 1U].overcurrent_milliamps)
    {
      http_service_format_error_json(response_body, response_body_size,
                                     "hysteresis_must_be_below_threshold");
      return http_service_send_payload(connection, "400 Bad Request",
                                       "application/json", response_body);
    }

    if (device_config_update_and_save(&config) == 0U)
    {
      (void)snprintf(response_body,
                     response_body_size,
                     "{\"ok\":false,\"error\":\"config_save_failed\",\"flash_error\":%lu}",
                     (unsigned long)device_config_last_flash_error());
      return http_service_send_payload(connection, "500 Internal Server Error", "application/json", response_body);
    }

    discovery_service_force_announce();
    http_service_format_relay_config_json(response_body, response_body_size);
    return http_service_send_payload_streaming(
      connection,
      "200 OK",
      "application/json",
      (const unsigned char *)response_body,
      strlen(response_body));
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/cluster/kick") == 0))
  {
    uint32_t node_id;

    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (cluster_state.role != CLUSTER_ROLE_MASTER)
    {
      (void)snprintf(response_body, response_body_size,
                     "{\"ok\":false,\"error\":\"not_master\",\"master_ip\":\"%s\"}", master_ip_text);
      return http_service_send_payload(connection, "409 Conflict", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "node_id", &node_id) ||
        (cluster_service_kick_node(node_id) == 0U))
    {
      http_service_format_error_json(response_body, response_body_size, "kick_failed");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    http_service_format_cluster_nodes_json(response_body, response_body_size);
    return http_service_send_payload_streaming(
      connection,
      "200 OK",
      "application/json",
      (const unsigned char *)response_body,
      strlen(response_body));
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/cluster/forget") == 0))
  {
    uint32_t node_id;

    if (!control_authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }
    if (cluster_state.role != CLUSTER_ROLE_MASTER)
    {
      (void)snprintf(response_body, response_body_size,
                     "{\"ok\":false,\"error\":\"not_master\",\"master_ip\":\"%s\"}", master_ip_text);
      return http_service_send_payload(connection, "409 Conflict", "application/json", response_body);
    }
    if (!http_service_extract_form_value(body, "node_id", &node_id) ||
        (cluster_service_remove_node(node_id) == 0U))
    {
      http_service_format_error_json(response_body, response_body_size, "forget_failed");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    http_service_format_cluster_nodes_json(response_body, response_body_size);
    return http_service_send_payload_streaming(
      connection,
      "200 OK",
      "application/json",
      (const unsigned char *)response_body,
      strlen(response_body));
  }

  /* ═══════════════════ Scheduler API (§22) ═══════════════════ */

  if (strncmp(path, "/api/relay/channels/", 20) == 0) {
    const char *rest = path + 20;
    uint32_t channel = 0;
    while (*rest >= '0' && *rest <= '9') {
      channel = channel * 10 + (uint32_t)(*rest - '0');
      rest++;
    }
    if (channel == 0 || channel > 16) {
      http_service_format_error_json(response_body, response_body_size, "bad_channel");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    /* ── 22.1 GET /api/relay/channels/:channel/schedule-week ── */
    if ((strcmp(method, "GET") == 0) && (strcmp(rest, "/schedule-week") == 0)) {
      weekly_schedule_t *ws = scheduler_get_weekly((uint8_t)channel);
      if (!ws) {
        http_service_format_error_json(response_body, response_body_size, "not_found");
        return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
      }
      int w = snprintf(response_body, response_body_size,
        "{\"ok\":true,\"channel\":%lu,\"schedule_enabled\":%u,\"days\":[",
        (unsigned long)channel,
        (unsigned int)(scheduler_get_state((uint8_t)channel) ?
                       scheduler_get_state((uint8_t)channel)->schedule_enabled : 0));
      for (uint8_t d = 0; d < 7; d++) {
        daily_schedule_t *day = &ws->days[d];
        w += snprintf(response_body + w, response_body_size - w,
          "%s{\"weekday\":%u,\"intervals\":[",
          d > 0 ? "," : "", (unsigned int)d);
        for (uint8_t k = 0; k < day->interval_count; k++) {
          w += snprintf(response_body + w, response_body_size - w,
            "%s{\"start_minute\":%u,\"end_minute\":%u}",
            k > 0 ? "," : "",
            (unsigned int)day->intervals[k].start_minute,
            (unsigned int)day->intervals[k].end_minute);
        }
        w += snprintf(response_body + w, response_body_size - w, "]}");
      }
      w += snprintf(response_body + w, response_body_size - w, "]}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.2 PUT /api/relay/channels/:channel/schedule-week/:weekday ── */
    if ((strcmp(method, "PUT") == 0) && strncmp(rest, "/schedule-week/", 15) == 0) {
      uint32_t weekday = 0;
      const char *wp = rest + 15;
      while (*wp >= '0' && *wp <= '9') { weekday = weekday * 10 + (uint32_t)(*wp - '0'); wp++; }
      if (weekday > 6) {
        http_service_format_error_json(response_body, response_body_size, "bad_weekday");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }
      weekly_schedule_t *ws = scheduler_get_weekly((uint8_t)channel);
      if (!ws) {
        http_service_format_error_json(response_body, response_body_size, "not_found");
        return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
      }
      /* Parse intervals from form body: count=N & s0=480 & e0=720 & s1=840 & e1=1080 */
      uint32_t count = 0;
      http_service_extract_form_value(body, "count", &count);
      if (count > SCHEDULER_MAX_INTERVALS) count = SCHEDULER_MAX_INTERVALS;
      daily_schedule_t new_day;
      memset(&new_day, 0, sizeof(new_day));
      new_day.interval_count = (uint8_t)count;
      char key[8];
      for (uint32_t k = 0; k < count; k++) {
        uint32_t sv = 0, ev = 0;
        snprintf(key, sizeof(key), "s%lu", (unsigned long)k);
        http_service_extract_form_value(body, key, &sv);
        snprintf(key, sizeof(key), "e%lu", (unsigned long)k);
        http_service_extract_form_value(body, key, &ev);
        if (sv >= 1440 || ev == 0 || ev > 1440 || sv >= ev) {
          http_service_format_error_json(response_body, response_body_size, "bad_interval");
          return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
        }
        new_day.intervals[k].start_minute = (uint16_t)sv;
        new_day.intervals[k].end_minute = (uint16_t)ev;
      }
      if (!scheduler_validate_intervals(&new_day)) {
        http_service_format_error_json(response_body, response_body_size, "schedule_overlap");
        return http_service_send_payload(connection, "409 Conflict", "application/json", response_body);
      }
      ws->days[(uint8_t)weekday] = new_day;
      scheduler_mark_flash_dirty(); scheduler_mark_rtc_dirty();
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.3 POST /api/relay/channels/:channel/schedule-week/:weekday/copy ── */
    if ((strcmp(method, "POST") == 0) && strstr(rest, "/schedule-week/") && strstr(rest, "/copy")) {
      uint32_t src_wd = 0;
      const char *wp = rest + 15;
      while (*wp >= '0' && *wp <= '9') { src_wd = src_wd * 10 + (uint32_t)(*wp - '0'); wp++; }
      if (src_wd > 6) {
        http_service_format_error_json(response_body, response_body_size, "bad_weekday");
        return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
      }
      weekly_schedule_t *ws = scheduler_get_weekly((uint8_t)channel);
      if (!ws) {
        http_service_format_error_json(response_body, response_body_size, "not_found");
        return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
      }
      daily_schedule_t src = ws->days[(uint8_t)src_wd];
      /* Parse target weekdays from form: targets=0,1,2,3 */
      char targets_buf[64];
      if (http_service_extract_form_text(body, "targets", targets_buf, sizeof(targets_buf))) {
        const char *t = targets_buf;
        while (*t) {
          while (*t == ',' || *t == ' ') t++;
          if (*t >= '0' && *t <= '9') {
            uint32_t tw = (uint32_t)(*t - '0');
            if (tw <= 6) ws->days[(uint8_t)tw] = src;
          scheduler_mark_flash_dirty(); scheduler_mark_rtc_dirty();
            while (*t >= '0' && *t <= '9') t++;
          } else break;
        }
      }
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.4 PATCH /api/relay/channels/:channel/scheduler/enabled ── */
    if ((strcmp(method, "PATCH") == 0) && (strcmp(rest, "/scheduler/enabled") == 0)) {
      uint32_t enabled = 0;
      http_service_extract_form_value(body, "enabled", &enabled);
      channel_scheduler_state_t *st = scheduler_get_state((uint8_t)channel);
      scheduler_mark_flash_dirty(); scheduler_mark_rtc_dirty();
      if (st) st->schedule_enabled = (uint8_t)(enabled ? 1 : 0);
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.5 POST /api/relay/channels/:channel/scheduler/pause ── */
    if ((strcmp(method, "POST") == 0) && (strcmp(rest, "/scheduler/pause") == 0)) {
      uint32_t mode = 0, dur = 0, until = 0, blocks = 0;
      http_service_extract_form_value(body, "mode", &mode);
      http_service_extract_form_value(body, "duration_seconds", &dur);
      http_service_extract_form_value(body, "until", &until);
      http_service_extract_form_value(body, "pause_blocks_auto_trigger", &blocks);
      channel_scheduler_state_t *st = scheduler_get_state((uint8_t)channel);
      if (!st) {
        http_service_format_error_json(response_body, response_body_size, "not_found");
        return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
      }
      uint8_t tv;
      uint32_t now_unix = cluster_service_get_unix_time(&tv);
      switch (mode) {
        case 0: st->schedule_pause_until = now_unix + 60; break;  /* current_segment_end simplified */
        case 1: st->schedule_pause_until = now_unix + dur; break;
        case 2: st->schedule_pause_until = until; break;
        case 3: st->schedule_pause_until = SCHEDULER_PAUSE_FOREVER; break;
        default: st->schedule_pause_until = now_unix + 60; break;
      }
      st->pause_blocks_auto_trigger = (uint8_t)(blocks ? 1 : 0);
      scheduler_mark_rtc_dirty();
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.6 POST /api/relay/channels/:channel/scheduler/resume ── */
    if ((strcmp(method, "POST") == 0) && (strcmp(rest, "/scheduler/resume") == 0)) {
      channel_scheduler_state_t *st = scheduler_get_state((uint8_t)channel);
      if (st) {
        st->schedule_pause_until = 0;
      scheduler_mark_rtc_dirty();
        st->pause_blocks_auto_trigger = 0;
      }
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.7 POST /api/relay/channels/:channel/control (user manual) ── */
    if ((strcmp(method, "POST") == 0) && (strcmp(rest, "/control") == 0)) {
      uint32_t target = 0, pmode = 0, pdur = 0, puntil = 0, pblocks = 0;
      http_service_extract_form_value(body, "target", &target);
      http_service_extract_form_value(body, "pause_mode", &pmode);
      http_service_extract_form_value(body, "pause_duration_seconds", &pdur);
      http_service_extract_form_value(body, "pause_until", &puntil);
      http_service_extract_form_value(body, "pause_blocks_auto_trigger", &pblocks);
      scheduler_user_control((uint8_t)channel, (uint8_t)(target ? 1 : 0),
                             (scheduler_pause_mode_t)pmode, pdur, puntil,
                             (uint8_t)(pblocks ? 1 : 0));
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.8 POST /api/relay/channels/:channel/auto-hold ── */
    if ((strcmp(method, "POST") == 0) && (strcmp(rest, "/auto-hold") == 0)) {
      auto_hold_cmd_t cmd;
      uint32_t target = 0, dur = 0, session = 0, cts = 0;
      memset(&cmd, 0, sizeof(cmd));
      cmd.channel = (uint8_t)channel;
      http_service_extract_form_value(body, "target", &target);   cmd.target = (uint8_t)(target ? 1 : 0);
      http_service_extract_form_value(body, "duration_seconds", &dur); cmd.duration_seconds = dur;
      http_service_extract_form_value(body, "session", &session); cmd.session = session;
      http_service_extract_form_value(body, "command_ts", &cts);  cmd.command_ts = cts;
      http_service_extract_form_text(body, "reason", cmd.reason, SCHEDULER_REASON_MAX_LEN + 1);
      { uint8_t tv2; scheduler_handle_auto_hold(&cmd, HAL_GetTick(), cluster_service_get_unix_time(&tv2)); }
      (void)snprintf(response_body, response_body_size, "{\"ok\":true}");
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* ── 22.9 GET /api/relay/channels/:channel/scheduler/status ── */
    if ((strcmp(method, "GET") == 0) && (strcmp(rest, "/scheduler/status") == 0)) {
      channel_scheduler_state_t *st = scheduler_get_state((uint8_t)channel);
      if (!st) {
        http_service_format_error_json(response_body, response_body_size, "not_found");
        return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
      }
      uint8_t tv; uint32_t now_unix = cluster_service_get_unix_time(&tv);
      uint32_t remain = 0;
      if (st->auto_hold.session != 0) {
        uint32_t now_ms = HAL_GetTick();
        remain = (st->auto_hold.until_ms > now_ms) ?
                 (st->auto_hold.until_ms - now_ms) / 1000U : 0;
      }
      (void)snprintf(response_body, response_body_size,
        "{\"ok\":true,\"channel\":%lu,"
        "\"schedule_enabled\":%u,"
        "\"pause_active\":%u,"
        "\"schedule_pause_until\":%lu,"
        "\"pause_blocks_auto_trigger\":%u,"
        "\"auto_hold_active\":%u,"
        "\"auto_hold_target\":%u,"
        "\"auto_hold_start_state\":%u,"
        "\"auto_hold_remaining_seconds\":%lu,"
        "\"auto_hold_session\":%lu,"
        "\"auto_hold_last_command_ts\":%lu,"
        "\"auto_hold_reason\":\"%s\","
        "\"time_valid\":%u}",
        (unsigned long)channel,
        (unsigned int)st->schedule_enabled,
        (unsigned int)(st->schedule_pause_until != 0 && now_unix < st->schedule_pause_until ? 1 : 0),
        (unsigned long)st->schedule_pause_until,
        (unsigned int)st->pause_blocks_auto_trigger,
        (unsigned int)(st->auto_hold.session != 0 ? 1 : 0),
        (unsigned int)st->auto_hold.target_state,
        (unsigned int)st->auto_hold.start_state,
        (unsigned long)remain,
        (unsigned long)st->auto_hold.session,
        (unsigned long)st->last_auto_hold_command_ts,
        st->auto_hold.reason,
        (unsigned int)tv);
      return http_service_send_payload(connection, "200 OK", "application/json", response_body);
    }

    /* fallthrough: unknown scheduler sub-path */
  }

  http_service_format_error_json(response_body, response_body_size, "not_found");
  return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
}

static err_t http_service_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;
  static char line[96];
  static char message[128];
  err_t request_err;
  u16_t copy_len;
  char *header_end;
  uint32_t content_length;
  uint32_t total_expected;

  if ((err != ERR_OK) || (p == NULL))
  {
    if (p != NULL)
    {
      pbuf_free(p);
    }
    http_service_close(tpcb, connection);
    return ERR_OK;
  }

  tcp_recved(tpcb, p->tot_len);
  if (connection == NULL)
  {
    pbuf_free(p);
    http_service_close(tpcb, connection);
    return ERR_OK;
  }
  connection->last_activity_ms = HAL_GetTick();

  if (connection->request_length >= (HTTP_REQUEST_BUFFER_SIZE - 1U))
  {
    pbuf_free(p);
    http_service_format_error_json(connection->response_buffer, HTTP_BODY_BUFFER_SIZE, "request_too_large");
    (void)http_service_send_payload(connection, "413 Payload Too Large", "application/json", connection->response_buffer);
    return ERR_OK;
  }

  copy_len = p->tot_len;
  if ((uint32_t)connection->request_length + (uint32_t)copy_len >= (HTTP_REQUEST_BUFFER_SIZE - 1U))
  {
    copy_len = (u16_t)((HTTP_REQUEST_BUFFER_SIZE - 1U) - connection->request_length);
  }
  pbuf_copy_partial(p, connection->request_buffer + connection->request_length, copy_len, 0U);
  connection->request_length = (uint16_t)(connection->request_length + copy_len);
  connection->request_buffer[connection->request_length] = '\0';
  pbuf_free(p);

  header_end = strstr(connection->request_buffer, "\r\n\r\n");
  if (header_end == NULL)
  {
    return ERR_OK;
  }

  if (connection->expected_length == 0U)
  {
    content_length = http_service_parse_content_length(connection->request_buffer);
    total_expected = (uint32_t)(header_end - connection->request_buffer) + 4U + content_length;
    connection->expected_length = (total_expected < HTTP_REQUEST_BUFFER_SIZE) ? (uint16_t)total_expected : (uint16_t)HTTP_REQUEST_BUFFER_SIZE;
  }

  if (connection->request_length < connection->expected_length)
  {
    return ERR_OK;
  }

  g_http_service_stats.requests++;
  if (sscanf(connection->request_buffer, "%95[^\r\n]", line) == 1)
  {
    (void)snprintf(message, sizeof(message), "[http] req %s\r\n", line);
    http_service_uart_write(message);
  }
  
  /* Reset chunk tracking for the new request, UNLESS a streaming
     response is already in progress (e.g. GET / is still pushing the
     large HTML page).  Clobbering send_data would silently kill the
     streaming and leave the browser waiting forever. */
  if (connection != NULL)
  {
     if (connection->send_data != NULL && connection->send_progress < connection->send_total)
     {
       /* Another streaming response is still in flight — silently
          drop the pipelined request.  The pbuf was already freed
          above; just return so the ongoing transfer can continue. */
       return ERR_OK;
     }
     connection->send_data = NULL;
     connection->send_total = 0;
     connection->send_progress = 0;
  }
  
  if (strncmp(connection->request_buffer, "GET /api/status ", 16U) == 0)
  {
    request_err = http_service_handle_status_request(connection);
  }
  else
  {
    request_err = http_service_handle_request(connection, connection->request_buffer);
  }
  if (request_err != ERR_OK)
  {
    http_service_log_error_value("[http] send err=", request_err);
    http_service_close(tpcb, connection);
    return ERR_OK;
  }
  
  if (connection != NULL && connection->send_data == NULL)
  {
    /* If we didn't start a chunked transfer, mark for close.
       http_service_send_payload sets this internally, but for safety: */
    connection->close_pending = 1U;
  }
  return ERR_OK;
}

static void http_service_error(void *arg, err_t err)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;
  char message[64];

  if ((err == ERR_CLSD) || (err == ERR_RST))
  {
    http_service_release_connection(connection);
    return;
  }

  g_http_service_stats.errors++;
  http_service_release_connection(connection);
  (void)snprintf(message, sizeof(message), "[http] err=%d active=%lu\r\n",
                 (int)err,
                 (unsigned long)http_service_active_connection_count());
  http_service_uart_write(message);
}

static err_t http_service_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;

  LWIP_UNUSED_ARG(len);
  if (connection != NULL)
  {
    connection->last_activity_ms = HAL_GetTick();
    if (connection->send_data != NULL && connection->send_progress < connection->send_total)
    {
       /* Continue sending chunks */
       http_service_send_payload_chunk(connection);
    }
    
    /* We check if all data has been completely sent AND acknowledged.
       If close_pending is set and we've queued all bytes, we close. */
    if (connection->close_pending != 0U)
    {
      if (connection->send_data == NULL || connection->send_progress >= connection->send_total)
      {
           if ((tcp_sndbuf(tpcb) == TCP_SND_BUF) && (tcp_sndqueuelen(tpcb) == 0U))
           {
               http_service_log_status("[http] sent close\r\n");
               http_service_close(tpcb, connection);
           }
       }
    }
  }
  return ERR_OK;
}

static err_t http_service_poll_close(void *arg, struct tcp_pcb *tpcb)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;

  if (connection != NULL)
  {
      if ((int32_t)(HAL_GetTick() - connection->last_activity_ms) >=
          (int32_t)HTTP_CONNECTION_IDLE_TIMEOUT_MS)
      {
        g_http_service_stats.errors++;
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_poll(tpcb, NULL, 0);
        tcp_sent(tpcb, NULL);
        tcp_abort(tpcb);
        http_service_release_connection(connection);
        http_service_log_status("[http] idle abort\r\n");
        return ERR_ABRT;
      }

      if (connection->send_data != NULL && connection->send_progress < connection->send_total)
      {
         (void)http_service_send_payload_chunk(connection);
      }

      if (connection->close_pending != 0U)
      {
         if (connection->send_data == NULL || connection->send_progress >= connection->send_total)
         {
             if ((tcp_sndbuf(tpcb) == TCP_SND_BUF) && (tcp_sndqueuelen(tpcb) == 0U))
             {
               http_service_log_status("[http] poll close\r\n");
               http_service_close(tpcb, connection);
             }
         }
      }
  }
  return ERR_OK;
}

static err_t http_service_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  http_service_connection_t *connection;
  char message[64];
  uint32_t active_count;

  LWIP_UNUSED_ARG(arg);

  if ((err != ERR_OK) || (newpcb == NULL))
  {
    g_http_service_stats.errors++;
    return ERR_VAL;
  }

  active_count = http_service_active_connection_count();
  if (active_count >= HTTP_CONN_POOL_SIZE)
  {
    g_http_service_stats.errors++;
    http_service_log_status("[http] accept busy\r\n");
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  connection = http_service_alloc_connection(newpcb);
  if (connection == NULL)
  {
    g_http_service_stats.errors++;
    http_service_log_status("[http] accept full\r\n");
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  g_http_service_stats.connections++;
  tcp_arg(newpcb, connection);
  tcp_recv(newpcb, http_service_recv);
  tcp_err(newpcb, http_service_error);
  tcp_sent(newpcb, http_service_sent);
  tcp_poll(newpcb, http_service_poll_close, 1);
  tcp_nagle_disable(newpcb);
  (void)snprintf(message, sizeof(message), "[http] accept active=%lu\r\n",
                 (unsigned long)http_service_active_connection_count());
  http_service_uart_write(message);
  return ERR_OK;
}

static void http_service_tcpip_step(void *arg)
{
  struct tcp_pcb *pcb;
  uint8_t link_up;

  LWIP_UNUSED_ARG(arg);
  link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
  if (!link_up)
  {
    if (g_http_last_link_up != 0U)
    {
      http_service_close_listener();
    }
    g_http_last_link_up = 0U;
    g_http_poll_pending = 0U;
    return;
  }

  if (g_http_listener_pcb == NULL)
  {
    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] pcb err\r\n");
      g_http_poll_pending = 0U;
      return;
    }

    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_TCP_PORT) != ERR_OK)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] bind err\r\n");
      tcp_abort(pcb);
      g_http_poll_pending = 0U;
      return;
    }

    g_http_listener_pcb = tcp_listen_with_backlog(pcb, 4);
    if (g_http_listener_pcb == NULL)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] listen err\r\n");
      tcp_abort(pcb);
      g_http_poll_pending = 0U;
      return;
    }

    tcp_accept(g_http_listener_pcb, http_service_accept);
    http_service_log_status("[http] up 80\r\n");
  }

  g_http_last_link_up = 1U;
  g_http_poll_pending = 0U;
}

void http_service_init(void)
{
  if (g_http_service_started != 0U)
  {
    return;
  }

  g_http_service_started = 1U;
  memset(&g_http_service_stats, 0, sizeof(g_http_service_stats));
  memset(&g_http_connections, 0, sizeof(g_http_connections));
  memset(&g_http_auth_state, 0, sizeof(g_http_auth_state));
}

void http_service_poll(void)
{
  err_t err;
  uint32_t now;
  uint32_t member_signature;

  http_service_init();
  now = HAL_GetTick();
  member_signature = http_service_backend_sync_member_signature();
  if (member_signature != g_http_backend_sync_member_signature)
  {
    g_http_backend_sync_member_signature = member_signature;
    http_service_request_backend_sync();
    g_http_backend_sync_last_ms = now;
  }
  else if ((int32_t)(now - g_http_backend_sync_last_ms) >=
           (int32_t)HTTP_BACKEND_SYNC_FALLBACK_MS)
  {
    http_service_request_backend_sync();
    g_http_backend_sync_last_ms = now;
  }
  if (g_http_backend_sync_active != 0U)
  {
    http_service_sync_backend_config_to_cluster_async();
  }
  if (g_http_poll_pending != 0U)
  {
    g_http_poll_skip_count++;
    if ((g_http_poll_skip_count % 50U) == 0U)
    {
      http_service_log_error_value("[http] poll skip=", (int)g_http_poll_skip_count);
    }
    return;
  }

  g_http_poll_pending = 1U;
  g_http_poll_skip_count = 0U;
  err = tcpip_try_callback(http_service_tcpip_step, NULL);
  if (err != ERR_OK)
  {
    g_http_poll_pending = 0U;
    http_service_log_error_value("[http] poll err=", err);
  }
}

void http_service_get_stats(http_service_stats_t *stats)
{
  uint32_t index;

  if (stats != NULL)
  {
    *stats = g_http_service_stats;
    stats->listener_up = (g_http_listener_pcb != NULL) ? 1U : 0U;
    stats->active_connections = 0U;
    stats->close_pending_connections = 0U;

    for (index = 0U; index < HTTP_CONN_POOL_SIZE; index++)
    {
      if (g_http_connections[index].active != 0U)
      {
        stats->active_connections++;
        if (g_http_connections[index].close_pending != 0U)
        {
          stats->close_pending_connections++;
        }
      }
    }
  }
}
