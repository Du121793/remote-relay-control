#include "backend_link_service.h"

#include "cluster_service.h"
#include "control_queue.h"
#include "control_proto.h"
#include "discovery_service.h"
#include "fault_service.h"
#include "../Config/device_config.h"
#include "../Platform/board_relay.h"

#include "cmsis_os.h"
#include "task.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/dhcp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKEND_LINK_CONNECT_RETRY_MS          3000UL
#define BACKEND_LINK_HEARTBEAT_INTERVAL_MS     5000UL
#define BACKEND_LINK_STATUS_INTERVAL_MS        3000UL
#define BACKEND_LINK_LOOP_DELAY_MS             200UL
#define BACKEND_LINK_MASTER_STABLE_DELAY_MS    2000UL
#define BACKEND_LINK_STALE_RECONNECT_MS        12000UL
#define BACKEND_LINK_SEND_TIMEOUT_MS            500UL
#define BACKEND_LINK_SEND_RETRY_DELAY_MS          5UL
#define BACKEND_LINK_RX_BUFFER_SIZE            512U
#define BACKEND_LINK_TX_BUFFER_SIZE            4096U

extern struct netif gnetif;

typedef struct
{
  uint8_t started;
  uint8_t connected;
  uint8_t status_dirty;
  uint8_t last_should_run;
  uint8_t last_should_run_reason;
  int sock;
  uint32_t last_connect_attempt_ms;
  uint32_t last_heartbeat_ms;
  uint32_t last_status_ms;
  uint32_t last_send_ok_ms;
  uint32_t last_rx_ms;
  uint32_t session_id;
  uint32_t master_since_ms;
  uint32_t last_loop_log_ms;
  osThreadId task_handle;
  backend_link_stats_t stats;
  char rx_buffer[BACKEND_LINK_RX_BUFFER_SIZE];
  char tx_buffer[BACKEND_LINK_TX_BUFFER_SIZE];
  size_t rx_length;
} backend_link_context_t;

static backend_link_context_t g_backend_link_context;

static void backend_link_log(const char *text);
static void backend_link_log_reason(const char *prefix, uint32_t value);
static uint8_t backend_link_should_run_reason(void);

void backend_link_service_mark_status_dirty(void)
{
  g_backend_link_context.status_dirty = 1U;
}

static void backend_link_json_escape_copy(char *dst, size_t dst_size, const char *src)
{
  size_t di = 0U;
  size_t si = 0U;

  if ((dst == NULL) || (dst_size == 0U))
  {
    return;
  }

  dst[0] = '\0';
  if (src == NULL)
  {
    return;
  }

  while ((src[si] != '\0') && ((di + 1U) < dst_size))
  {
    const char *replacement = NULL;
    switch (src[si])
    {
      case '\"': replacement = "\\\""; break;
      case '\\': replacement = "\\\\"; break;
      case '\n': replacement = "\\n"; break;
      case '\r': replacement = "\\r"; break;
      case '\t': replacement = "\\t"; break;
      default: break;
    }

    if (replacement != NULL)
    {
      size_t ri = 0U;
      while ((replacement[ri] != '\0') && ((di + 1U) < dst_size))
      {
        dst[di++] = replacement[ri++];
      }
    }
    else
    {
      dst[di++] = src[si];
    }
    si++;
  }

  dst[di] = '\0';
}

