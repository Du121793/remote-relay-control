#include "backend_link_service.h"

#include "cluster_service.h"
#include "control_queue.h"
#include "control_proto.h"
#include "fault_service.h"
#include "../Config/device_config.h"
#include "../Platform/board_relay.h"

#include "cmsis_os.h"

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
#define BACKEND_LINK_RX_BUFFER_SIZE            512U
#define BACKEND_LINK_TX_BUFFER_SIZE            2048U

extern struct netif gnetif;

typedef struct
{
  uint8_t started;
  uint8_t connected;
  uint8_t status_dirty;
  int sock;
  uint32_t last_connect_attempt_ms;
  uint32_t last_heartbeat_ms;
  uint32_t last_status_ms;
  uint32_t last_send_ok_ms;
  uint32_t last_rx_ms;
  uint32_t session_id;
  uint32_t master_since_ms;
  osThreadId task_handle;
  backend_link_stats_t stats;
  char rx_buffer[BACKEND_LINK_RX_BUFFER_SIZE];
  char tx_buffer[BACKEND_LINK_TX_BUFFER_SIZE];
  size_t rx_length;
} backend_link_context_t;

static backend_link_context_t g_backend_link_context;

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

static uint8_t backend_link_should_run(void)
{
  cluster_service_state_t cluster_state;
  uint32_t now;

  if (backend_link_network_ready() == 0U)
  {
    g_backend_link_context.master_since_ms = 0U;
    return 0U;
  }

  cluster_service_get_state(&cluster_state);
  if ((cluster_state.cluster_enabled == 0U) ||
      (cluster_state.role != CLUSTER_ROLE_MASTER) ||
      (cluster_state.master_node_id != cluster_state.self_node_id) ||
      (backend_link_key()[0] == '\0'))
  {
    g_backend_link_context.master_since_ms = 0U;
    return 0U;
  }

  now = HAL_GetTick();
  if (g_backend_link_context.master_since_ms == 0U)
  {
    g_backend_link_context.master_since_ms = now;
    return 0U;
  }

  if ((int32_t)(now - g_backend_link_context.master_since_ms) < (int32_t)BACKEND_LINK_MASTER_STABLE_DELAY_MS)
  {
    return 0U;
  }

  return 1U;
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
  int send_len;

  if ((json_text == NULL) || (g_backend_link_context.sock < 0))
  {
    return -1;
  }

  len = strlen(json_text);
  send_len = lwip_send(g_backend_link_context.sock, json_text, (int)len, 0);
  if (send_len != (int)len)
  {
    g_backend_link_context.stats.last_error = 1U;
    backend_link_close_socket();
    return -1;
  }

  g_backend_link_context.last_send_ok_ms = HAL_GetTick();

  return 0;
}

static int backend_link_send_hello(void)
{
  char ip_text[16];
  char message[320];
  cluster_service_state_t cluster_state;
  const device_config_t *config = device_config_get();

  cluster_service_get_state(&cluster_state);
  backend_link_build_ip_text(ip_text, sizeof(ip_text));

  (void)snprintf(message, sizeof(message),
                 "{\"type\":\"hello\",\"key\":\"%s\",\"device\":\"%s\",\"cluster\":\"%s\",\"node_id\":%lu,\"master\":%u,\"ip\":\"%s\",\"session\":%lu}\n",
                 backend_link_key(),
                 config->device_name,
                 config->cluster_name,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned int)((cluster_state.role == CLUSTER_ROLE_MASTER) ? 1U : 0U),
                 ip_text,
                 (unsigned long)g_backend_link_context.session_id);

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
                 "{\"type\":\"heartbeat\",\"node_id\":%lu,\"role\":%u,\"term\":%lu}\n",
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned int)cluster_state.role,
                 (unsigned long)cluster_state.term);

  if (backend_link_send_json(message) != 0)
  {
    return -1;
  }

  g_backend_link_context.stats.sent_heartbeat++;
  return 0;
}

