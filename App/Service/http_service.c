#include "http_service.h"

#include "cmsis_os.h"

#include "board_relay.h"
#include "control_proto.h"
#include "control_queue.h"
#include "control_service.h"
#include "cluster_service.h"
#include "discovery_service.h"
#include "relay_domain.h"
#include "../Config/device_config.h"

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
#define HTTP_BODY_BUFFER_SIZE       4096U
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
static volatile uint8_t g_http_poll_pending = 0U;
static uint32_t g_http_poll_skip_count = 0U;
static char g_http_request_buffer[HTTP_REQUEST_BUFFER_SIZE];
static char g_http_response_buffer[HTTP_BODY_BUFFER_SIZE];

static void http_service_tcpip_step(void *arg);
static void http_service_uart_write(const char *text);
static void http_service_log_status(const char *text);
static void http_service_log_error_value(const char *prefix, int value);
static uint32_t http_service_active_connection_count(void);
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
static void http_service_format_cluster_config_json(char *buffer, size_t buffer_size);
static void http_service_format_cluster_nodes_json(char *buffer, size_t buffer_size);
static void http_service_format_relay_config_json(char *buffer, size_t buffer_size);
static uint8_t http_service_get_master_ip_text(char *buffer, size_t buffer_size, uint8_t *is_local_master);
static uint8_t http_service_is_master_control_allowed(char *master_ip_text, size_t buffer_size);
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
  if (g_http_listener_pcb == NULL)
  {
    return;
  }

  http_service_log_status("[http] listener down\r\n");
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
                        "Content-Length: %u\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Headers: Content-Type, X-Auth-Token\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
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
                        "Content-Length: %u\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Headers: Content-Type, X-Auth-Token\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
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
  cluster_service_state_t cluster_state;
  size_t heap_free = xPortGetFreeHeapSize();
  size_t heap_min = xPortGetMinimumEverFreeHeapSize();
  uint32_t http_active = 0U;
  uint32_t http_close_pending = 0U;
  uint32_t index;
  uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
  uint8_t dhcp_bound = dhcp_supplied_address(&gnetif) ? 1U : 0U;

  control_service_get_stats(&control_stats);
  discovery_service_get_stats(&discovery_stats);
  cluster_service_get_state(&cluster_state);
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
                 "\"cluster\":{\"enabled\":%u,\"name\":\"%s\",\"role\":%u,\"join_mode\":%u,\"self_node_id\":%lu,\"master_node_id\":%lu,\"term\":%lu},"
                 "\"ports\":{\"control\":41000,\"discovery\":40000,\"http\":80},"
                 "\"rtos\":{\"heap_free\":%lu,\"heap_min\":%lu},"
                 "\"control\":{\"connections\":%lu,\"commands\":%lu,\"errors\":%lu,\"listener_up\":%lu},"
                 "\"discovery\":{\"rx\":%lu,\"tx_attempts\":%lu,\"announce\":%lu,\"tx_failures\":%lu,\"discover\":%lu,\"last_tx_err\":%ld,\"last_payload_len\":%lu},"
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
                 (unsigned int)cluster_state.cluster_enabled,
                 cluster_state.cluster_name,
                 (unsigned int)cluster_state.role,
                 (unsigned int)cluster_state.join_mode,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned long)cluster_state.master_node_id,
                 (unsigned long)cluster_state.term,
                 (unsigned long)heap_free,
                 (unsigned long)heap_min,
                 (unsigned long)control_stats.connections,
                 (unsigned long)control_stats.commands,
                 (unsigned long)control_stats.errors,
                 (unsigned long)control_stats.listener_up,
                 (unsigned long)discovery_stats.rx_packets,
                 (unsigned long)discovery_stats.tx_attempts,
                 (unsigned long)discovery_stats.announce_packets,
                 (unsigned long)discovery_stats.tx_failures,
                 (unsigned long)discovery_stats.discover_packets,
                 (long)discovery_stats.last_tx_err,
                 (unsigned long)discovery_stats.last_payload_len,
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

  if (http_service_get_master_ip_text(master_ip_text, buffer_size, &is_local_master) == 0U)
  {
    return 1U;
  }

  if ((cluster_state.role == CLUSTER_ROLE_MEMBER) && (is_local_master == 0U))
  {
    return 0U;
  }

  return 1U;
}