static uint8_t backend_link_extract_json_string(const char *line, const char *key, char *buffer, size_t buffer_size)
{
  char pattern[64];
  const char *match;
  const char *value;
  size_t index = 0U;

  if ((line == NULL) || (key == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  (void)snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  match = strstr(line, pattern);
  if (match == NULL)
  {
    return 0U;
  }

  value = match + strlen(pattern);
  while ((*value != '\0') && (*value != '\"') && (index < (buffer_size - 1U)))
  {
    buffer[index++] = *value++;
  }
  buffer[index] = '\0';
  return (index > 0U) ? 1U : 0U;
}

static uint8_t backend_link_extract_json_u32(const char *line, const char *key, uint32_t *value_out)
{
  char pattern[64];
  const char *match;
  char *end_ptr;
  unsigned long parsed;

  if ((line == NULL) || (key == NULL) || (value_out == NULL))
  {
    return 0U;
  }

  (void)snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  match = strstr(line, pattern);
  if (match == NULL)
  {
    return 0U;
  }

  parsed = strtoul(match + strlen(pattern), &end_ptr, 10);
  if (end_ptr == (match + strlen(pattern)))
  {
    return 0U;
  }

  *value_out = (uint32_t)parsed;
  return 1U;
}

static void backend_link_apply_cloud_config(const char *line)
{
  device_config_t config;
  const device_config_t *current_config = device_config_get();
  char text_buffer[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN];
  uint32_t value;
  uint8_t updated = 0U;

  if (line == NULL)
  {
    return;
  }

  config = *current_config;

  if (backend_link_extract_json_string(line, "device_name", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.device_name, text_buffer, sizeof(config.device_name)) != 0)
    {
      memset(config.device_name, 0, sizeof(config.device_name));
      strncpy(config.device_name, text_buffer, sizeof(config.device_name) - 1U);
      updated = 1U;
    }
  }
  if (backend_link_extract_json_u32(line, "cloud_enabled", &value) != 0U)
  {
    uint8_t next_value = (uint8_t)(value & 0xFFU);
    if (config.cloud_enabled != next_value)
    {
      config.cloud_enabled = next_value;
      updated = 1U;
    }
  }
  if (backend_link_extract_json_u32(line, "cloud_transport", &value) != 0U)
  {
    uint8_t next_value = (uint8_t)(value & 0xFFU);
    if (config.cloud_transport != next_value)
    {
      config.cloud_transport = next_value;
      updated = 1U;
    }
  }
  if (backend_link_extract_json_string(line, "cloud_broker_host", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.cloud_broker_host, text_buffer, sizeof(config.cloud_broker_host)) != 0)
    {
      memset(config.cloud_broker_host, 0, sizeof(config.cloud_broker_host));
      strncpy(config.cloud_broker_host, text_buffer, sizeof(config.cloud_broker_host) - 1U);
      updated = 1U;
    }
  }
  if (backend_link_extract_json_u32(line, "cloud_broker_port", &value) != 0U)
  {
    uint16_t next_value = (uint16_t)(value & 0xFFFFU);
    if (config.cloud_broker_port != next_value)
    {
      config.cloud_broker_port = next_value;
      updated = 1U;
    }
  }
  if (backend_link_extract_json_string(line, "cloud_cluster_id", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.cloud_cluster_id, text_buffer, sizeof(config.cloud_cluster_id)) != 0)
    {
      memset(config.cloud_cluster_id, 0, sizeof(config.cloud_cluster_id));
      strncpy(config.cloud_cluster_id, text_buffer, sizeof(config.cloud_cluster_id) - 1U);
      updated = 1U;
    }
  }
  if (backend_link_extract_json_string(line, "cloud_device_id", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.cloud_device_id, text_buffer, sizeof(config.cloud_device_id)) != 0)
    {
      memset(config.cloud_device_id, 0, sizeof(config.cloud_device_id));
      strncpy(config.cloud_device_id, text_buffer, sizeof(config.cloud_device_id) - 1U);
      updated = 1U;
    }
  }
  if (backend_link_extract_json_string(line, "cloud_key_id", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.cloud_key_id, text_buffer, sizeof(config.cloud_key_id)) != 0)
    {
      memset(config.cloud_key_id, 0, sizeof(config.cloud_key_id));
      strncpy(config.cloud_key_id, text_buffer, sizeof(config.cloud_key_id) - 1U);
      updated = 1U;
    }
  }
  if (backend_link_extract_json_string(line, "cloud_cluster_ca_fingerprint", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.cloud_cluster_ca_fingerprint, text_buffer, sizeof(config.cloud_cluster_ca_fingerprint)) != 0)
    {
      memset(config.cloud_cluster_ca_fingerprint, 0, sizeof(config.cloud_cluster_ca_fingerprint));
      strncpy(config.cloud_cluster_ca_fingerprint, text_buffer, sizeof(config.cloud_cluster_ca_fingerprint) - 1U);
      updated = 1U;
    }
  }
  if (backend_link_extract_json_string(line, "cloud_cluster_binding_secret", text_buffer, sizeof(text_buffer)) != 0U)
  {
    if (strncmp(config.cloud_cluster_binding_secret, text_buffer, sizeof(config.cloud_cluster_binding_secret)) != 0)
    {
      memset(config.cloud_cluster_binding_secret, 0, sizeof(config.cloud_cluster_binding_secret));
      strncpy(config.cloud_cluster_binding_secret, text_buffer, sizeof(config.cloud_cluster_binding_secret) - 1U);
      updated = 1U;
    }
  }

  if ((updated != 0U) && (device_config_update_and_save(&config) != 0U))
  {
    backend_link_log("[backend] config_cloud applied\r\n");
    backend_link_service_mark_status_dirty();
    discovery_service_force_announce();
  }
  else if (updated != 0U)
  {
    backend_link_log("[backend] config_cloud save failed\r\n");
  }
}

static uint8_t backend_link_network_ready(void)
{
  if ((netif_is_up(&gnetif) == 0) || (netif_is_link_up(&gnetif) == 0))
  {
    return 0U;
  }

  if (ip4_addr_isany_val(*netif_ip4_addr(&gnetif)) != 0)
  {
    return 0U;
  }

  return 1U;
}

static const char *backend_link_host(void)
{
  const device_config_t *config = device_config_get();

  if ((config->backend_host[0] == '\0') || (strcmp(config->backend_host, "0.0.0.0") == 0))
  {
    return "192.168.0.10";
  }

  return config->backend_host;
}

static uint16_t backend_link_port(void)
{
  const device_config_t *config = device_config_get();

  return (config->backend_port != 0U) ? config->backend_port : 42001U;
}

static const char *backend_link_key(void)
{
  const device_config_t *config = device_config_get();

  return config->backend_key;
}