static int backend_link_send_status(void)
{
  char ip_text[16];
  char node_ip_text[16];
  char device_json[DEVICE_CONFIG_NAME_MAX_LEN * 2U];
  char cluster_json[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN * 2U];
  char node_name_json[DEVICE_CONFIG_NAME_MAX_LEN * 2U];
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

  used = (size_t)snprintf(g_backend_link_context.tx_buffer,
                          sizeof(g_backend_link_context.tx_buffer),
                          "{\"type\":\"status\",\"device\":\"%s\",\"cluster\":\"%s\",\"node_id\":%lu,\"master_node_id\":%lu,\"role\":%u,\"term\":%lu,\"relay_mask\":%lu,\"device_state\":%u,\"ip\":\"%s\",\"nodes\":[{\"node_id\":%lu,\"name\":\"%s\",\"ip\":\"%s\",\"role\":%u,\"online\":1,\"device_state\":%u,\"relay_mask\":%lu}",
                          device_json,
                          cluster_json,
                          (unsigned long)cluster_state.self_node_id,
                          (unsigned long)cluster_state.master_node_id,
                          (unsigned int)cluster_state.role,
                          (unsigned long)cluster_state.term,
                          (unsigned long)relay_mask,
                          (unsigned int)CLUSTER_DEVICE_STATE_RUNNING,
                          ip_text,
                          (unsigned long)cluster_state.self_node_id,
                          device_json,
                          ip_text,
                          (unsigned int)cluster_state.role,
                          (unsigned int)CLUSTER_DEVICE_STATE_RUNNING,
                          (unsigned long)relay_mask);
  if (used >= sizeof(g_backend_link_context.tx_buffer))
  {
    return -1;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    int written;

    if (nodes[index].valid == 0U)
    {
      continue;
    }

    backend_link_json_escape_copy(node_name_json, sizeof(node_name_json), nodes[index].node_name);
    backend_link_build_node_ip_text(nodes[index].ip_addr, node_ip_text, sizeof(node_ip_text));

    written = snprintf(g_backend_link_context.tx_buffer + used,
                       sizeof(g_backend_link_context.tx_buffer) - used,
                       ",{\"node_id\":%lu,\"name\":\"%s\",\"ip\":\"%s\",\"role\":%u,\"online\":%u,\"device_state\":%u,\"relay_mask\":%lu}",
                       (unsigned long)nodes[index].node_id,
                       node_name_json,
                       node_ip_text,
                       (unsigned int)nodes[index].role,
                       (unsigned int)nodes[index].online,
                       (unsigned int)nodes[index].device_state,
                       (unsigned long)nodes[index].relay_mask);
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
  return 0;
}

static int backend_link_send_fault_report(void)
{
  fault_service_record_t record;
  int written;

  if (fault_service_peek(&record) == 0U)
  {
    return 0;
  }

  written = snprintf(g_backend_link_context.tx_buffer,
                     sizeof(g_backend_link_context.tx_buffer),
                     "{\"type\":\"fault_report\",\"fault_type\":%lu,\"cfsr\":%lu,\"hfsr\":%lu,\"mmfar\":%lu,\"bfar\":%lu,\"exc_lr\":%lu,\"sp\":%lu,\"pc\":%lu,\"lr\":%lu,\"psr\":%lu,\"r0\":%lu,\"r1\":%lu,\"r2\":%lu,\"r3\":%lu,\"r12\":%lu}\n",
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
                     (unsigned long)record.r12);
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

    remote_node = cluster_service_find_node(target_node_id);
    if ((remote_node == NULL) || (remote_node->ip_addr == 0U))
    {
      g_backend_link_context.stats.command_errors++;
      return;
    }

    if (control_queue_submit_remote_async(remote_node->ip_addr, remote_node->node_id, &command) != HAL_OK)
    {
      g_backend_link_context.stats.command_errors++;
      return;
    }

    (void)backend_link_send_status();
    return;
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
    backend_link_close_socket();
    return;
  }

  if (lwip_connect(g_backend_link_context.sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    g_backend_link_context.stats.connect_failures++;
    g_backend_link_context.stats.last_error = 3U;
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
    const uint8_t should_run = backend_link_should_run();

    g_backend_link_context.stats.should_run = should_run;
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

  osThreadDef(BackendLinkTask, backend_link_task, osPriorityBelowNormal, 0, 1024);
  g_backend_link_context.task_handle = osThreadCreate(osThread(BackendLinkTask), NULL);
}

void backend_link_service_poll(void)
{
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
