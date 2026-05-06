#include "http_service.h"

#include "cmsis_os.h"

#include "board_relay.h"
#include "control_proto.h"
#include "control_queue.h"
#include "control_service.h"
#include "discovery_service.h"
#include "relay_domain.h"

#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#include "FreeRTOS.h"
#include "portable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_TCP_PORT               80U
#define HTTP_CONN_POOL_SIZE         8U
#define HTTP_REQUEST_BUFFER_SIZE    768U
#define HTTP_BODY_BUFFER_SIZE       1024U
#define HTTP_AUTH_TOKEN_SIZE        16U
#define HTTP_AUTH_SESSION_TTL_MS    (15UL * 60UL * 1000UL)
#define HTTP_AUTH_PASSWORD          "admin"

extern struct netif gnetif;
extern UART_HandleTypeDef huart3;

typedef struct
{
  struct tcp_pcb *pcb;
  uint8_t active;
  uint8_t close_pending;
  
  /* State for progressive sending of large payloads */
  const unsigned char *send_data;
  size_t send_total;
  size_t send_progress;
} http_service_connection_t;

typedef struct
{
  uint32_t token;
  uint32_t expires_at_ms;
  uint32_t sessions_issued;
  uint32_t login_failures;
  uint8_t active;
} http_service_auth_state_t;

static struct tcp_pcb *g_http_listener_pcb = NULL;
static http_service_stats_t g_http_service_stats;
static http_service_connection_t g_http_connections[HTTP_CONN_POOL_SIZE];
static http_service_auth_state_t g_http_auth_state;
static uint8_t g_http_service_started = 0U;
static uint8_t g_http_last_link_up = 0U;

static void http_service_tcpip_step(void *arg);
static void http_service_uart_write(const char *text);
static void http_service_log_status(const char *text);
static void http_service_log_error_value(const char *prefix, int value);
static void http_service_close(struct tcp_pcb *tpcb, http_service_connection_t *connection);
static http_service_connection_t *http_service_alloc_connection(struct tcp_pcb *tpcb);
static void http_service_release_connection(http_service_connection_t *connection);
static err_t http_service_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t http_service_poll_close(void *arg, struct tcp_pcb *tpcb);
static uint8_t http_service_extract_form_text(const char *body, const char *key, char *buffer, size_t buffer_size);
static uint8_t http_service_extract_header_text(const char *request, const char *header_name, char *buffer, size_t buffer_size);
static uint8_t http_service_has_valid_session(const char *request);
static uint32_t http_service_issue_session_token(void);
static void http_service_format_session_json(char *buffer, size_t buffer_size, uint8_t authenticated);
static void http_service_format_login_json(char *buffer, size_t buffer_size);
static void http_service_format_logout_json(char *buffer, size_t buffer_size);

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
  }
}