static void backend_link_close_socket(void)
{
  if (g_backend_link_context.sock >= 0)
  {
    lwip_close(g_backend_link_context.sock);
    g_backend_link_context.sock = -1;
  }

  g_backend_link_context.connected = 0U;
  g_backend_link_context.stats.connected = 0U;
  g_backend_link_context.rx_length = 0U;
  g_backend_link_context.status_dirty = 1U;
  g_backend_link_context.last_send_ok_ms = 0U;
  g_backend_link_context.last_rx_ms = 0U;
}

static void backend_link_log(const char *text)
{
  extern UART_HandleTypeDef huart3;
  if ((text != NULL) && (huart3.Instance != NULL))
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)strlen(text), 50U);
  }
}

static void backend_link_log_reason(const char *prefix, uint32_t value)
{
  char buffer[96];

  (void)snprintf(buffer, sizeof(buffer), "%s%lu\r\n", (prefix != NULL) ? prefix : "[backend] ", (unsigned long)value);
  backend_link_log(buffer);
}

static uint8_t backend_link_should_run_reason(void)
{
  cluster_service_state_t cluster_state;
  uint32_t now;

  if (backend_link_network_ready() == 0U)
  {
    g_backend_link_context.master_since_ms = 0U;
    return 1U;
  }

  cluster_service_get_state(&cluster_state);
  if (cluster_state.cluster_enabled == 0U)
  {
    g_backend_link_context.master_since_ms = 0U;
    return 2U;
  }
  if (cluster_state.role != CLUSTER_ROLE_MASTER)
  {
    g_backend_link_context.master_since_ms = 0U;
    return 3U;
  }
  if (cluster_state.master_node_id != cluster_state.self_node_id)
  {
    g_backend_link_context.master_since_ms = 0U;
    return 4U;
  }
  if (backend_link_key()[0] == '\0')
  {
    g_backend_link_context.master_since_ms = 0U;
    return 5U;
  }

  now = HAL_GetTick();
  if (g_backend_link_context.master_since_ms == 0U)
  {
    g_backend_link_context.master_since_ms = now;
    return 6U;
  }

  if ((int32_t)(now - g_backend_link_context.master_since_ms) < (int32_t)BACKEND_LINK_MASTER_STABLE_DELAY_MS)
  {
    return 7U;
  }

  return 0U;
}

static uint8_t backend_link_should_run(void)
{
  return (backend_link_should_run_reason() == 0U) ? 1U : 0U;
}

static void backend_link_build_ip_text(char *buffer, size_t buffer_size)
{
  if ((buffer == NULL) || (buffer_size == 0U))
  {
    return;
  }

  ip4addr_ntoa_r(netif_ip4_addr(&gnetif), buffer, (int)buffer_size);
}

static void backend_link_build_node_ip_text(uint32_t ip_addr, char *buffer, size_t buffer_size)
{
  ip4_addr_t addr;

  if ((buffer == NULL) || (buffer_size == 0U))
  {
    return;
  }

  if (ip_addr == 0U)
  {
    buffer[0] = '\0';
    return;
  }

  addr.addr = ip_addr;
  ip4addr_ntoa_r(&addr, buffer, (int)buffer_size);
}

static int backend_link_send_json(const char *json_text)
{
  size_t len;
  size_t sent = 0U;
  uint32_t started_ms;
  int send_len;

  if ((json_text == NULL) || (g_backend_link_context.sock < 0))
  {
    return -1;
  }

  len = strlen(json_text);
  started_ms = HAL_GetTick();
  while (sent < len)
  {
    send_len = lwip_send(g_backend_link_context.sock,
                         json_text + sent,
                         (int)(len - sent),
                         0);
    if (send_len > 0)
    {
      sent += (size_t)send_len;
      continue;
    }

    if ((send_len < 0) &&
        ((errno == EWOULDBLOCK) || (errno == EAGAIN)) &&
        ((HAL_GetTick() - started_ms) < BACKEND_LINK_SEND_TIMEOUT_MS))
    {
      osDelay(BACKEND_LINK_SEND_RETRY_DELAY_MS);
      continue;
    }

    g_backend_link_context.stats.last_error = 1U;
    backend_link_log_reason("[backend] send fail=",
                            (uint32_t)((send_len < 0) ? (1000 - send_len) : sent));
    backend_link_close_socket();
    return -1;
  }

  g_backend_link_context.last_send_ok_ms = HAL_GetTick();

  return 0;
}