static void http_service_format_cluster_config_json(char *buffer, size_t buffer_size)
{
  const device_config_t *config = device_config_get();
  cluster_service_state_t cluster_state;
  char master_ip[16];
  char device_name_json[(DEVICE_CONFIG_NAME_MAX_LEN * 2U) + 8U];
  char cluster_name_json[(DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN * 2U) + 8U];
  uint8_t is_local_master = 0U;

  cluster_service_get_state(&cluster_state);
  (void)http_service_get_master_ip_text(master_ip, sizeof(master_ip), &is_local_master);
  http_service_json_escape_copy(device_name_json, sizeof(device_name_json),
                                config->device_name, sizeof(config->device_name));
  http_service_json_escape_copy(cluster_name_json, sizeof(cluster_name_json),
                                config->cluster_name, sizeof(config->cluster_name));
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
                 "\"is_local_master\":%u"
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
                 (unsigned int)is_local_master);
}

static void http_service_format_cluster_nodes_json(char *buffer, size_t buffer_size)
{
  const cluster_node_t *nodes;
  const device_config_t *config = device_config_get();
  uint32_t count = 0U;
  uint32_t index;
  uint32_t local_auth_tag = device_config_password_tag(config);
  size_t used;
  char ip_text[16];
  char cluster_name_json[(CLUSTER_SERVICE_CLUSTER_NAME_LEN * 2U) + 8U];
  char node_name_json[(CLUSTER_SERVICE_DEVICE_NAME_LEN * 2U) + 8U];
  ip4_addr_t ip_addr;

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

    same_cluster = ((config->cluster_enabled != 0U) &&
                    (strcmp(nodes[index].cluster_name, config->cluster_name) == 0) &&
                    (nodes[index].auth_tag == local_auth_tag)) ? 1U : 0U;
    http_service_json_escape_copy(cluster_name_json, sizeof(cluster_name_json),
                                  nodes[index].cluster_name, sizeof(nodes[index].cluster_name));
    http_service_json_escape_copy(node_name_json, sizeof(node_name_json),
                                  nodes[index].node_name, sizeof(nodes[index].node_name));
    ip_addr.addr = nodes[index].ip_addr;
    (void)ip4addr_ntoa_r(&ip_addr, ip_text, sizeof(ip_text));
    written = snprintf(buffer + used,
                       buffer_size - used,
                       "%s{\"node_id\":%lu,\"ip\":%lu,\"ip_text\":\"%s\",\"role\":%u,\"term\":%lu,\"load\":%lu,\"relay_mask\":%lu,\"enabled_mask\":%lu,\"auto\":%u,\"same_cluster\":%u,\"controllable\":%u,\"auth_match\":%u,\"cluster_name\":\"%s\",\"name\":\"%s\"}",
                       (used > (size_t)20U) ? "," : "",
                       (unsigned long)nodes[index].node_id,
                       (unsigned long)nodes[index].ip_addr,
                       ip_text,
                       (unsigned int)nodes[index].role,
                       (unsigned long)nodes[index].term,
                       (unsigned long)nodes[index].load_score,
                       (unsigned long)nodes[index].relay_mask,
                       (unsigned long)nodes[index].enabled_mask,
                       (unsigned int)nodes[index].auto_join,
                       (unsigned int)same_cluster,
                       (unsigned int)same_cluster,
                       (unsigned int)(nodes[index].auth_tag == local_auth_tag ? 1U : 0U),
                       cluster_name_json,
                       node_name_json);
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
    http_service_json_escape_copy(relay_name_json, sizeof(relay_name_json),
                                  config->relay[index].name, sizeof(config->relay[index].name));
    int written = snprintf(buffer + used,
                           buffer_size - used,
                           "%s{\"channel\":%lu,\"name\":\"%s\",\"enabled\":%u,\"remember_state\":%u,\"boot_default_on\":%u}",
                           (index > 0U) ? "," : "",
                           (unsigned long)(index + 1U),
                           relay_name_json,
                           (unsigned int)config->relay[index].enabled,
                           (unsigned int)config->relay[index].remember_state,
                           (unsigned int)config->relay[index].boot_default_on);
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
  relay_domain_result_t result;

  if ((command == NULL) || (buffer == NULL))
  {
    return;
  }

  memset(&result, 0, sizeof(result));
  if (control_queue_submit(command, &result) != HAL_OK)
  {
    g_http_service_stats.errors++;
    http_service_format_error_json(buffer, buffer_size, "relay_command_failed");
    return;
  }

  if (result.error != CONTROL_PROTO_ERR_NONE)
  {
    g_http_service_stats.errors++;
    http_service_format_error_json(buffer, buffer_size, "relay_command_failed");
    return;
  }

  http_service_format_relay_json(buffer, buffer_size, result.relay_mask);
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

static err_t http_service_handle_request(http_service_connection_t *connection, const char *request)
{
  char method[8];
  char path[64];
  char *response_body = g_http_response_buffer;
  const size_t response_body_size = HTTP_BODY_BUFFER_SIZE;
  const char *body;
  uint32_t value;
  uint8_t authenticated = http_service_has_valid_session(request);
  control_proto_command_t command;
  char password_text[32];
  char device_name[DEVICE_CONFIG_NAME_MAX_LEN];
  char cluster_name[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN];
  char relay_name[DEVICE_CONFIG_NAME_MAX_LEN];
  char master_ip_text[16];
  const cluster_node_t *remote_node;
  const device_config_t *active_config = device_config_get();
  uint32_t local_auth_tag = device_config_password_tag(active_config);
  cluster_service_state_t cluster_state;

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
    return http_service_send_payload_streaming(connection, "200 OK", "text/html; charset=utf-8", g_http_index_html, (size_t)g_http_index_html_len);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/session") == 0))
  {
    http_service_format_session_json(response_body, response_body_size, authenticated);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/status") == 0))
  {
    http_service_format_status_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/relay") == 0))
  {
    http_service_format_relay_json(response_body, response_body_size, board_relay_snapshot());
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/config/cluster") == 0))
  {
    http_service_format_cluster_config_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/cluster/nodes") == 0))
  {
    http_service_format_cluster_nodes_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/config/relays") == 0))
  {
    http_service_format_relay_config_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/login") == 0))
  {
    if (!http_service_extract_form_text(body, "password", password_text, sizeof(password_text)) ||
        (strcmp(password_text, HTTP_AUTH_PASSWORD) != 0))
    {
      g_http_auth_state.login_failures++;
      http_service_format_error_json(response_body, response_body_size, "bad_password");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    g_http_auth_state.token = http_service_issue_session_token();
    g_http_auth_state.expires_at_ms = HAL_GetTick() + HTTP_AUTH_SESSION_TTL_MS;
    g_http_auth_state.sessions_issued++;
    g_http_auth_state.active = 1U;
    http_service_format_login_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/logout") == 0))
  {
    g_http_auth_state.active = 0U;
    g_http_auth_state.token = 0U;
    g_http_auth_state.expires_at_ms = 0U;
    http_service_format_logout_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/relay-mask") == 0))
  {
    if (!authenticated)
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

    if (!authenticated)
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

    if (!authenticated)
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
        (strcmp(remote_node->cluster_name, active_config->cluster_name) != 0) ||
        (remote_node->auth_tag != local_auth_tag))
    {
      http_service_format_error_json(response_body, response_body_size, "remote_node_unavailable");
      return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
    }

    command.channel = (uint8_t)channel;
    command.value = (uint8_t)relay_value;
    http_service_apply_remote_command(remote_node, &command, response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/config/cluster") == 0))
  {
    device_config_t config;

    if (!authenticated)
    {
      http_service_format_error_json(response_body, response_body_size, "unauthorized");
      return http_service_send_payload(connection, "401 Unauthorized", "application/json", response_body);
    }

    config = *device_config_get();

    if (http_service_extract_form_text(body, "device_name", device_name, sizeof(device_name)))
    {
      memset(config.device_name, 0, sizeof(config.device_name));
      strncpy(config.device_name, device_name, sizeof(config.device_name) - 1U);
    }

    if (http_service_extract_form_text(body, "cluster_name", cluster_name, sizeof(cluster_name)))
    {
      memset(config.cluster_name, 0, sizeof(config.cluster_name));
      strncpy(config.cluster_name, cluster_name, sizeof(config.cluster_name) - 1U);
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
      if (password_text[0] != '\0')
      {
        device_config_hash_password_text(password_text, config.cluster_password_hash);
      }
      else
      {
        memset(config.cluster_password_hash, 0, sizeof(config.cluster_password_hash));
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

    if (device_config_update_and_save(&config) == 0U)
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

    cluster_service_reload_from_config();
    discovery_service_force_announce();
    http_service_format_cluster_config_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/config/relay") == 0))
  {
    device_config_t config;
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

    if (config.relay[channel - 1U].name[0] == '\0')
    {
      http_service_format_error_json(response_body, response_body_size, "bad_relay_name");
      return http_service_send_payload(connection, "400 Bad Request", "application/json", response_body);
    }

    if (device_config_update_and_save(&config) == 0U)
    {
      (void)snprintf(response_body,
                     response_body_size,
                     "{\"ok\":false,\"error\":\"config_save_failed\",\"flash_error\":%lu}",
                     (unsigned long)device_config_last_flash_error());
      return http_service_send_payload(connection, "500 Internal Server Error", "application/json", response_body);
    }

    http_service_format_relay_config_json(response_body, response_body_size);
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/cluster/kick") == 0))
  {
    uint32_t node_id;

    if (!authenticated)
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
    return http_service_send_payload(connection, "200 OK", "application/json", response_body);
  }

  http_service_format_error_json(response_body, response_body_size, "not_found");
  return http_service_send_payload(connection, "404 Not Found", "application/json", response_body);
}

static err_t http_service_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  http_service_connection_t *connection = (http_service_connection_t *)arg;
  char *request = g_http_request_buffer;
  const size_t request_buffer_size = HTTP_REQUEST_BUFFER_SIZE;
  char line[96];
  err_t request_err;
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
  copy_len = (p->tot_len < (request_buffer_size - 1U)) ? p->tot_len : (u16_t)(request_buffer_size - 1U);
  pbuf_copy_partial(p, request, copy_len, 0U);
  request[copy_len] = '\0';
  pbuf_free(p);

  g_http_service_stats.requests++;
  if (sscanf(request, "%95[^\r\n]", line) == 1)
  {
    char message[128];
    (void)snprintf(message, sizeof(message), "[http] req %s\r\n", line);
    http_service_uart_write(message);
  }
  
  /* Reset chunk tracking for the new request */
  if (connection != NULL)
  {
     connection->send_data = NULL;
     connection->send_total = 0;
     connection->send_progress = 0;
  }
  
  request_err = http_service_handle_request(connection, request);
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
  char message[64];

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
      (void)tcp_close(pcb);
      g_http_poll_pending = 0U;
      return;
    }

    g_http_listener_pcb = tcp_listen_with_backlog(pcb, 4);
    if (g_http_listener_pcb == NULL)
    {
      g_http_service_stats.errors++;
      http_service_log_status("[http] listen err\r\n");
      (void)tcp_close(pcb);
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

  http_service_init();
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
  err = tcpip_callback(http_service_tcpip_step, NULL);
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