static void http_service_close_listener(void)
{
  if (g_http_listener_pcb == NULL)
  {
    return;
  }

  tcp_arg(g_http_listener_pcb, NULL);
  tcp_accept(g_http_listener_pcb, NULL);
  (void)tcp_close(g_http_listener_pcb);
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

static uint8_t http_service_extract_form_value(const char *body, const char *key, uint32_t *value_out)
{
  const char *match;
  const char *value_text;
  char token[16];
  size_t index = 0U;

  if ((body == NULL) || (key == NULL) || (value_out == NULL))
  {
    return 0U;
  }

  match = strstr(body, key);
  if (match == NULL)
  {
    return 0U;
  }

  value_text = match + strlen(key);
  if (*value_text != '=')
  {
    return 0U;
  }
  value_text++;

  while ((*value_text != '\0') && (*value_text != '&') && (index < (sizeof(token) - 1U)))
  {
    token[index++] = *value_text++;
  }
  token[index] = '\0';

  return http_service_parse_u32_value(token, value_out);
}

static uint8_t http_service_extract_form_text(const char *body, const char *key, char *buffer, size_t buffer_size)
{
  const char *match;
  const char *value_text;
  size_t index = 0U;

  if ((body == NULL) || (key == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  match = strstr(body, key);
  if (match == NULL)
  {
    return 0U;
  }

  value_text = match + strlen(key);
  if (*value_text != '=')
  {
    return 0U;
  }
  value_text++;

  while ((*value_text != '\0') && (*value_text != '&') && (*value_text != '\r') &&
         (*value_text != '\n') && (index < (buffer_size - 1U)))
  {
    buffer[index++] = *value_text++;
  }
  buffer[index] = '\0';

  return (index > 0U) ? 1U : 0U;
}

static uint8_t http_service_extract_header_text(const char *request, const char *header_name, char *buffer, size_t buffer_size)
{
  const char *match;
  const char *value_text;
  size_t index = 0U;

  if ((request == NULL) || (header_name == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  match = strstr(request, header_name);
  if (match == NULL)
  {
    return 0U;
  }

  value_text = match + strlen(header_name);
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

static uint32_t http_service_issue_session_token(void)
{
  uint32_t seed = HAL_GetTick();

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

static uint8_t http_service_has_valid_session(const char *request)
{
  char token_text[HTTP_AUTH_TOKEN_SIZE];
  uint32_t token_value;

  if (g_http_auth_state.active == 0U)
  {
    return 0U;
  }

  if (HAL_GetTick() > g_http_auth_state.expires_at_ms)
  {
    g_http_auth_state.active = 0U;
    g_http_auth_state.token = 0U;
    return 0U;
  }

  if (!http_service_extract_header_text(request, "X-Auth-Token:", token_text, sizeof(token_text)))
  {
    return 0U;
  }

  if (!http_service_parse_u32_value(token_text, &token_value))
  {
    return 0U;
  }

  return (token_value == g_http_auth_state.token) ? 1U : 0U;
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

  send_len = (u16_t)(connection->send_total - connection->send_progress);
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
    return ERR_OK;
  }
  if (err != ERR_OK)
  {
    return err;
  }

  connection->send_progress += send_len;

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
  char header[256];
  int header_len;
  err_t err;

  if ((connection == NULL) || (connection->pcb == NULL) || (status_line == NULL) || (content_type == NULL) || (body == NULL))
  {
    return ERR_ARG;
  }

  header_len = snprintf(header, sizeof(header),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %u\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "\r\n",
                        status_line,
                        content_type,
                        (unsigned int)body_len);
  if ((header_len <= 0) || ((size_t)header_len >= sizeof(header)))
  {
    return ERR_BUF;
  }

  err = tcp_write(connection->pcb, header, (u16_t)header_len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    /* If the header fails to queue, we must abort the stream */
    return err;
  }
  
  connection->send_data = body;
  connection->send_total = body_len;
  connection->send_progress = 0;
  connection->close_pending = 0U; /* Clear flag just in case */

  (void)http_service_send_payload_chunk(connection);
  return ERR_OK; 
}

static err_t http_service_send_payload(http_service_connection_t *connection,
                                       const char *status_line,
                                       const char *content_type,
                                       const char *body)
{
   /* For small JSON payloads, we can copy the body into TCP buffers. */
  char header[256];
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
                        "Content-Length: %u\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "\r\n",
                        status_line,
                        content_type,
                        (unsigned int)body_len);
  if ((header_len <= 0) || ((size_t)header_len >= sizeof(header)))
  {
    return ERR_BUF;
  }

  err = tcp_write(tpcb, header, (u16_t)header_len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    return err;
  }

  err = tcp_write(tpcb, body, (u16_t)body_len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    return err;
  }

  /* Clear chunking state and mark close pending for short payloads */
  connection->send_data = NULL;
  connection->send_total = 0;
  connection->send_progress = 0;
  connection->close_pending = 1U;
  
  return tcp_output(tpcb);
}

static void http_service_format_status_json(char *buffer, size_t buffer_size)
{
  char ip_text[16];
  char mask_text[16];
  char gw_text[16];
  control_service_stats_t control_stats;
  discovery_service_stats_t discovery_stats;
  size_t heap_free = xPortGetFreeHeapSize();
  size_t heap_min = xPortGetMinimumEverFreeHeapSize();
  uint32_t http_active = 0U;
  uint32_t http_close_pending = 0U;
  uint32_t index;
  uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
  uint8_t dhcp_bound = dhcp_supplied_address(&gnetif) ? 1U : 0U;

  control_service_get_stats(&control_stats);
  discovery_service_get_stats(&discovery_stats);
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
                 "\"ports\":{\"control\":41000,\"discovery\":40000,\"http\":80},"
                 "\"rtos\":{\"heap_free\":%lu,\"heap_min\":%lu},"
                 "\"control\":{\"connections\":%lu,\"commands\":%lu,\"errors\":%lu,\"listener_up\":%lu},"
                 "\"discovery\":{\"rx\":%lu,\"announce\":%lu,\"discover\":%lu},"
                 "\"auth\":{\"required\":1,\"active_session\":%u,\"sessions_issued\":%lu,\"login_failures\":%lu},"
                 "\"http\":{\"connections\":%lu,\"requests\":%lu,\"errors\":%lu,\"active\":%lu,\"close_pending\":%lu,\"listener_up\":%lu},"
                 "\"lwip\":{\"mem_avail\":%lu,\"mem_used\":%lu,\"mem_err\":%lu,\"tcp_pcb_used\":%lu,\"tcp_pcb_max\":%lu,\"tcp_seg_used\":%lu,\"tcp_seg_max\":%lu,\"pbuf_used\":%lu,\"pbuf_max\":%lu}"
                 "}",
                 (unsigned long)HAL_GetTick(),
                 (unsigned int)link_up,
                 (unsigned int)dhcp_bound,
                 ip_text,
                 mask_text,
                 gw_text,
                 (unsigned long)board_relay_snapshot(),
                 (unsigned long)heap_free,
                 (unsigned long)heap_min,
                 (unsigned long)control_stats.connections,
                 (unsigned long)control_stats.commands,
                 (unsigned long)control_stats.errors,
                 (unsigned long)control_stats.listener_up,
                 (unsigned long)discovery_stats.rx_packets,
                 (unsigned long)discovery_stats.announce_packets,
                 (unsigned long)discovery_stats.discover_packets,
                 (unsigned int)(g_http_auth_state.active ? 1U : 0U),
                 (unsigned long)g_http_auth_state.sessions_issued,
                 (unsigned long)g_http_auth_state.login_failures,
                 (unsigned long)g_http_service_stats.connections,
                 (unsigned long)g_http_service_stats.requests,
                 (unsigned long)g_http_service_stats.errors,
                 (unsigned long)http_active,
                 (unsigned long)http_close_pending,
                 (unsigned long)((g_http_listener_pcb != NULL) ? 1U : 0U),
                 (unsigned long)lwip_stats.mem.avail,
                 (unsigned long)lwip_stats.mem.used,
                 (unsigned long)lwip_stats.mem.err,
                 (unsigned long)MEMP_STATS_GET(used, MEMP_TCP_PCB),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_TCP_PCB),
                 (unsigned long)MEMP_STATS_GET(used, MEMP_TCP_SEG),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_TCP_SEG),
                 (unsigned long)MEMP_STATS_GET(used, MEMP_PBUF),
                 (unsigned long)MEMP_STATS_GET(max, MEMP_PBUF));
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
                 (unsigned int)(g_http_auth_state.active ? 1U : 0U),
                 (unsigned long)HTTP_AUTH_SESSION_TTL_MS);
}

static void http_service_format_login_json(char *buffer, size_t buffer_size)
{
  (void)snprintf(buffer, buffer_size,
                 "{"
                 "\"ok\":true,"
                 "\"token\":%lu,"
                 "\"expires_in_ms\":%lu"
                 "}",
                 (unsigned long)g_http_auth_state.token,
                 (unsigned long)HTTP_AUTH_SESSION_TTL_MS);
}

static void http_service_format_logout_json(char *buffer, size_t buffer_size)
{
  (void)snprintf(buffer, buffer_size,
                 "{\"ok\":true,\"logged_out\":true}");
}

static void http_service_apply_command(control_proto_command_t *command, char *buffer, size_t buffer_size)
{
  relay_domain_result_t result;

  if ((command == NULL) || (buffer == NULL))
  {
    return;
  }

  relay_domain_execute(command, &result);

  if (result.error != CONTROL_PROTO_ERR_NONE)
  {
    g_http_service_stats.errors++;
    http_service_format_error_json(buffer, buffer_size, "relay_command_failed");
    return;
  }

  http_service_format_relay_json(buffer, buffer_size, result.relay_mask);
}

static err_t http_service_handle_request(http_service_connection_t *connection, const char *request)
{
  char method[8];
  char path[64];
  char response_body[HTTP_BODY_BUFFER_SIZE];
  const char *body;
  uint32_t value;
  uint8_t authenticated = http_service_has_valid_session(request);
  control_proto_command_t command;
  char password_text[32];

  if ((connection == NULL) || (connection->pcb == NULL) || (request == NULL))
  {
    return ERR_ARG;
  }

  if (sscanf(request, "%7s %63s", method, path) != 2)
  {
    http_service_format_error_json(response_body, sizeof(response_body), "bad_request");
    return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
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

  if ((strcmp(method, "GET") == 0) &&
      ((strcmp(path, "/") == 0) || (strcmp(path, "/index.html") == 0)))
  {
    return http_service_send_payload_streaming(connection, "200 OK", "text/html; charset=utf-8", g_http_index_html, (size_t)g_http_index_html_len);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/session") == 0))
  {
    http_service_format_session_json(response_body, sizeof(response_body), authenticated);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/status") == 0))
  {
    http_service_format_status_json(response_body, sizeof(response_body));
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/relay") == 0))
  {
    http_service_format_relay_json(response_body, sizeof(response_body), board_relay_snapshot());
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/login") == 0))
  {
    if (!http_service_extract_form_text(body, "password", password_text, sizeof(password_text)) ||
        (strcmp(password_text, HTTP_AUTH_PASSWORD) != 0))
    {
      g_http_auth_state.login_failures++;
      http_service_format_error_json(response_body, sizeof(response_body), "bad_password");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    g_http_auth_state.token = http_service_issue_session_token();
    g_http_auth_state.expires_at_ms = HAL_GetTick() + HTTP_AUTH_SESSION_TTL_MS;
    g_http_auth_state.sessions_issued++;
    g_http_auth_state.active = 1U;
    http_service_format_login_json(response_body, sizeof(response_body));
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/logout") == 0))
  {
    g_http_auth_state.active = 0U;
    g_http_auth_state.token = 0U;
    g_http_auth_state.expires_at_ms = 0U;
    http_service_format_logout_json(response_body, sizeof(response_body));
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/relay-mask") == 0))
  {
    if (!authenticated)
    {
      http_service_format_error_json(response_body, sizeof(response_body), "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    memset(&command, 0, sizeof(command));
    command.type = CONTROL_PROTO_CMD_SET_RELAY_MASK;
    command.error = CONTROL_PROTO_ERR_NONE;

    if (!http_service_extract_form_value(body, "mask", &value) || (value > BOARD_RELAY_ALL_MASK))
    {
      http_service_format_error_json(response_body, sizeof(response_body), "bad_mask");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    command.mask = value;
    http_service_apply_command(&command, response_body, sizeof(response_body));
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/relay-channel") == 0))
  {
    uint32_t channel;
    uint32_t relay_value;

    if (!authenticated)
    {
      http_service_format_error_json(response_body, sizeof(response_body), "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    memset(&command, 0, sizeof(command));
    command.type = CONTROL_PROTO_CMD_SET_RELAY;
    command.error = CONTROL_PROTO_ERR_NONE;

    if (!http_service_extract_form_value(body, "channel", &channel) ||
        !http_service_extract_form_value(body, "value", &relay_value) ||
        (channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT) || (relay_value > 1U))
    {
      http_service_format_error_json(response_body, sizeof(response_body), "bad_channel_or_value");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    command.channel = (uint8_t)channel;
    command.value = (uint8_t)relay_value;
    http_service_apply_command(&command, response_body, sizeof(response_body));
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  http_service_format_error_json(response_body, sizeof(response_body), "not_found");
  return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
}

static err_t http_service_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;
  char request[HTTP_REQUEST_BUFFER_SIZE];
  u16_t copy_len;

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
  copy_len = (p->tot_len < (sizeof(request) - 1U)) ? p->tot_len : (u16_t)(sizeof(request) - 1U);
  pbuf_copy_partial(p, request, copy_len, 0U);
  request[copy_len] = '\0';
  pbuf_free(p);

  g_http_service_stats.requests++;
  
  /* Reset chunk tracking for the new request */
  if (connection != NULL)
  {
     connection->send_data = NULL;
     connection->send_total = 0;
     connection->send_progress = 0;
  }
  
  (void)http_service_handle_request(connection, request);
  
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

  LWIP_UNUSED_ARG(err);
  g_http_service_stats.errors++;
  http_service_release_connection(connection);
}

static err_t http_service_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;

  LWIP_UNUSED_ARG(len);
  if (connection != NULL)
  {
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

  LWIP_UNUSED_ARG(arg);

  if ((err != ERR_OK) || (newpcb == NULL))
  {
    g_http_service_stats.errors++;
    return ERR_VAL;
  }

  connection = http_service_alloc_connection(newpcb);
  if (connection == NULL)
  {
    g_http_service_stats.errors++;
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  g_http_service_stats.connections++;
  tcp_arg(newpcb, connection);
  tcp_recv(newpcb, http_service_recv);
  tcp_err(newpcb, http_service_error);
  tcp_sent(newpcb, http_service_sent);
  tcp_poll(newpcb, http_service_poll_close, 1);
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
    return;
  }

  if (g_http_listener_pcb == NULL)
  {
    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] pcb err\r\n");
      return;
    }

    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_TCP_PORT) != ERR_OK)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] bind err\r\n");
      (void)tcp_close(pcb);
      return;
    }

    g_http_listener_pcb = tcp_listen_with_backlog(pcb, 4);
    if (g_http_listener_pcb == NULL)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] listen err\r\n");
      (void)tcp_close(pcb);
      return;
    }

    tcp_accept(g_http_listener_pcb, http_service_accept);
    http_service_log_status("[http] up 80\r\n");
  }

  g_http_last_link_up = 1U;
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
  http_service_init();
  (void)tcpip_callback(http_service_tcpip_step, NULL);
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