static int backend_link_send_hello(void)
{
  char ip_text[16];
  char message[512];
  char cloud_cluster_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
  char cloud_device_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
  char cloud_key_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
  char cloud_cluster_ca_fingerprint_json[DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN * 2U];
  char cloud_cluster_binding_secret_json[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN * 2U];
  cluster_service_state_t cluster_state;
  const device_config_t *config = device_config_get();

  cluster_service_get_state(&cluster_state);
  backend_link_build_ip_text(ip_text, sizeof(ip_text));
  backend_link_json_escape_copy(cloud_cluster_id_json, sizeof(cloud_cluster_id_json), config->cloud_cluster_id);
  backend_link_json_escape_copy(cloud_device_id_json, sizeof(cloud_device_id_json), config->cloud_device_id);
  backend_link_json_escape_copy(cloud_key_id_json, sizeof(cloud_key_id_json), config->cloud_key_id);
  backend_link_json_escape_copy(cloud_cluster_ca_fingerprint_json, sizeof(cloud_cluster_ca_fingerprint_json), config->cloud_cluster_ca_fingerprint);
  backend_link_json_escape_copy(cloud_cluster_binding_secret_json, sizeof(cloud_cluster_binding_secret_json), config->cloud_cluster_binding_secret);

  (void)snprintf(message, sizeof(message),
                 "{\"type\":\"hello\",\"key\":\"%s\",\"device\":\"%s\",\"cluster\":\"%s\",\"node_id\":%lu,\"master\":%u,\"ip\":\"%s\",\"session\":%lu,\"cloud_transport\":%u,\"cloud_cluster_id\":\"%s\",\"cloud_device_id\":\"%s\",\"cloud_key_id\":\"%s\",\"cloud_cluster_ca_fingerprint\":\"%s\",\"cloud_cluster_binding_secret\":\"%s\"}\n",
                 backend_link_key(),
                 config->device_name,
                 config->cluster_name,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned int)((cluster_state.role == CLUSTER_ROLE_MASTER) ? 1U : 0U),
                 ip_text,
                 (unsigned long)g_backend_link_context.session_id,
                 (unsigned int)config->cloud_transport,
                 cloud_cluster_id_json,
                 cloud_device_id_json,
                 cloud_key_id_json,
                 cloud_cluster_ca_fingerprint_json,
                 cloud_cluster_binding_secret_json);

  if (backend_link_send_json(message) != 0)
  {
    return -1;
  }

  g_backend_link_context.stats.sent_hello++;
  return 0;
}

static int backend_link_send_heartbeat(void)
{
  char message[192];
  cluster_service_state_t cluster_state;

  cluster_service_get_state(&cluster_state);
  (void)snprintf(message, sizeof(message),
                 "{\"type\":\"heartbeat\",\"node_id\":%lu,\"role\":%u,\"term\":%lu,"
                 "\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u,"
                 "\"time_source\":%u,\"time_quality\":%u}\n",
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned int)cluster_state.role,
                 (unsigned long)cluster_state.term,
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)cluster_state.unix_time,
                 (unsigned int)cluster_state.time_valid,
                 (unsigned int)cluster_state.time_source,
                 (unsigned int)cluster_state.time_quality);

  if (backend_link_send_json(message) != 0)
  {
    return -1;
  }

  g_backend_link_context.stats.sent_heartbeat++;
  backend_link_log("[backend] heartbeat sent\r\n");
  return 0;
}

static int backend_link_send_status(void)
{
  char ip_text[16];
  char node_ip_text[16];
  char device_json[DEVICE_CONFIG_NAME_MAX_LEN * 2U];
  char cluster_json[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN * 2U];
  char node_name_json[DEVICE_CONFIG_NAME_MAX_LEN * 2U];
  char cloud_cluster_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
  char cloud_device_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
  char cloud_key_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
  char cloud_cluster_ca_fingerprint_json[DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN * 2U];
  cluster_service_state_t cluster_state;
  const cluster_node_t *nodes;
  const device_config_t *config = device_config_get();
  const uint32_t relay_mask = board_relay_snapshot();
  size_t used;
  uint32_t count = 0U;
  uint32_t index;

  cluster_service_get_state(&cluster_state);
  nodes = cluster_service_get_nodes(&count);
  backend_link_build_ip_text(ip_text, sizeof(ip_text));
  backend_link_json_escape_copy(device_json, sizeof(device_json), config->device_name);
  backend_link_json_escape_copy(cluster_json, sizeof(cluster_json), config->cluster_name);
  backend_link_json_escape_copy(cloud_cluster_id_json, sizeof(cloud_cluster_id_json), config->cloud_cluster_id);
  backend_link_json_escape_copy(cloud_device_id_json, sizeof(cloud_device_id_json), config->cloud_device_id);
  backend_link_json_escape_copy(cloud_key_id_json, sizeof(cloud_key_id_json), config->cloud_key_id);
  backend_link_json_escape_copy(cloud_cluster_ca_fingerprint_json, sizeof(cloud_cluster_ca_fingerprint_json), config->cloud_cluster_ca_fingerprint);

  used = (size_t)snprintf(g_backend_link_context.tx_buffer,
                          sizeof(g_backend_link_context.tx_buffer),
                          "{\"type\":\"status\",\"device\":\"%s\",\"cluster\":\"%s\",\"node_id\":%lu,\"master_node_id\":%lu,\"role\":%u,\"term\":%lu,\"relay_mask\":%lu,\"device_state\":%u,\"ip\":\"%s\",\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u,\"time_source\":%u,\"time_quality\":%u,\"time_seq\":%lu,\"gateway_epoch\":%lu,\"gateway_node_id\":%lu,\"cloud_transport\":%u,\"cloud_cluster_id\":\"%s\",\"cloud_device_id\":\"%s\",\"cloud_key_id\":\"%s\",\"cloud_cluster_ca_fingerprint\":\"%s\",\"nodes\":[{\"node_id\":%lu,\"name\":\"%s\",\"ip\":\"%s\",\"role\":%u,\"online\":1,\"device_state\":%u,\"relay_mask\":%lu,\"cloud_cluster_id\":\"%s\",\"cloud_device_id\":\"%s\",\"cloud_key_id\":\"%s\"}",
                          device_json,
                          cluster_json,
                          (unsigned long)cluster_state.self_node_id,
                          (unsigned long)cluster_state.master_node_id,
                          (unsigned int)cluster_state.role,
                          (unsigned long)cluster_state.term,
                          (unsigned long)relay_mask,
                          (unsigned int)CLUSTER_DEVICE_STATE_RUNNING,
                          ip_text,
                          (unsigned long)HAL_GetTick(),
                          (unsigned long)cluster_state.unix_time,
                          (unsigned int)cluster_state.time_valid,
                          (unsigned int)cluster_state.time_source,
                          (unsigned int)cluster_state.time_quality,
                          (unsigned long)cluster_state.time_seq,
                          (unsigned long)cluster_state.gateway_epoch,
                          (unsigned long)cluster_state.gateway_node_id,
                          (unsigned int)config->cloud_transport,
                          cloud_cluster_id_json,
                          cloud_device_id_json,
                          cloud_key_id_json,
                          cloud_cluster_ca_fingerprint_json,
                          (unsigned long)cluster_state.self_node_id,
                          device_json,
                          ip_text,
                          (unsigned int)cluster_state.role,
                          (unsigned int)CLUSTER_DEVICE_STATE_RUNNING,
                          (unsigned long)relay_mask,
                          cloud_cluster_id_json,
                          cloud_device_id_json,
                          cloud_key_id_json);
  if (used >= sizeof(g_backend_link_context.tx_buffer))
  {
    return -1;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    int written;
    char node_cloud_cluster_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
    char node_cloud_device_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];
    char node_cloud_key_id_json[DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 2U];

    if (nodes[index].valid == 0U)
    {
      continue;
    }

    backend_link_json_escape_copy(node_name_json, sizeof(node_name_json), nodes[index].node_name);
    backend_link_json_escape_copy(node_cloud_cluster_id_json, sizeof(node_cloud_cluster_id_json), nodes[index].cloud_cluster_id);
    backend_link_json_escape_copy(node_cloud_device_id_json, sizeof(node_cloud_device_id_json), nodes[index].cloud_device_id);
    backend_link_json_escape_copy(node_cloud_key_id_json, sizeof(node_cloud_key_id_json), nodes[index].cloud_key_id);
    backend_link_build_node_ip_text(nodes[index].ip_addr, node_ip_text, sizeof(node_ip_text));

    written = snprintf(g_backend_link_context.tx_buffer + used,
                       sizeof(g_backend_link_context.tx_buffer) - used,
                       ",{\"node_id\":%lu,\"name\":\"%s\",\"ip\":\"%s\",\"role\":%u,\"online\":%u,\"device_state\":%u,\"relay_mask\":%lu,\"cloud_cluster_id\":\"%s\",\"cloud_device_id\":\"%s\",\"cloud_key_id\":\"%s\"}",
                       (unsigned long)nodes[index].node_id,
                       node_name_json,
                       node_ip_text,
                       (unsigned int)nodes[index].role,
                       (unsigned int)nodes[index].online,
                       (unsigned int)nodes[index].device_state,
                       (unsigned long)nodes[index].relay_mask,
                       node_cloud_cluster_id_json,
                       node_cloud_device_id_json,
                       node_cloud_key_id_json);
    if ((written < 0) || ((size_t)written >= (sizeof(g_backend_link_context.tx_buffer) - used)))
    {
      return -1;
    }
    used += (size_t)written;
  }

  if (used + 4U >= sizeof(g_backend_link_context.tx_buffer))
  {
    return -1;
  }
  g_backend_link_context.tx_buffer[used++] = ']';
  g_backend_link_context.tx_buffer[used++] = '}';
  g_backend_link_context.tx_buffer[used++] = '\n';
  g_backend_link_context.tx_buffer[used] = '\0';

  if (backend_link_send_json(g_backend_link_context.tx_buffer) != 0)
  {
    return -1;
  }

  g_backend_link_context.stats.sent_status++;
  backend_link_log("[backend] status sent\r\n");
  return 0;
}

static int backend_link_send_fault_report(void)
{
  fault_service_record_t record;
  cluster_service_state_t cluster_state;
  int written;

  if (fault_service_peek(&record) == 0U)
  {
    return 0;
  }

  cluster_service_get_state(&cluster_state);
  written = snprintf(g_backend_link_context.tx_buffer,
                     sizeof(g_backend_link_context.tx_buffer),
                     "{\"type\":\"fault_report\",\"fault_type\":%lu,\"cfsr\":%lu,\"hfsr\":%lu,\"mmfar\":%lu,\"bfar\":%lu,\"exc_lr\":%lu,\"sp\":%lu,\"pc\":%lu,\"lr\":%lu,\"psr\":%lu,\"r0\":%lu,\"r1\":%lu,\"r2\":%lu,\"r3\":%lu,\"r12\":%lu,\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u}\n",
                     (unsigned long)record.type,
                     (unsigned long)record.cfsr,
                     (unsigned long)record.hfsr,
                     (unsigned long)record.mmfar,
                     (unsigned long)record.bfar,
                     (unsigned long)record.exc_lr,
                     (unsigned long)record.sp,
                     (unsigned long)record.pc,
                     (unsigned long)record.lr,
                     (unsigned long)record.psr,
                     (unsigned long)record.r0,
                     (unsigned long)record.r1,
                     (unsigned long)record.r2,
                     (unsigned long)record.r3,
                     (unsigned long)record.r12,
                     (unsigned long)HAL_GetTick(),
                     (unsigned long)cluster_state.unix_time,
                     (unsigned int)cluster_state.time_valid);
  if ((written <= 0) || ((size_t)written >= sizeof(g_backend_link_context.tx_buffer)))
  {
    return -1;
  }

  if (backend_link_send_json(g_backend_link_context.tx_buffer) != 0)
  {
    return -1;
  }

  g_backend_link_context.stats.sent_fault_reports++;
  fault_service_clear();
  return 0;
}

static void backend_link_handle_command_line(const char *line)
{
  uint32_t channel;
  uint32_t value;
  uint32_t mask;
  uint32_t target_node_id = 0U;
  uint8_t has_target_node = 0U;
  control_proto_command_t command;
  relay_domain_result_t result;
  cluster_service_state_t cluster_state;
  const cluster_node_t *remote_node;

  if (line == NULL)
  {
    return;
  }

  g_backend_link_context.stats.received_commands++;
  memset(&command, 0, sizeof(command));
  memset(&result, 0, sizeof(result));
  has_target_node = (strstr(line, "\"node_id\":") != NULL) ? 1U : 0U;

  if (strstr(line, "\"type\":\"ping\"") != NULL)
  {
    (void)backend_link_send_heartbeat();
    return;
  }

  if (strstr(line, "\"type\":\"config_cloud\"") != NULL)
  {
    backend_link_apply_cloud_config(line);
    (void)backend_link_send_status();
    return;
  }

  if (has_target_node != 0U)
  {
    if (((sscanf(line, "{\"type\":\"set_relay\",\"node_id\":%u,\"channel\":%u,\"value\":%u}",
                 &target_node_id, &channel, &value) == 3) ||
         (sscanf(line, "{\"type\":\"set_relay\",\"channel\":%u,\"value\":%u,\"node_id\":%u}",
                 &channel, &value, &target_node_id) == 3)) &&
        (channel >= 1U) && (channel <= BOARD_RELAY_CHANNEL_COUNT) && (value <= 1U))
    {
      command.type = CONTROL_PROTO_CMD_SET_RELAY;
      command.channel = (uint8_t)channel;
      command.value = (uint8_t)value;
    }
    else if (((sscanf(line, "{\"type\":\"set_mask\",\"node_id\":%u,\"mask\":%u}",
                      &target_node_id, &mask) == 2) ||
              (sscanf(line, "{\"type\":\"set_mask\",\"mask\":%u,\"node_id\":%u}",
                      &mask, &target_node_id) == 2)) &&
             (mask <= BOARD_RELAY_ALL_MASK))
    {
      command.type = CONTROL_PROTO_CMD_SET_RELAY_MASK;
      command.mask = mask;
    }
    else if (((sscanf(line, "{\"type\":\"set_all\",\"node_id\":%u,\"value\":%u}",
                      &target_node_id, &value) == 2) ||
              (sscanf(line, "{\"type\":\"set_all\",\"value\":%u,\"node_id\":%u}",
                      &value, &target_node_id) == 2)) &&
             (value <= 1U))
    {
      command.type = CONTROL_PROTO_CMD_SET_RELAY_ALL;
      command.value = (uint8_t)value;
    }
    else
    {
      g_backend_link_context.stats.command_errors++;
      return;
    }
  }
  else if ((sscanf(line, "{\"type\":\"set_relay\",\"channel\":%u,\"value\":%u}",
                   &channel, &value) == 2) &&
           (channel >= 1U) && (channel <= BOARD_RELAY_CHANNEL_COUNT) && (value <= 1U))
  {
    command.type = CONTROL_PROTO_CMD_SET_RELAY;
    command.channel = (uint8_t)channel;
    command.value = (uint8_t)value;
  }
  else if ((sscanf(line, "{\"type\":\"set_mask\",\"mask\":%u}", &mask) == 1) &&
           (mask <= BOARD_RELAY_ALL_MASK))
  {
    command.type = CONTROL_PROTO_CMD_SET_RELAY_MASK;
    command.mask = mask;
  }
  else if ((sscanf(line, "{\"type\":\"set_all\",\"value\":%u}", &value) == 1) &&
           (value <= 1U))
  {
    command.type = CONTROL_PROTO_CMD_SET_RELAY_ALL;
    command.value = (uint8_t)value;
  }
  else
  {
    g_backend_link_context.stats.command_errors++;
    return;
  }

  if (target_node_id != 0U)
  {
    cluster_service_get_state(&cluster_state);
    if (cluster_state.role != CLUSTER_ROLE_MASTER)
    {
      g_backend_link_context.stats.command_errors++;
      return;
    }

    if (target_node_id != cluster_state.self_node_id)
    {
      remote_node = cluster_service_find_node(target_node_id);
      if ((remote_node == NULL) || (remote_node->ip_addr == 0U))
      {
        g_backend_link_context.stats.command_errors++;
        return;
      }

      if (control_queue_submit_remote_async(remote_node->ip_addr,
                                            remote_node->node_id,
                                            &command) != HAL_OK)
      {
        g_backend_link_context.stats.command_errors++;
        return;
      }

      (void)backend_link_send_status();
      return;
    }
  }

  if (control_queue_submit(&command, &result) != HAL_OK)
  {
    g_backend_link_context.stats.command_errors++;
    return;
  }

  if (result.error != CONTROL_PROTO_ERR_NONE)
  {
    g_backend_link_context.stats.command_errors++;
    return;
  }

  device_config_runtime_relay_mask_set(result.relay_mask);
  (void)backend_link_send_status();
}

static void backend_link_process_rx(void)
{
  int recv_len;
  int recv_errno;
  char *line_end;

  if (g_backend_link_context.sock < 0)
  {
    return;
  }

  recv_len = lwip_recv(g_backend_link_context.sock,
                       g_backend_link_context.rx_buffer + g_backend_link_context.rx_length,
                       (int)(sizeof(g_backend_link_context.rx_buffer) - g_backend_link_context.rx_length - 1U),
                       0);

  if (recv_len == 0)
  {
    backend_link_log("[backend] recv closed\r\n");
    backend_link_close_socket();
    return;
  }

  if (recv_len < 0)
  {
    recv_errno = errno;
    if ((recv_errno == EWOULDBLOCK) || (recv_errno == EAGAIN))
    {
      return;
    }

    g_backend_link_context.stats.last_error = (uint32_t)(1000 + recv_errno);
    backend_link_log_reason("[backend] recv err=", (uint32_t)recv_errno);
    backend_link_close_socket();
    return;
  }

  g_backend_link_context.last_rx_ms = HAL_GetTick();
  g_backend_link_context.rx_length += (size_t)recv_len;
  g_backend_link_context.rx_buffer[g_backend_link_context.rx_length] = '\0';

  for (;;)
  {
    line_end = strchr(g_backend_link_context.rx_buffer, '\n');
    if (line_end == NULL)
    {
      break;
    }

    *line_end = '\0';
    backend_link_handle_command_line(g_backend_link_context.rx_buffer);

    {
      size_t consumed = (size_t)(line_end - g_backend_link_context.rx_buffer) + 1U;
      memmove(g_backend_link_context.rx_buffer,
              g_backend_link_context.rx_buffer + consumed,
              g_backend_link_context.rx_length - consumed);
      g_backend_link_context.rx_length -= consumed;
      g_backend_link_context.rx_buffer[g_backend_link_context.rx_length] = '\0';
    }
  }
}

static void backend_link_try_connect(void)
{
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  u32_t nonblocking = 1U;
  const uint32_t now = HAL_GetTick();

  if ((int32_t)(now - g_backend_link_context.last_connect_attempt_ms) < (int32_t)BACKEND_LINK_CONNECT_RETRY_MS)
  {
    return;
  }

  g_backend_link_context.last_connect_attempt_ms = now;
  g_backend_link_context.stats.connect_attempts++;
  g_backend_link_context.sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (g_backend_link_context.sock < 0)
  {
    g_backend_link_context.stats.connect_failures++;
    g_backend_link_context.stats.last_error = 2U;
    backend_link_log_reason("[backend] socket fail=", (uint32_t)errno);
    return;
  }

  timeout_value.tv_sec = 1;
  timeout_value.tv_usec = 0;
  (void)lwip_setsockopt(g_backend_link_context.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(g_backend_link_context.sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(backend_link_port());
  remote_addr.sin_addr.s_addr = inet_addr(backend_link_host());

  if (remote_addr.sin_addr.s_addr == IPADDR_NONE)
  {
    g_backend_link_context.stats.connect_failures++;
    g_backend_link_context.stats.last_error = 4U;
    backend_link_log_reason("[backend] nonblock fail=", (uint32_t)errno);
    backend_link_close_socket();
    return;
  }

  if (lwip_connect(g_backend_link_context.sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    g_backend_link_context.stats.connect_failures++;
    g_backend_link_context.stats.last_error = 3U;
    backend_link_log_reason("[backend] connect fail=", (uint32_t)errno);
    backend_link_close_socket();
    return;
  }

  g_backend_link_context.connected = 1U;
  g_backend_link_context.stats.connected = 1U;
  g_backend_link_context.session_id++;
  g_backend_link_context.last_heartbeat_ms = 0U;
  g_backend_link_context.last_status_ms = 0U;
  g_backend_link_context.last_send_ok_ms = now;
  g_backend_link_context.last_rx_ms = now;
  g_backend_link_context.status_dirty = 1U;
  (void)lwip_ioctl(g_backend_link_context.sock, FIONBIO, &nonblocking);

  if (backend_link_send_hello() != 0)
  {
    g_backend_link_context.stats.connect_failures++;
    return;
  }

  (void)backend_link_send_fault_report();
  (void)backend_link_send_status();
  backend_link_log("[backend] connected\r\n");
}

static void backend_link_task(void const *argument)
{
  (void)argument;

  for (;;)
  {
    const uint32_t now = HAL_GetTick();
    const uint8_t reason = backend_link_should_run_reason();
    const uint8_t should_run = (reason == 0U) ? 1U : 0U;

    g_backend_link_context.stats.should_run = should_run;
    if ((g_backend_link_context.last_loop_log_ms == 0U) ||
        ((int32_t)(now - g_backend_link_context.last_loop_log_ms) >= 5000))
    {
      char loop_msg[96];
      (void)snprintf(loop_msg, sizeof(loop_msg),
                     "[backend] loop connected=%u dirty=%u rx=%lu txh=%lu txs=%lu hw=%lu\r\n",
                     (unsigned int)g_backend_link_context.connected,
                     (unsigned int)g_backend_link_context.status_dirty,
                     (unsigned long)g_backend_link_context.rx_length,
                     (unsigned long)g_backend_link_context.stats.sent_heartbeat,
                     (unsigned long)g_backend_link_context.stats.sent_status,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
      backend_link_log(loop_msg);
      g_backend_link_context.last_loop_log_ms = now;
    }
    if ((g_backend_link_context.last_should_run != should_run) ||
        (g_backend_link_context.last_should_run_reason != reason))
    {
      backend_link_log_reason("[backend] should_run_reason=", reason);
      g_backend_link_context.last_should_run = should_run;
      g_backend_link_context.last_should_run_reason = reason;
    }
    if (should_run == 0U)
    {
      if (g_backend_link_context.connected != 0U)
      {
        backend_link_log("[backend] disconnected\r\n");
      }
      backend_link_close_socket();
      osDelay(BACKEND_LINK_LOOP_DELAY_MS);
      continue;
    }

    if (g_backend_link_context.connected == 0U)
    {
      backend_link_try_connect();
      osDelay(BACKEND_LINK_LOOP_DELAY_MS);
      continue;
    }

    backend_link_process_rx();

    if ((g_backend_link_context.last_send_ok_ms != 0U) &&
        ((int32_t)(now - g_backend_link_context.last_send_ok_ms) >= (int32_t)BACKEND_LINK_STALE_RECONNECT_MS))
    {
      backend_link_log("[backend] stale reconnect\r\n");
      backend_link_close_socket();
      osDelay(BACKEND_LINK_LOOP_DELAY_MS);
      continue;
    }

    if (g_backend_link_context.status_dirty != 0U)
    {
      if (backend_link_send_status() == 0)
      {
        g_backend_link_context.status_dirty = 0U;
        g_backend_link_context.last_status_ms = now;
      }
    }

    if ((int32_t)(now - g_backend_link_context.last_heartbeat_ms) >= (int32_t)BACKEND_LINK_HEARTBEAT_INTERVAL_MS)
    {
      if (backend_link_send_heartbeat() == 0)
      {
        g_backend_link_context.last_heartbeat_ms = now;
      }
    }

    if ((int32_t)(now - g_backend_link_context.last_status_ms) >= (int32_t)BACKEND_LINK_STATUS_INTERVAL_MS)
    {
      if (backend_link_send_status() == 0)
      {
        g_backend_link_context.status_dirty = 0U;
        g_backend_link_context.last_status_ms = now;
      }
    }

    osDelay(BACKEND_LINK_LOOP_DELAY_MS);
  }
}

void backend_link_service_init(void)
{
  if (g_backend_link_context.started != 0U)
  {
    return;
  }

  memset(&g_backend_link_context, 0, sizeof(g_backend_link_context));
  g_backend_link_context.started = 1U;
  g_backend_link_context.sock = -1;

  osThreadDef(BackendLinkTask, backend_link_task, osPriorityBelowNormal, 0, 2048);
  g_backend_link_context.task_handle = osThreadCreate(osThread(BackendLinkTask), NULL);
}

void backend_link_service_poll(void)
{
  if (netif_is_link_up(&gnetif) == 0)
  {
    return;
  }
  backend_link_service_init();
}

void backend_link_service_get_stats(backend_link_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_backend_link_context.stats;
    stats->port = backend_link_port();
  }
}
