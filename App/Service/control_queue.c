#include "control_queue.h"
#include "adc_service.h"
#include "mqtt_link_service.h"
#include "board_relay.h"
#include "relay_protection.h"
#include "cluster_service.h"
#include "discovery_service.h"

#include "cmsis_os.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

#include "mbedtls/gcm.h"

extern RNG_HandleTypeDef hrng;
#include "task.h"
#include "../Config/device_config.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  char backend_host[DEVICE_CONFIG_BACKEND_HOST_MAX_LEN];
  uint16_t backend_port;
  uint8_t cloud_enabled;
  uint8_t cloud_transport;
  char cloud_broker_host[DEVICE_CONFIG_CLOUD_HOST_MAX_LEN];
  uint16_t cloud_broker_port;
  uint16_t reserved;
  char cloud_cluster_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_cluster_ca_fingerprint[DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN];
  char cloud_cluster_binding_secret[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN];
} control_queue_backend_sync_t;

typedef struct
{
  uint8_t kind;
  control_proto_command_t command;
  control_queue_relay_config_t relay_config;
  char request_id[CONTROL_QUEUE_REQUEST_ID_MAX_LEN];
  relay_domain_result_t *result;
  TaskHandle_t waiting_task;
  uint32_t remote_ip_addr;
  uint32_t remote_node_id;
  uint32_t coalesce_seq;
  uint8_t remote;
} control_queue_request_t;

typedef struct
{
  uint32_t ip_addr;
  char method[CONTROL_QUEUE_REMOTE_HTTP_METHOD_MAX_LEN];
  char path[CONTROL_QUEUE_REMOTE_HTTP_PATH_MAX_LEN];
  char body[CONTROL_QUEUE_REMOTE_HTTP_BODY_MAX_LEN];
} control_queue_remote_http_request_t;

typedef struct
{
  control_queue_remote_http_request_t request;
  volatile uint8_t state;
  volatile uint8_t ok;
  uint32_t request_id;
} control_queue_remote_http_slot_t;

typedef struct
{
  char login_request[512];
  char login_response[512];
  char config_request[1792];
  char config_response[13312];
  char encoded_host[(DEVICE_CONFIG_BACKEND_HOST_MAX_LEN * 3U) + 1U];
  char encoded_cloud_host[(DEVICE_CONFIG_CLOUD_HOST_MAX_LEN * 3U) + 1U];
  char encoded_cloud_cluster_id[(DEVICE_CONFIG_CLOUD_ID_MAX_LEN * 3U) + 1U];
  char encoded_cloud_cluster_ca_fingerprint[(DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN * 3U) + 1U];
  char encoded_cloud_cluster_binding_secret[(DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN * 3U) + 1U];
  char encoded_relay_name[(DEVICE_CONFIG_NAME_MAX_LEN * 3U) + 1U];
  char body[1152];
} control_queue_backend_sync_workspace_t;

typedef struct
{
  uint32_t node_id;
  uint32_t seq[BOARD_RELAY_CHANNEL_COUNT];
} control_queue_remote_coalesce_t;

#define CONTROL_QUEUE_LENGTH             16U
#define CONTROL_QUEUE_WAIT_TIMEOUT_MS    1000U
#define CONTROL_QUEUE_REQ_COMMAND        0U
#define CONTROL_QUEUE_REQ_BACKEND_SYNC   1U
#define CONTROL_QUEUE_REQ_RELAY_CONFIG   2U
#define CONTROL_QUEUE_REQ_REMOTE_HTTP    3U
#define CONTROL_QUEUE_REMOTE_HTTP_RESPONSE_MAX_LEN 12288U
#define CONTROL_QUEUE_REMOTE_HTTP_IDLE      0U
#define CONTROL_QUEUE_REMOTE_HTTP_PREPARING 1U
#define CONTROL_QUEUE_REMOTE_HTTP_PENDING   2U
#define CONTROL_QUEUE_REMOTE_HTTP_RUNNING   3U
#define CONTROL_QUEUE_REMOTE_HTTP_DONE      4U
#define CONTROL_QUEUE_REMOTE_HTTP_SLOTS     4U
#define CONTROL_QUEUE_REMOTE_COALESCE_SLOTS 16U

static control_queue_stats_t g_control_queue_stats;
static QueueHandle_t g_control_queue_handle = NULL;
static osThreadId g_control_queue_task_handle = NULL;
static control_queue_request_t g_control_queue_worker_request;
static relay_domain_result_t g_control_queue_async_result;
static control_queue_backend_sync_t g_control_queue_backend_sync_request;
static control_queue_backend_sync_workspace_t g_control_queue_backend_sync_workspace;
static device_config_t g_control_queue_relay_config_snapshot;
static volatile uint8_t g_control_queue_backend_sync_pending = 0U;
static volatile uint8_t g_control_queue_force_stop[16];
static StaticTask_t g_control_queue_task_buffer;
static StackType_t g_control_queue_task_stack[2048] /* 8 KB — enc_buf+ct_buf+AES ctx needed >4 KB */;
static StaticSemaphore_t g_control_queue_sync_mutex_buffer;
static SemaphoreHandle_t g_control_queue_sync_mutex = NULL;
static relay_domain_result_t g_control_queue_sync_result;
static control_queue_remote_http_slot_t g_control_queue_remote_http_slots[CONTROL_QUEUE_REMOTE_HTTP_SLOTS];
static char g_control_queue_remote_http_response[CONTROL_QUEUE_REMOTE_HTTP_RESPONSE_MAX_LEN];
static uint32_t g_control_queue_remote_http_next_request_id = 1U;
static uint32_t g_control_queue_last_relay_on_ms = 0U;
static uint32_t g_control_queue_last_relay_on_by_channel_ms[BOARD_RELAY_CHANNEL_COUNT];
static uint32_t g_control_queue_next_coalesce_seq = 1U;
static uint32_t g_control_queue_local_coalesce_seq[BOARD_RELAY_CHANNEL_COUNT];
static control_queue_remote_coalesce_t g_control_queue_remote_coalesce[CONTROL_QUEUE_REMOTE_COALESCE_SLOTS];

static void control_queue_note_enqueued(void);

static int32_t control_queue_remote_http_find_slot_by_id(uint32_t request_id)
{
  uint32_t index;

  if (request_id == 0U)
  {
    return -1;
  }

  for (index = 0U; index < CONTROL_QUEUE_REMOTE_HTTP_SLOTS; ++index)
  {
    if ((g_control_queue_remote_http_slots[index].state != CONTROL_QUEUE_REMOTE_HTTP_IDLE) &&
        (g_control_queue_remote_http_slots[index].request_id == request_id))
    {
      return (int32_t)index;
    }
  }

  return -1;
}

static int32_t control_queue_remote_http_find_free_slot(void)
{
  uint32_t index;

  for (index = 0U; index < CONTROL_QUEUE_REMOTE_HTTP_SLOTS; ++index)
  {
    if (g_control_queue_remote_http_slots[index].state == CONTROL_QUEUE_REMOTE_HTTP_IDLE)
    {
      return (int32_t)index;
    }
  }

  return -1;
}

static int32_t control_queue_remote_http_select_pending_slot(void)
{
  uint32_t index;

  for (index = 0U; index < CONTROL_QUEUE_REMOTE_HTTP_SLOTS; ++index)
  {
    if ((g_control_queue_remote_http_slots[index].state == CONTROL_QUEUE_REMOTE_HTTP_RUNNING) ||
        (g_control_queue_remote_http_slots[index].state == CONTROL_QUEUE_REMOTE_HTTP_DONE))
    {
      return -1;
    }
  }

  for (index = 0U; index < CONTROL_QUEUE_REMOTE_HTTP_SLOTS; ++index)
  {
    if (g_control_queue_remote_http_slots[index].state == CONTROL_QUEUE_REMOTE_HTTP_PENDING)
    {
      return (int32_t)index;
    }
  }

  return -1;
}

static void control_queue_remote_http_kick_worker(void)
{
  control_queue_request_t request;

  if (g_control_queue_handle == NULL)
  {
    return;
  }

  memset(&request, 0, sizeof(request));
  request.kind = CONTROL_QUEUE_REQ_REMOTE_HTTP;
  if (xQueueSend(g_control_queue_handle, &request, 0U) == pdPASS)
  {
    g_control_queue_stats.submitted++;
    control_queue_note_enqueued();
  }
  else
  {
    g_control_queue_stats.queue_full++;
  }
}

static HAL_StatusTypeDef control_queue_build_service_login_body(char *body, size_t body_size)
{
  const device_config_t *config = device_config_get();
  cluster_service_state_t state;
  uint8_t time_valid = 0U;
  uint32_t unix_time;
  uint32_t auth_tag;
  int written;

  if ((body == NULL) || (body_size == 0U) || (config == NULL))
  {
    return HAL_ERROR;
  }

  if (device_config_hash_is_zero(config->cluster_password_hash) != 0U)
  {
    return HAL_ERROR;
  }

  cluster_service_get_state(&state);
  unix_time = cluster_service_get_unix_time(&time_valid);
  if ((time_valid == 0U) || (unix_time == 0U))
  {
    unix_time = state.unix_time;
  }
  if ((state.self_node_id == 0U) || (unix_time == 0U))
  {
    return HAL_ERROR;
  }

  auth_tag = device_config_compute_auth_tag(config->cluster_password_hash,
                                            unix_time,
                                            state.self_node_id,
                                            config->cluster_name);
  written = snprintf(body,
                     body_size,
                     "scope=service&node_id=%lu&unix_time=%lu&auth_tag=%lu",
                     (unsigned long)state.self_node_id,
                     (unsigned long)unix_time,
                     (unsigned long)auth_tag);
  return ((written >= 0) && ((size_t)written < body_size)) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef control_queue_send_all(int sock, const char *text)
{
  size_t length;
  size_t sent = 0U;

  if ((sock < 0) || (text == NULL))
  {
    return HAL_ERROR;
  }

  length = strlen(text);
  while (sent < length)
  {
    int send_len = lwip_send(sock, text + sent, length - sent, 0);
    if (send_len <= 0)
    {
      return HAL_ERROR;
    }
    sent += (size_t)send_len;
  }

  return HAL_OK;
}

static void control_queue_note_enqueued(void)
{
  UBaseType_t depth;

  if (g_control_queue_handle == NULL)
  {
    return;
  }

  depth = uxQueueMessagesWaiting(g_control_queue_handle);
  g_control_queue_stats.current_depth = (uint32_t)depth;
  if ((uint32_t)depth > g_control_queue_stats.high_watermark)
  {
    g_control_queue_stats.high_watermark = (uint32_t)depth;
  }
}

static void control_queue_note_dequeued(void)
{
  if (g_control_queue_handle != NULL)
  {
    g_control_queue_stats.current_depth =
      (uint32_t)uxQueueMessagesWaiting(g_control_queue_handle);
  }
}

static uint8_t control_queue_command_can_coalesce(const control_proto_command_t *command)
{
  return ((command != NULL) &&
          (command->type == CONTROL_PROTO_CMD_SET_RELAY) &&
          (command->channel >= 1U) &&
          (command->channel <= BOARD_RELAY_CHANNEL_COUNT)) ? 1U : 0U;
}

static uint32_t control_queue_alloc_coalesce_seq(void)
{
  uint32_t seq;

  seq = g_control_queue_next_coalesce_seq++;
  if (seq == 0U)
  {
    seq = g_control_queue_next_coalesce_seq++;
  }
  return seq;
}

static control_queue_remote_coalesce_t *control_queue_get_remote_coalesce_slot(uint32_t node_id)
{
  control_queue_remote_coalesce_t *empty_slot = NULL;
  uint32_t index;

  if (node_id == 0U)
  {
    return NULL;
  }

  for (index = 0U; index < CONTROL_QUEUE_REMOTE_COALESCE_SLOTS; ++index)
  {
    if (g_control_queue_remote_coalesce[index].node_id == node_id)
    {
      return &g_control_queue_remote_coalesce[index];
    }
    if ((g_control_queue_remote_coalesce[index].node_id == 0U) && (empty_slot == NULL))
    {
      empty_slot = &g_control_queue_remote_coalesce[index];
    }
  }

  if (empty_slot != NULL)
  {
    empty_slot->node_id = node_id;
    memset(empty_slot->seq, 0, sizeof(empty_slot->seq));
  }
  return empty_slot;
}

static uint32_t control_queue_mark_latest_relay_command(uint8_t remote,
                                                        uint32_t node_id,
                                                        uint8_t channel,
                                                        uint32_t *previous_seq)
{
  uint32_t seq = 0U;

  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    return 0U;
  }

  taskENTER_CRITICAL();
  seq = control_queue_alloc_coalesce_seq();
  if (remote != 0U)
  {
    control_queue_remote_coalesce_t *slot =
      control_queue_get_remote_coalesce_slot(node_id);

    if (slot != NULL)
    {
      if (previous_seq != NULL)
      {
        *previous_seq = slot->seq[channel - 1U];
      }
      slot->seq[channel - 1U] = seq;
    }
    else
    {
      seq = 0U;
    }
  }
  else
  {
    if (previous_seq != NULL)
    {
      *previous_seq = g_control_queue_local_coalesce_seq[channel - 1U];
    }
    g_control_queue_local_coalesce_seq[channel - 1U] = seq;
  }
  taskEXIT_CRITICAL();

  return seq;
}

static void control_queue_restore_latest_relay_command(uint8_t remote,
                                                       uint32_t node_id,
                                                       uint8_t channel,
                                                       uint32_t failed_seq,
                                                       uint32_t previous_seq)
{
  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    return;
  }

  taskENTER_CRITICAL();
  if (remote != 0U)
  {
    control_queue_remote_coalesce_t *slot =
      control_queue_get_remote_coalesce_slot(node_id);

    if (slot != NULL)
    {
      if (slot->seq[channel - 1U] == failed_seq)
      {
        slot->seq[channel - 1U] = previous_seq;
      }
    }
  }
  else
  {
    if (g_control_queue_local_coalesce_seq[channel - 1U] == failed_seq)
    {
      g_control_queue_local_coalesce_seq[channel - 1U] = previous_seq;
    }
  }
  taskEXIT_CRITICAL();
}

static uint8_t control_queue_request_is_stale(const control_queue_request_t *request)
{
  uint32_t latest = 0U;
  uint8_t stale = 0U;

  if ((request == NULL) ||
      (request->coalesce_seq == 0U) ||
      (control_queue_command_can_coalesce(&request->command) == 0U))
  {
    return 0U;
  }

  taskENTER_CRITICAL();
  if (request->remote != 0U)
  {
    control_queue_remote_coalesce_t *slot =
      control_queue_get_remote_coalesce_slot(request->remote_node_id);

    if (slot != NULL)
    {
      latest = slot->seq[request->command.channel - 1U];
    }
  }
  else
  {
    latest = g_control_queue_local_coalesce_seq[request->command.channel - 1U];
  }
  stale = (latest != request->coalesce_seq) ? 1U : 0U;
  taskEXIT_CRITICAL();

  return stale;
}

static size_t control_queue_url_encode(const char *src, char *dst, size_t dst_size)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t si = 0U;
  size_t di = 0U;

  if ((src == NULL) || (dst == NULL) || (dst_size == 0U))
  {
    return 0U;
  }

  while ((src[si] != '\0') && (di + 1U < dst_size))
  {
    unsigned char ch = (unsigned char)src[si++];
    if (((ch >= 'a') && (ch <= 'z')) ||
        ((ch >= 'A') && (ch <= 'Z')) ||
        ((ch >= '0') && (ch <= '9')) ||
        (ch == '-') || (ch == '_') || (ch == '.') || (ch == '~'))
    {
      dst[di++] = (char)ch;
    }
    else
    {
      if ((di + 3U) >= dst_size)
      {
        break;
      }
      dst[di++] = '%';
      dst[di++] = hex[(ch >> 4) & 0x0FU];
      dst[di++] = hex[ch & 0x0FU];
    }
  }

  dst[di] = '\0';
  return di;
}

/* ── Config-sync AES-128-GCM encrypt / decrypt ──────────────────
   Key = first 16 bytes of cluster_password_hash (must match on both
   master and member for the cluster to be authenticated anyway). */

size_t control_queue_encrypt_body(const char *plain_body, size_t plain_len,
                                   const uint8_t key[16],
                                   char *enc_out, size_t enc_out_size)
{
  uint8_t iv[12], tag[16]; static uint8_t ct_buf[1536];
  size_t ct_len = plain_len, out_pos = 0U;
  mbedtls_gcm_context ctx;
  int ret; uint32_t i;

  if (!plain_body || !key || !enc_out || plain_len > sizeof(ct_buf)) return 0U;
  for (i = 0U; i < 12U; ++i) {
    uint32_t r = 0U;
    if (HAL_RNG_GenerateRandomNumber(&hrng, &r) == HAL_OK) iv[i] = (uint8_t)r;
    else iv[i] = (uint8_t)(HAL_GetTick() >> (i & 7U));
  }
  mbedtls_gcm_init(&ctx);
  ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128U);
  if (ret) { mbedtls_gcm_free(&ctx); return 0U; }
  ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain_len,
                                  iv, 12U, NULL, 0,
                                  (const unsigned char *)plain_body, ct_buf,
                                  16U, tag);
  mbedtls_gcm_free(&ctx);
  if (ret) return 0U;
  if (enc_out_size < (12U + plain_len + 16U)) return 0U;
  memcpy(enc_out, iv, 12U);
  memcpy(enc_out + 12U, ct_buf, plain_len);
  memcpy(enc_out + 12U + plain_len, tag, 16U);
  return 12U + plain_len + 16U;
}

size_t control_queue_decrypt_body(const char *enc_body, size_t enc_len,
                                   const uint8_t key[16],
                                   char *plain_out, size_t plain_out_size)
{
  mbedtls_gcm_context ctx;
  const uint8_t *iv, *tag, *ct;
  size_t ct_len; int ret;
  if (!enc_body || !key || !plain_out || enc_len < 28U) return 0U;
  iv  = (const uint8_t *)enc_body;
  tag = (const uint8_t *)(enc_body + enc_len - 16U);
  ct  = (const uint8_t *)(enc_body + 12U);
  ct_len = enc_len - 28U;
  if (ct_len > plain_out_size) return 0U;
  mbedtls_gcm_init(&ctx);
  ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128U);
  if (ret) { mbedtls_gcm_free(&ctx); return 0U; }
  ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len, iv, 12U, NULL, 0,
                                 tag, 16U, ct, (unsigned char *)plain_out);
  mbedtls_gcm_free(&ctx);
  if (ret) return 0U;
  plain_out[ct_len] = '\0';
  return ct_len;
}

static uint8_t control_queue_http_response_complete(const char *response, size_t used)
{
  static const char content_length_header[] = "\r\nContent-Length:";
  const char *header_end;
  const char *content_length_text;
  char *endptr = NULL;
  unsigned long content_length;
  size_t header_length;

  if ((response == NULL) || (used == 0U))
  {
    return 0U;
  }

  header_end = strstr(response, "\r\n\r\n");
  if (header_end == NULL)
  {
    return 0U;
  }

  content_length_text = strstr(response, content_length_header);
  if ((content_length_text == NULL) || (content_length_text >= header_end))
  {
    return 0U;
  }

  content_length_text += sizeof(content_length_header) - 1U;
  content_length = strtoul(content_length_text, &endptr, 10);
  if ((endptr == content_length_text) || (endptr > header_end))
  {
    return 0U;
  }

  header_length = (size_t)(header_end - response) + 4U;
  return (used >= (header_length + (size_t)content_length)) ? 1U : 0U;
}

static HAL_StatusTypeDef control_queue_http_request(uint32_t ip_addr,
                                                    const char *request_text,
                                                    char *response,
                                                    size_t response_size)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  int recv_len;
  size_t used = 0U;
  uint8_t complete = 0U;

  if ((ip_addr == 0U) || (request_text == NULL) || (response == NULL) || (response_size < 2U))
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 1;
  timeout_value.tv_usec = 0;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(80U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (control_queue_send_all(sock, request_text) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  response[0] = '\0';
  for (;;)
  {
    recv_len = lwip_recv(sock, response + used, (int)(response_size - used - 1U), 0);
    if (recv_len <= 0)
    {
      break;
    }

    used += (size_t)recv_len;
    response[used] = '\0';
    if (control_queue_http_response_complete(response, used) != 0U)
    {
      complete = 1U;
      break;
    }
    if (used >= (response_size - 1U))
    {
      break;
    }
  }

  response[used] = '\0';
  lwip_close(sock);
  return (complete != 0U) ? HAL_OK : HAL_ERROR;
}

__attribute__((noinline)) static HAL_StatusTypeDef control_queue_sync_remote_backend_config(
  uint32_t ip_addr,
  const control_queue_backend_sync_t *config)
{
  control_queue_backend_sync_workspace_t *workspace = &g_control_queue_backend_sync_workspace;
  uint32_t token = 0U;
  char *token_text;
  char *endptr = NULL;

  if ((ip_addr == 0U) || (config == NULL))
  {
    return HAL_ERROR;
  }

  if (control_queue_build_service_login_body(workspace->body,
                                             sizeof(workspace->body)) != HAL_OK)
  {
    return HAL_ERROR;
  }
  (void)snprintf(workspace->login_request, sizeof(workspace->login_request),
                 "POST /api/login HTTP/1.1\r\n"
                 "Host: remote\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 (unsigned int)strlen(workspace->body),
                 workspace->body);

  if (control_queue_http_request(ip_addr,
                                 workspace->login_request,
                                 workspace->login_response,
                                 sizeof(workspace->login_response)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  token_text = strstr(workspace->login_response, "\"token\":");
  if (token_text == NULL)
  {
    return HAL_ERROR;
  }

  token = (uint32_t)strtoul(token_text + 8, &endptr, 10);
  if ((endptr == NULL) || (token == 0U))
  {
    return HAL_ERROR;
  }

  control_queue_url_encode(config->backend_host,
                           workspace->encoded_host,
                           sizeof(workspace->encoded_host));
  control_queue_url_encode(config->cloud_broker_host,
                           workspace->encoded_cloud_host,
                           sizeof(workspace->encoded_cloud_host));
  control_queue_url_encode(config->cloud_cluster_id,
                           workspace->encoded_cloud_cluster_id,
                           sizeof(workspace->encoded_cloud_cluster_id));
  control_queue_url_encode(config->cloud_cluster_ca_fingerprint,
                           workspace->encoded_cloud_cluster_ca_fingerprint,
                           sizeof(workspace->encoded_cloud_cluster_ca_fingerprint));
  control_queue_url_encode(config->cloud_cluster_binding_secret,
                           workspace->encoded_cloud_cluster_binding_secret,
                           sizeof(workspace->encoded_cloud_cluster_binding_secret));
  (void)snprintf(workspace->body, sizeof(workspace->body),
                 "backend_host=%s&backend_port=%u"
                 "&cloud_enabled=%u&cloud_transport=%u"
                 "&cloud_broker_host=%s&cloud_broker_port=%u"
                 "&cloud_cluster_id=%s&cloud_cluster_ca_fingerprint=%s"
                 "&cloud_cluster_binding_secret=%s&cloud_cluster_sync_only=1",
                 workspace->encoded_host,
                 (unsigned int)config->backend_port,
                 (unsigned int)config->cloud_enabled,
                 (unsigned int)config->cloud_transport,
                 workspace->encoded_cloud_host,
                 (unsigned int)config->cloud_broker_port,
                 workspace->encoded_cloud_cluster_id,
                 workspace->encoded_cloud_cluster_ca_fingerprint,
                 workspace->encoded_cloud_cluster_binding_secret);

  /* Encrypt the config-sync body with AES-128-GCM when a cluster password
     is set, so the binding_secret never traverses the LAN in plaintext. */
  {
    const device_config_t *dc = device_config_get();
    if ((dc != NULL) && (dc->cluster_password_hash[0] != 0U))
    {
      uint8_t key[16];
      static char enc_buf[2048];
      size_t enc_len;
      memcpy(key, dc->cluster_password_hash, sizeof(key));
      enc_len = control_queue_encrypt_body(workspace->body,
                                           strlen(workspace->body),
                                           key, enc_buf, sizeof(enc_buf));
      if (enc_len > 0U)
      {
        static const char b64[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        size_t out = (size_t)snprintf(workspace->body, sizeof(workspace->body),
                                      "_enc=1&_data=");
        size_t si = 0U;
        while (si < enc_len && (out + 3U) < sizeof(workspace->body))
        {
          uint32_t triple = (uint32_t)((unsigned char)enc_buf[si]) << 16;
          if (si + 1U < enc_len) triple |= (uint32_t)((unsigned char)enc_buf[si+1U]) << 8;
          if (si + 2U < enc_len) triple |= (uint32_t)((unsigned char)enc_buf[si+2U]);
          workspace->body[out++] = b64[(triple >> 18) & 0x3F];
          workspace->body[out++] = b64[(triple >> 12) & 0x3F];
          workspace->body[out++] = (si + 1U < enc_len) ? b64[(triple >> 6) & 0x3F] : '=';
          workspace->body[out++] = (si + 2U < enc_len) ? b64[triple & 0x3F] : '=';
          si += 3U;
        }
        workspace->body[out] = '\0';
      }
    }
  }

  (void)snprintf(workspace->config_request, sizeof(workspace->config_request),
                 "POST /api/config/cluster HTTP/1.1\r\n"
                 "Host: remote\r\n"
                 "X-Auth-Token: %lu\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 (unsigned long)token,
                 (unsigned int)strlen(workspace->body),
                 workspace->body);

  if (control_queue_http_request(ip_addr,
                                 workspace->config_request,
                                 workspace->config_response,
                                 sizeof(workspace->config_response)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if ((strstr(workspace->config_response, "200 OK") == NULL) ||
      (strstr(workspace->config_response, "\"ok\":true") == NULL))
  {
    return HAL_ERROR;
  }

  return HAL_OK;
}

__attribute__((noinline)) static HAL_StatusTypeDef control_queue_apply_local_relay_config(
  const control_queue_relay_config_t *relay_config)
{
  device_config_t *config = &g_control_queue_relay_config_snapshot;
  device_config_relay_t *relay;
  device_config_current_t *current;

  if ((relay_config == NULL) ||
      (relay_config->channel == 0U) ||
      (relay_config->channel > DEVICE_CONFIG_RELAY_COUNT) ||
      (relay_config->fields == 0U))
  {
    return HAL_ERROR;
  }

  *config = *device_config_get();
  relay = &config->relay[relay_config->channel - 1U];
  current = &config->current[relay_config->channel - 1U];
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_NAME_PRESENT) != 0U)
  {
    if (relay_config->name[0] == '\0')
    {
      return HAL_ERROR;
    }
    memset(relay->name, 0, sizeof(relay->name));
    strncpy(relay->name, relay_config->name, sizeof(relay->name) - 1U);
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_ENABLED_PRESENT) != 0U)
  {
    relay->enabled = relay_config->enabled;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_REMEMBER_PRESENT) != 0U)
  {
    relay->remember_state = relay_config->remember_state;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_BOOT_PRESENT) != 0U)
  {
    relay->boot_default_on = relay_config->boot_default_on;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_ADC_PRESENT) != 0U)
  {
    (void)relay_config->adc_channel;
  }
  current->adc_channel = (uint8_t)(relay_config->channel - 1U);
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_PROTECTION_PRESENT) != 0U)
  {
    if (relay_config->protection_enabled > 1U)
    {
      return HAL_ERROR;
    }
    current->protection_enabled = relay_config->protection_enabled;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_THRESHOLD_PRESENT) != 0U)
  {
    if ((relay_config->overcurrent_milliamps < 100U) ||
        (relay_config->overcurrent_milliamps > 100000U))
    {
      return HAL_ERROR;
    }
    current->overcurrent_milliamps = relay_config->overcurrent_milliamps;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_DELAY_PRESENT) != 0U)
  {
    if (relay_config->trip_delay_ms > 600000U)
    {
      return HAL_ERROR;
    }
    current->trip_delay_ms = relay_config->trip_delay_ms;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_HYSTERESIS_PRESENT) != 0U)
  {
    if (relay_config->reset_hysteresis_milliamps > 100000U)
    {
      return HAL_ERROR;
    }
    current->reset_hysteresis_milliamps =
      relay_config->reset_hysteresis_milliamps;
  }
  if (current->reset_hysteresis_milliamps >=
      current->overcurrent_milliamps)
  {
    return HAL_ERROR;
  }

  return (device_config_update_and_save(config) != 0U) ? HAL_OK : HAL_ERROR;
}

__attribute__((noinline)) static HAL_StatusTypeDef control_queue_apply_remote_relay_config(
  uint32_t ip_addr,
  const control_queue_relay_config_t *relay_config)
{
  control_queue_backend_sync_workspace_t *workspace = &g_control_queue_backend_sync_workspace;
  uint32_t token;
  char *token_text;
  char *endptr = NULL;
  size_t used;
  int written;

  if ((ip_addr == 0U) ||
      (relay_config == NULL) ||
      (relay_config->channel == 0U) ||
      (relay_config->channel > DEVICE_CONFIG_RELAY_COUNT) ||
      (relay_config->fields == 0U))
  {
    return HAL_ERROR;
  }

  if (control_queue_build_service_login_body(workspace->body,
                                             sizeof(workspace->body)) != HAL_OK)
  {
    return HAL_ERROR;
  }
  (void)snprintf(workspace->login_request, sizeof(workspace->login_request),
                 "POST /api/login HTTP/1.1\r\n"
                 "Host: remote\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 (unsigned int)strlen(workspace->body),
                 workspace->body);
  if (control_queue_http_request(ip_addr,
                                 workspace->login_request,
                                 workspace->login_response,
                                 sizeof(workspace->login_response)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  token_text = strstr(workspace->login_response, "\"token\":");
  if (token_text == NULL)
  {
    return HAL_ERROR;
  }
  token = (uint32_t)strtoul(token_text + 8, &endptr, 10);
  if ((endptr == (token_text + 8)) || (token == 0U))
  {
    return HAL_ERROR;
  }

  written = snprintf(workspace->body, sizeof(workspace->body),
                     "channel=%u", (unsigned int)relay_config->channel);
  if ((written <= 0) || ((size_t)written >= sizeof(workspace->body)))
  {
    return HAL_ERROR;
  }
  used = (size_t)written;

  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_NAME_PRESENT) != 0U)
  {
    control_queue_url_encode(relay_config->name,
                             workspace->encoded_relay_name,
                             sizeof(workspace->encoded_relay_name));
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&name=%s", workspace->encoded_relay_name);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_ENABLED_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&enabled=%u", (unsigned int)relay_config->enabled);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_REMEMBER_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&remember_state=%u",
                       (unsigned int)relay_config->remember_state);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_BOOT_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&boot_default_on=%u",
                       (unsigned int)relay_config->boot_default_on);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_ADC_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&adc_channel=%u",
                       (unsigned int)relay_config->adc_channel);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_PROTECTION_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&protection_enabled=%u",
                       (unsigned int)relay_config->protection_enabled);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_THRESHOLD_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&overcurrent_milliamps=%lu",
                       (unsigned long)relay_config->overcurrent_milliamps);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_DELAY_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&trip_delay_ms=%lu",
                       (unsigned long)relay_config->trip_delay_ms);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
    used += (size_t)written;
  }
  if ((relay_config->fields & CONTROL_QUEUE_RELAY_CONFIG_HYSTERESIS_PRESENT) != 0U)
  {
    written = snprintf(workspace->body + used, sizeof(workspace->body) - used,
                       "&reset_hysteresis_milliamps=%lu",
                       (unsigned long)relay_config->reset_hysteresis_milliamps);
    if ((written <= 0) || ((size_t)written >= (sizeof(workspace->body) - used)))
    {
      return HAL_ERROR;
    }
  }

  (void)snprintf(workspace->config_request, sizeof(workspace->config_request),
                 "POST /api/config/relay HTTP/1.1\r\n"
                 "Host: remote\r\n"
                 "X-Auth-Token: %lu\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 (unsigned long)token,
                 (unsigned int)strlen(workspace->body),
                 workspace->body);
  if (control_queue_http_request(ip_addr,
                                 workspace->config_request,
                                 workspace->config_response,
                                 sizeof(workspace->config_response)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if ((strstr(workspace->config_response, "200 OK") == NULL) ||
      (strstr(workspace->config_response, "\"ok\":true") == NULL))
  {
    return HAL_ERROR;
  }

  return HAL_OK;
}

static HAL_StatusTypeDef control_queue_extract_http_body(const char *response,
                                                         char *body,
                                                         size_t body_size)
{
  const char *body_start;
  size_t body_len;

  if ((response == NULL) || (body == NULL) || (body_size == 0U))
  {
    return HAL_ERROR;
  }

  body_start = strstr(response, "\r\n\r\n");
  if (body_start == NULL)
  {
    return HAL_ERROR;
  }
  body_start += 4;
  body_len = strlen(body_start);
  if (body_len >= body_size)
  {
    body_len = body_size - 1U;
  }
  memcpy(body, body_start, body_len);
  body[body_len] = '\0';
  return HAL_OK;
}

static HAL_StatusTypeDef control_queue_remote_http_login(uint32_t ip_addr,
                                                         uint32_t *token)
{
  control_queue_backend_sync_workspace_t *workspace = &g_control_queue_backend_sync_workspace;
  char *token_text;
  char *endptr = NULL;
  int written;

  if ((ip_addr == 0U) || (token == NULL))
  {
    return HAL_ERROR;
  }

  if (control_queue_build_service_login_body(workspace->body,
                                             sizeof(workspace->body)) != HAL_OK)
  {
    return HAL_ERROR;
  }
  written = snprintf(workspace->login_request, sizeof(workspace->login_request),
                     "POST /api/login HTTP/1.1\r\n"
                     "Host: remote\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     (unsigned int)strlen(workspace->body),
                     workspace->body);
  if ((written < 0) || ((size_t)written >= sizeof(workspace->login_request)))
  {
    return HAL_ERROR;
  }

  if (control_queue_http_request(ip_addr,
                                 workspace->login_request,
                                 workspace->login_response,
                                 sizeof(workspace->login_response)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  token_text = strstr(workspace->login_response, "\"token\":");
  if (token_text == NULL)
  {
    return HAL_ERROR;
  }
  *token = (uint32_t)strtoul(token_text + 8, &endptr, 10);
  if ((endptr == (token_text + 8)) || (*token == 0U))
  {
    return HAL_ERROR;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef control_queue_execute_remote_http(uint32_t ip_addr,
                                                           const char *method,
                                                           const char *path,
                                                           const char *body,
                                                           char *response_body,
                                                           size_t response_body_size)
{
  control_queue_backend_sync_workspace_t *workspace = &g_control_queue_backend_sync_workspace;
  uint32_t token = 0U;
  const char *request_body = (body != NULL) ? body : "";
  int written;

  if ((ip_addr == 0U) || (method == NULL) || (path == NULL) ||
      (response_body == NULL) || (response_body_size == 0U) ||
      (method[0] == '\0') || (path[0] != '/'))
  {
    return HAL_ERROR;
  }

  if (control_queue_remote_http_login(ip_addr, &token) != HAL_OK)
  {
    return HAL_ERROR;
  }

  written = snprintf(workspace->config_request, sizeof(workspace->config_request),
                     "%s %s HTTP/1.1\r\n"
                     "Host: remote\r\n"
                     "X-Auth-Token: %lu\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     method,
                     path,
                     (unsigned long)token,
                     (unsigned int)strlen(request_body),
                     request_body);
  if ((written < 0) || ((size_t)written >= sizeof(workspace->config_request)))
  {
    return HAL_ERROR;
  }

  if (control_queue_http_request(ip_addr,
                                 workspace->config_request,
                                 workspace->config_response,
                                 sizeof(workspace->config_response)) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (strstr(workspace->config_response, "401 Unauthorized") != NULL)
  {
    if (control_queue_remote_http_login(ip_addr, &token) != HAL_OK)
    {
      return HAL_ERROR;
    }
    written = snprintf(workspace->config_request, sizeof(workspace->config_request),
                       "%s %s HTTP/1.1\r\n"
                       "Host: remote\r\n"
                       "X-Auth-Token: %lu\r\n"
                       "Content-Type: application/x-www-form-urlencoded\r\n"
                       "Content-Length: %u\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       method,
                       path,
                       (unsigned long)token,
                       (unsigned int)strlen(request_body),
                       request_body);
    if ((written < 0) || ((size_t)written >= sizeof(workspace->config_request)))
    {
      return HAL_ERROR;
    }
    if (control_queue_http_request(ip_addr,
                                   workspace->config_request,
                                   workspace->config_response,
                                   sizeof(workspace->config_response)) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  if (control_queue_extract_http_body(workspace->config_response,
                                      response_body,
                                      response_body_size) != HAL_OK)
  {
    return HAL_ERROR;
  }

  return (strstr(workspace->config_response, "200 OK") != NULL) ? HAL_OK : HAL_ERROR;
}

static void control_queue_wait_relay_on_spacing(uint8_t channel)
{
  relay_protection_user_config_t cfg;
  uint32_t now = HAL_GetTick();
  uint32_t wait_ms = 0U;
  uint32_t relay_spacing_ms;
  uint32_t same_relay_spacing_ms;

  relay_protection_get_user_config(&cfg);
  relay_spacing_ms = cfg.relay_on_spacing_ms;
  same_relay_spacing_ms = cfg.same_relay_on_spacing_ms;

  if (g_control_queue_last_relay_on_ms != 0U)
  {
    uint32_t elapsed = now - g_control_queue_last_relay_on_ms;

    if (elapsed < relay_spacing_ms)
    {
      wait_ms = relay_spacing_ms - elapsed;
    }
  }

  if ((channel >= 1U) && (channel <= BOARD_RELAY_CHANNEL_COUNT))
  {
    uint32_t last_same = g_control_queue_last_relay_on_by_channel_ms[channel - 1U];
    if (last_same != 0U)
    {
      uint32_t elapsed_same = now - last_same;
      if (elapsed_same < same_relay_spacing_ms)
      {
        uint32_t same_wait = same_relay_spacing_ms - elapsed_same;
        if (same_wait > wait_ms)
        {
          wait_ms = same_wait;
        }
      }
    }
  }

  if (wait_ms > 0U)
  {
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    now = HAL_GetTick();
  }

  g_control_queue_last_relay_on_ms = now;
  if ((channel >= 1U) && (channel <= BOARD_RELAY_CHANNEL_COUNT))
  {
    g_control_queue_last_relay_on_by_channel_ms[channel - 1U] = now;
  }
}

static uint8_t control_queue_validate_relay_on(uint8_t channel,
                                               relay_domain_result_t *result)
{
  uint32_t fault_mask = relay_protection_fault_lock_mask();

  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    if (result != NULL)
    {
      result->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
      result->relay_mask = board_relay_snapshot();
    }
    return 0U;
  }

  if (relay_protection_boot_safety_active(HAL_GetTick()) ||
      (((fault_mask >> (channel - 1U)) & 1U) != 0U) ||
      relay_protection_is_cooldown(channel, HAL_GetTick()))
  {
    if (result != NULL)
    {
      result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
      result->relay_mask = board_relay_snapshot();
    }
    return 0U;
  }

  return 1U;
}

static void control_queue_execute_local_relay_command(const control_proto_command_t *command,
                                                      relay_domain_result_t *result)
{
  uint32_t current_mask;
  uint32_t target_mask;
  uint32_t new_on_mask;
  uint32_t off_first_mask;
  uint8_t ch;

  if (result == NULL)
  {
    return;
  }

  result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
  result->relay_mask = board_relay_snapshot();

  if (command == NULL)
  {
    return;
  }

  if (command->type == CONTROL_PROTO_CMD_SET_RELAY)
  {
    if (command->value != 0U)
    {
      uint32_t bit;
      if (control_queue_validate_relay_on(command->channel, result) == 0U)
      {
        return;
      }
      bit = 1UL << (command->channel - 1U);
      if ((board_relay_snapshot() & bit) == 0U)
      {
        control_queue_wait_relay_on_spacing(command->channel);
      }
    }
    relay_domain_execute(command, result);
    return;
  }

  if ((command->type != CONTROL_PROTO_CMD_SET_RELAY_MASK) &&
      (command->type != CONTROL_PROTO_CMD_SET_RELAY_ALL))
  {
    relay_domain_execute(command, result);
    return;
  }

  current_mask = board_relay_snapshot() & BOARD_RELAY_ALL_MASK;
  target_mask = (command->type == CONTROL_PROTO_CMD_SET_RELAY_MASK)
                  ? (command->mask & BOARD_RELAY_ALL_MASK)
                  : ((command->value != 0U) ? BOARD_RELAY_ALL_MASK : 0U);
  new_on_mask = target_mask & ~current_mask;

  for (ch = 1U; ch <= BOARD_RELAY_CHANNEL_COUNT; ++ch)
  {
    if (((new_on_mask >> (ch - 1U)) & 1U) != 0U)
    {
      if (control_queue_validate_relay_on(ch, result) == 0U)
      {
        return;
      }
    }
  }

  off_first_mask = current_mask & target_mask;
  if (board_relay_apply_mask(off_first_mask) != HAL_OK)
  {
    result->error = CONTROL_PROTO_ERR_BAD_VALUE;
    result->relay_mask = board_relay_snapshot();
    return;
  }

  for (ch = 1U; ch <= BOARD_RELAY_CHANNEL_COUNT; ++ch)
  {
    if (((new_on_mask >> (ch - 1U)) & 1U) != 0U)
    {
      control_queue_wait_relay_on_spacing(ch);
      if (board_relay_set(ch, GPIO_PIN_SET) != HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
        result->relay_mask = board_relay_snapshot();
        return;
      }
    }
  }

  result->error = CONTROL_PROTO_ERR_NONE;
  result->relay_mask = board_relay_snapshot();
}

static void control_queue_format_request(const control_proto_command_t *command,
                                         char *buffer,
                                         size_t buffer_size)
{
  if ((command == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return;
  }

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_PING:
      (void)snprintf(buffer, buffer_size, "PING\r\n");
      break;

    case CONTROL_PROTO_CMD_GET_RELAY:
      (void)snprintf(buffer, buffer_size, "GET RELAY\r\n");
      break;

    case CONTROL_PROTO_CMD_SET_RELAY:
      (void)snprintf(buffer, buffer_size, "SET RELAY %u %u\r\n",
                     (unsigned int)command->channel,
                     (unsigned int)command->value);
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
      (void)snprintf(buffer, buffer_size, "SET RELAY MASK 0x%04lX\r\n",
                     (unsigned long)command->mask);
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      (void)snprintf(buffer, buffer_size, "SET RELAY ALL %u\r\n",
                     (unsigned int)command->value);
      break;

    case CONTROL_PROTO_CMD_INVALID:
    default:
      buffer[0] = '\0';
      break;
  }
}

static void control_queue_parse_remote_response(const char *response, relay_domain_result_t *result)
{
  const char *relay_text;
  char *endptr = NULL;
  unsigned long relay_value;

  if ((response == NULL) || (result == NULL))
  {
    return;
  }

  memset(result, 0, sizeof(*result));
  result->error = CONTROL_PROTO_ERR_BAD_COMMAND;

  if (strncmp(response, "OK ", 3) == 0)
  {
    relay_text = strstr(response, "relay=0x");
    if (relay_text != NULL)
    {
      relay_value = strtoul(relay_text + 8, &endptr, 16);
      if (endptr != NULL)
      {
        result->relay_mask = (uint32_t)relay_value;
      }
    }
    result->error = CONTROL_PROTO_ERR_NONE;
    return;
  }

  if (strstr(response, "ERR code=bad_channel") != NULL)
  {
    result->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
  }
  else if (strstr(response, "ERR code=bad_value") != NULL)
  {
    result->error = CONTROL_PROTO_ERR_BAD_VALUE;
  }
}

static HAL_StatusTypeDef control_queue_recv_and_parse(int sock, relay_domain_result_t *result)
{
  char response[256];
  int response_len;

  if (result == NULL)
  {
    return HAL_ERROR;
  }

  response_len = lwip_recv(sock, response, sizeof(response) - 1, 0);
  if (response_len <= 0)
  {
    return HAL_ERROR;
  }

  response[response_len] = '\0';
  control_queue_parse_remote_response(response, result);
  return (result->error == CONTROL_PROTO_ERR_NONE) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef control_queue_fetch_remote_relay_mask(uint32_t ip_addr, relay_domain_result_t *result)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  static const char request[] = "GET RELAY\r\n";

  if ((ip_addr == 0U) || (result == NULL))
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 1;
  timeout_value.tv_usec = 0;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(41000U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (control_queue_send_all(sock, request) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (control_queue_recv_and_parse(sock, result) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  lwip_close(sock);
  return HAL_OK;
}

__attribute__((noinline)) static HAL_StatusTypeDef control_queue_execute_remote_async_fast(
  uint32_t ip_addr,
  const control_proto_command_t *command)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  char request[64];
  relay_domain_result_t result;

  if ((command == NULL) || (ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  control_queue_format_request(command, request, sizeof(request));
  if (request[0] == '\0')
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 0;
  timeout_value.tv_usec = 250000;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(41000U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (control_queue_send_all(sock, request) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  memset(&result, 0, sizeof(result));
  result.error = CONTROL_PROTO_ERR_NONE;
  if (control_queue_recv_and_parse(sock, &result) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  lwip_close(sock);
  return HAL_OK;
}

__attribute__((noinline)) static HAL_StatusTypeDef control_queue_execute_remote(
  uint32_t ip_addr,
  const control_proto_command_t *command,
  relay_domain_result_t *result)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  char request[64];
  HAL_StatusTypeDef recv_status;

  if ((command == NULL) || (result == NULL) || (ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  control_queue_format_request(command, request, sizeof(request));
  if (request[0] == '\0')
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 1;
  timeout_value.tv_usec = 0;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(41000U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (control_queue_send_all(sock, request) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  memset(result, 0, sizeof(*result));
  result->error = CONTROL_PROTO_ERR_NONE;

  recv_status = control_queue_recv_and_parse(sock, result);
  lwip_close(sock);

  if (recv_status == HAL_OK)
  {
    return HAL_OK;
  }

  if ((command->type == CONTROL_PROTO_CMD_SET_RELAY) ||
      (command->type == CONTROL_PROTO_CMD_SET_RELAY_MASK) ||
      (command->type == CONTROL_PROTO_CMD_SET_RELAY_ALL))
  {
    memset(result, 0, sizeof(*result));
    result->error = CONTROL_PROTO_ERR_NONE;
    if (control_queue_fetch_remote_relay_mask(ip_addr, result) == HAL_OK)
    {
      return HAL_OK;
    }
    return HAL_ERROR;
  }

  return HAL_ERROR;
}

static void control_queue_worker_task(void const *argument)
{
  (void)argument;

  for (;;)
  {
    uint8_t ch;

    /* Phase A: Check force-stop flags before processing queued commands.
       Force-stop: turn relay OFF immediately, set 3s cooldown.
       Pending ON commands for this channel will be rejected by the
       cooldown check below — no need to flush the queue. */
    for (ch = 0U; ch < 16U; ++ch)
    {
      if (g_control_queue_force_stop[ch] != 0U)
      {
        g_control_queue_force_stop[ch] = 0U;
        (void)board_relay_set((uint8_t)(ch + 1U), GPIO_PIN_RESET);
      }
    }

    if (xQueueReceive(g_control_queue_handle, &g_control_queue_worker_request,
                      pdMS_TO_TICKS(100U)) == pdPASS)
    {
      control_queue_request_t *request = &g_control_queue_worker_request;

      control_queue_note_dequeued();
      if (control_queue_request_is_stale(request) != 0U)
      {
        g_control_queue_stats.dropped++;
        if (request->waiting_task != NULL)
        {
          xTaskNotifyGive(request->waiting_task);
        }
        continue;
      }

      if (request->kind == CONTROL_QUEUE_REQ_REMOTE_HTTP)
      {
        HAL_StatusTypeDef remote_http_status;
        int32_t slot_index;
        control_queue_remote_http_slot_t *slot;

        taskENTER_CRITICAL();
        slot_index = control_queue_remote_http_select_pending_slot();
        if (slot_index >= 0)
        {
          g_control_queue_remote_http_slots[slot_index].state = CONTROL_QUEUE_REMOTE_HTTP_RUNNING;
        }
        taskEXIT_CRITICAL();

        if (slot_index < 0)
        {
          continue;
        }

        slot = &g_control_queue_remote_http_slots[(uint32_t)slot_index];
        memset(g_control_queue_remote_http_response, 0, sizeof(g_control_queue_remote_http_response));
        remote_http_status =
          control_queue_execute_remote_http(slot->request.ip_addr,
                                            slot->request.method,
                                            slot->request.path,
                                            slot->request.body,
                                            g_control_queue_remote_http_response,
                                            sizeof(g_control_queue_remote_http_response));

        taskENTER_CRITICAL();
        slot->ok = (remote_http_status == HAL_OK) ? 1U : 0U;
        slot->state = CONTROL_QUEUE_REMOTE_HTTP_DONE;
        taskEXIT_CRITICAL();

        if (remote_http_status == HAL_OK)
        {
          g_control_queue_stats.executed++;
        }
        else
        {
          g_control_queue_stats.dropped++;
        }
      }
      else if (request->kind == CONTROL_QUEUE_REQ_BACKEND_SYNC)
      {
        if (control_queue_sync_remote_backend_config(request->remote_ip_addr,
                                                     &g_control_queue_backend_sync_request) == HAL_OK)
        {
          g_control_queue_stats.executed++;
        }
        else
        {
          g_control_queue_stats.dropped++;
        }
        g_control_queue_backend_sync_pending = 0U;
      }
      else if (request->kind == CONTROL_QUEUE_REQ_RELAY_CONFIG)
      {
        HAL_StatusTypeDef config_status;

        if (request->remote != 0U)
        {
          config_status =
            control_queue_apply_remote_relay_config(request->remote_ip_addr,
                                                    &request->relay_config);
        }
        else
        {
          config_status =
            control_queue_apply_local_relay_config(&request->relay_config);
        }

        if (config_status == HAL_OK)
        {
          g_control_queue_stats.executed++;
          discovery_service_force_announce();
          mqtt_link_service_complete_relay_config(request->request_id,
                                                  request->remote_node_id,
                                                  1U,
                                                  "");
        }
        else
        {
          g_control_queue_stats.dropped++;
          mqtt_link_service_complete_relay_config(
            request->request_id,
            request->remote_node_id,
            0U,
            (request->remote != 0U) ? "remote_config_failed" : "config_save_failed");
        }
      }
      else if (request->remote != 0U)
      {
        relay_domain_result_t *target_result =
          (request->result != NULL) ? request->result : &g_control_queue_async_result;
        HAL_StatusTypeDef remote_status;

        if (request->result != NULL)
        {
          remote_status =
            control_queue_execute_remote(request->remote_ip_addr, &request->command, target_result);
        }
        else
        {
          remote_status =
            control_queue_execute_remote_async_fast(request->remote_ip_addr, &request->command);
        }

        if (remote_status != HAL_OK)
        {
          if (request->result != NULL)
          {
            memset(request->result, 0, sizeof(*request->result));
            request->result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
          }
          g_control_queue_stats.dropped++;
        }
        else
        {
          if ((request->result == NULL) && (request->remote_node_id != 0U))
          {
            (void)cluster_service_apply_remote_command(request->remote_node_id, &request->command);
          }
          g_control_queue_stats.executed++;
        }
      }
      else
      {
        relay_domain_result_t *target_result =
          (request->result != NULL) ? request->result : &g_control_queue_async_result;

        /* Phase A+B: Cooldown + boot safety + fault lockout checks.
           Reject ON commands for channels still cooling, during boot
           safety delay, or with hardware fault lockout. */
        if (request->command.type == CONTROL_PROTO_CMD_SET_RELAY &&
            request->command.value != 0U)
        {
          uint8_t ch = request->command.channel;
          uint32_t fault_mask = relay_protection_fault_lock_mask();

          if (relay_protection_boot_safety_active(HAL_GetTick()))
          {
            target_result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
            target_result->relay_mask = board_relay_snapshot();
            goto skip_execute;
          }
          if (ch >= 1U && ch <= 16U && ((fault_mask >> (ch - 1U)) & 1U))
          {
            target_result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
            target_result->relay_mask = board_relay_snapshot();
            goto skip_execute;
          }
          if (ch >= 1U && ch <= 16U &&
              relay_protection_is_cooldown(ch, HAL_GetTick()))
          {
            target_result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
            target_result->relay_mask = board_relay_snapshot();
            goto skip_execute;
          }
        }

        control_queue_execute_local_relay_command(&request->command, target_result);
        skip_execute:

        /* Phase B-H: Notify protection module of relay state changes */
        if (target_result->error == CONTROL_PROTO_ERR_NONE &&
            request->command.type == CONTROL_PROTO_CMD_SET_RELAY &&
            request->command.channel >= 1U && request->command.channel <= 16U)
        {
          relay_protection_notify_relay_change(request->command.channel,
                                               request->command.value,
                                               HAL_GetTick());
        }

        if ((target_result->error == CONTROL_PROTO_ERR_NONE) &&
            ((request->command.type == CONTROL_PROTO_CMD_SET_RELAY) ||
             (request->command.type == CONTROL_PROTO_CMD_SET_RELAY_MASK) ||
             (request->command.type == CONTROL_PROTO_CMD_SET_RELAY_ALL)))
        {
          device_config_runtime_relay_mask_set(target_result->relay_mask);
          mqtt_link_service_mark_status_dirty();
          /* duplicate removed */
        }
        if (target_result->error == CONTROL_PROTO_ERR_NONE)
          g_control_queue_stats.executed++;
      }

      if (request->waiting_task != NULL)
      {
        xTaskNotifyGive(request->waiting_task);
      }
    }
  }
}

void control_queue_init(void)
{
  static uint8_t initialized = 0U;

  if (initialized != 0U)
  {
    return;
  }

  memset(&g_control_queue_stats, 0, sizeof(g_control_queue_stats));
  g_control_queue_stats.capacity = CONTROL_QUEUE_LENGTH;
  g_control_queue_handle = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_queue_request_t));

  if (g_control_queue_handle != NULL)
  {
    g_control_queue_sync_mutex =
      xSemaphoreCreateMutexStatic(&g_control_queue_sync_mutex_buffer);
    if (g_control_queue_sync_mutex == NULL)
    {
      vQueueDelete(g_control_queue_handle);
      g_control_queue_handle = NULL;
      return;
    }
    g_control_queue_task_handle =
      xTaskCreateStatic((TaskFunction_t)control_queue_worker_task,
                        "ControlQueueWorker",
                        2048U /* must match g_control_queue_task_stack */,
                        NULL,
                        (UBaseType_t)(tskIDLE_PRIORITY + 4U),
                        g_control_queue_task_stack,
                        &g_control_queue_task_buffer);
    if (g_control_queue_task_handle == NULL)
    {
      vQueueDelete(g_control_queue_handle);
      g_control_queue_handle = NULL;
      return;
    }
  }
  else
  {
    return;
  }

  initialized = 1U;
}

HAL_StatusTypeDef control_queue_submit(const control_proto_command_t *command, relay_domain_result_t *result)
{
  control_queue_request_t request;

  if ((command == NULL) || (result == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) ||
      (g_control_queue_task_handle == NULL) ||
      (g_control_queue_sync_mutex == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  if (xSemaphoreTake(g_control_queue_sync_mutex,
                     pdMS_TO_TICKS(CONTROL_QUEUE_WAIT_TIMEOUT_MS)) != pdTRUE)
  {
    g_control_queue_stats.busy_rejected++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  memset(&request, 0, sizeof(request));
  memset(&g_control_queue_sync_result, 0, sizeof(g_control_queue_sync_result));
  request.command = *command;
  request.kind = CONTROL_QUEUE_REQ_COMMAND;
  request.result = &g_control_queue_sync_result;
  request.waiting_task = xTaskGetCurrentTaskHandle();
  (void)ulTaskNotifyTake(pdTRUE, 0U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.queue_full++;
    g_control_queue_stats.dropped++;
    (void)xSemaphoreGive(g_control_queue_sync_mutex);
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  control_queue_note_enqueued();
  (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  *result = g_control_queue_sync_result;
  (void)xSemaphoreGive(g_control_queue_sync_mutex);
  return (result->error == CONTROL_PROTO_ERR_NONE) ? HAL_OK : HAL_ERROR;
}

void control_queue_force_stop(uint8_t channel)
{
  if (channel == 0U || channel > 16U) return;
  g_control_queue_force_stop[channel - 1U] = 1U;
}

HAL_StatusTypeDef control_queue_submit_async(const control_proto_command_t *command)
{
  control_queue_request_t request;
  uint32_t previous_seq = 0U;

  if (command == NULL)
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  memset(&request, 0, sizeof(request));
  request.command = *command;
  request.kind = CONTROL_QUEUE_REQ_COMMAND;
  if (control_queue_command_can_coalesce(command) != 0U)
  {
    request.coalesce_seq =
      control_queue_mark_latest_relay_command(0U, 0U, command->channel, &previous_seq);
  }

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    if (request.coalesce_seq != 0U)
    {
      control_queue_restore_latest_relay_command(0U,
                                                 0U,
                                                 command->channel,
                                                 request.coalesce_seq,
                                                 previous_seq);
    }
    g_control_queue_stats.queue_full++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  control_queue_note_enqueued();
  return HAL_OK;
}

HAL_StatusTypeDef control_queue_submit_remote(uint32_t ip_addr,
                                              const control_proto_command_t *command,
                                              relay_domain_result_t *result)
{
  control_queue_request_t request;

  if ((ip_addr == 0U) || (command == NULL) || (result == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) ||
      (g_control_queue_task_handle == NULL) ||
      (g_control_queue_sync_mutex == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  if (xSemaphoreTake(g_control_queue_sync_mutex,
                     pdMS_TO_TICKS(CONTROL_QUEUE_WAIT_TIMEOUT_MS)) != pdTRUE)
  {
    g_control_queue_stats.busy_rejected++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  memset(&request, 0, sizeof(request));
  memset(&g_control_queue_sync_result, 0, sizeof(g_control_queue_sync_result));
  request.command = *command;
  request.kind = CONTROL_QUEUE_REQ_COMMAND;
  request.result = &g_control_queue_sync_result;
  request.waiting_task = xTaskGetCurrentTaskHandle();
  request.remote_ip_addr = ip_addr;
  request.remote_node_id = 0U;
  request.remote = 1U;

  (void)ulTaskNotifyTake(pdTRUE, 0U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.queue_full++;
    g_control_queue_stats.dropped++;
    (void)xSemaphoreGive(g_control_queue_sync_mutex);
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  control_queue_note_enqueued();

  (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  *result = g_control_queue_sync_result;
  (void)xSemaphoreGive(g_control_queue_sync_mutex);
  return (result->error == CONTROL_PROTO_ERR_NONE) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef control_queue_submit_remote_http_async(
  uint32_t ip_addr,
  const char *method,
  const char *path,
  const char *body,
  uint32_t *request_id)
{
  uint32_t new_request_id;
  int32_t slot_index;
  control_queue_remote_http_slot_t *slot;

  if ((ip_addr == 0U) || (method == NULL) || (path == NULL) ||
      (request_id == NULL) ||
      (strlen(method) >= CONTROL_QUEUE_REMOTE_HTTP_METHOD_MAX_LEN) ||
      (strlen(path) >= CONTROL_QUEUE_REMOTE_HTTP_PATH_MAX_LEN) ||
      ((body != NULL) && (strlen(body) >= CONTROL_QUEUE_REMOTE_HTTP_BODY_MAX_LEN)))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) ||
      (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  taskENTER_CRITICAL();
  slot_index = control_queue_remote_http_find_free_slot();
  if (slot_index < 0)
  {
    taskEXIT_CRITICAL();
    g_control_queue_stats.busy_rejected++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }
  g_control_queue_remote_http_slots[(uint32_t)slot_index].state =
    CONTROL_QUEUE_REMOTE_HTTP_PREPARING;
  new_request_id = g_control_queue_remote_http_next_request_id++;
  if (new_request_id == 0U)
  {
    new_request_id = g_control_queue_remote_http_next_request_id++;
  }
  g_control_queue_remote_http_slots[(uint32_t)slot_index].request_id = new_request_id;
  taskEXIT_CRITICAL();

  slot = &g_control_queue_remote_http_slots[(uint32_t)slot_index];
  memset(&slot->request, 0, sizeof(slot->request));

  slot->request.ip_addr = ip_addr;
  strncpy(slot->request.method,
          method,
          sizeof(slot->request.method) - 1U);
  strncpy(slot->request.path,
          path,
          sizeof(slot->request.path) - 1U);
  if (body != NULL)
  {
    strncpy(slot->request.body,
            body,
            sizeof(slot->request.body) - 1U);
  }

  taskENTER_CRITICAL();
  slot->ok = 0U;
  slot->state = CONTROL_QUEUE_REMOTE_HTTP_PENDING;
  taskEXIT_CRITICAL();

  control_queue_remote_http_kick_worker();
  *request_id = new_request_id;
  return HAL_OK;
}

HAL_StatusTypeDef control_queue_get_remote_http_result(
  uint32_t request_id,
  uint8_t *done,
  uint8_t *remote_ok,
  char *response_body,
  size_t response_body_size)
{
  uint8_t state;
  uint8_t ok;
  int32_t slot_index;
  control_queue_remote_http_slot_t *slot;

  if ((request_id == 0U) || (done == NULL) || (remote_ok == NULL) ||
      (response_body == NULL) || (response_body_size == 0U))
  {
    return HAL_ERROR;
  }

  taskENTER_CRITICAL();
  slot_index = control_queue_remote_http_find_slot_by_id(request_id);
  if (slot_index < 0)
  {
    taskEXIT_CRITICAL();
    return HAL_ERROR;
  }
  slot = &g_control_queue_remote_http_slots[(uint32_t)slot_index];
  state = slot->state;
  ok = slot->ok;
  taskEXIT_CRITICAL();

  if (state != CONTROL_QUEUE_REMOTE_HTTP_DONE)
  {
    *done = 0U;
    *remote_ok = 0U;
    response_body[0] = '\0';
    return HAL_OK;
  }

  strncpy(response_body, g_control_queue_remote_http_response, response_body_size - 1U);
  response_body[response_body_size - 1U] = '\0';
  *done = 1U;
  *remote_ok = ok;

  taskENTER_CRITICAL();
  memset(&slot->request, 0, sizeof(slot->request));
  slot->request_id = 0U;
  slot->ok = 0U;
  slot->state = CONTROL_QUEUE_REMOTE_HTTP_IDLE;
  taskEXIT_CRITICAL();
  control_queue_remote_http_kick_worker();
  return HAL_OK;
}

HAL_StatusTypeDef control_queue_submit_remote_async(uint32_t ip_addr,
                                                    uint32_t node_id,
                                                    const control_proto_command_t *command)
{
  control_queue_request_t request;
  uint32_t previous_seq = 0U;

  if ((ip_addr == 0U) || (command == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  memset(&request, 0, sizeof(request));
  request.command = *command;
  request.kind = CONTROL_QUEUE_REQ_COMMAND;
  request.result = NULL;
  request.waiting_task = NULL;
  request.remote_ip_addr = ip_addr;
  request.remote_node_id = node_id;
  request.remote = 1U;
  if (control_queue_command_can_coalesce(command) != 0U)
  {
    request.coalesce_seq =
      control_queue_mark_latest_relay_command(1U, node_id, command->channel, &previous_seq);
  }

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    if (request.coalesce_seq != 0U)
    {
      control_queue_restore_latest_relay_command(1U,
                                                 node_id,
                                                 command->channel,
                                                 request.coalesce_seq,
                                                 previous_seq);
    }
    g_control_queue_stats.queue_full++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  control_queue_note_enqueued();
  return HAL_OK;
}

HAL_StatusTypeDef control_queue_submit_backend_sync_async(uint32_t ip_addr,
                                                          const device_config_t *config)
{
  control_queue_request_t request;

  if ((ip_addr == 0U) || (config == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  taskENTER_CRITICAL();
  if (g_control_queue_backend_sync_pending != 0U)
  {
    taskEXIT_CRITICAL();
    g_control_queue_stats.busy_rejected++;
    return HAL_BUSY;
  }
  g_control_queue_backend_sync_pending = 1U;
  taskEXIT_CRITICAL();

  memset(&request, 0, sizeof(request));
  request.kind = CONTROL_QUEUE_REQ_BACKEND_SYNC;
  request.remote_ip_addr = ip_addr;
  memset(&g_control_queue_backend_sync_request, 0, sizeof(g_control_queue_backend_sync_request));
  strncpy(g_control_queue_backend_sync_request.backend_host,
          config->backend_host,
          sizeof(g_control_queue_backend_sync_request.backend_host) - 1U);
  g_control_queue_backend_sync_request.backend_port = config->backend_port;
  g_control_queue_backend_sync_request.cloud_enabled = config->cloud_enabled;
  g_control_queue_backend_sync_request.cloud_transport = config->cloud_transport;
  strncpy(g_control_queue_backend_sync_request.cloud_broker_host,
          config->cloud_broker_host,
          sizeof(g_control_queue_backend_sync_request.cloud_broker_host) - 1U);
  g_control_queue_backend_sync_request.cloud_broker_port = config->cloud_broker_port;
  strncpy(g_control_queue_backend_sync_request.cloud_cluster_id,
          config->cloud_cluster_id,
          sizeof(g_control_queue_backend_sync_request.cloud_cluster_id) - 1U);
  strncpy(g_control_queue_backend_sync_request.cloud_cluster_ca_fingerprint,
          config->cloud_cluster_ca_fingerprint,
          sizeof(g_control_queue_backend_sync_request.cloud_cluster_ca_fingerprint) - 1U);
  strncpy(g_control_queue_backend_sync_request.cloud_cluster_binding_secret,
          config->cloud_cluster_binding_secret,
          sizeof(g_control_queue_backend_sync_request.cloud_cluster_binding_secret) - 1U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_backend_sync_pending = 0U;
    g_control_queue_stats.queue_full++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  control_queue_note_enqueued();
  return HAL_OK;
}

HAL_StatusTypeDef control_queue_submit_relay_config_async(
  uint32_t ip_addr,
  uint32_t node_id,
  const control_queue_relay_config_t *relay_config,
  const char *request_id)
{
  control_queue_request_t request;

  if ((relay_config == NULL) ||
      (relay_config->channel == 0U) ||
      (relay_config->channel > DEVICE_CONFIG_RELAY_COUNT) ||
      (relay_config->fields == 0U) ||
      (request_id == NULL) ||
      (request_id[0] == '\0'))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  memset(&request, 0, sizeof(request));
  request.kind = CONTROL_QUEUE_REQ_RELAY_CONFIG;
  request.relay_config = *relay_config;
  request.remote_ip_addr = ip_addr;
  request.remote_node_id = node_id;
  request.remote = (ip_addr != 0U) ? 1U : 0U;
  strncpy(request.request_id, request_id, sizeof(request.request_id) - 1U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.queue_full++;
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  control_queue_note_enqueued();
  return HAL_OK;
}

void control_queue_poll(void)
{
  control_queue_init();
}

void control_queue_get_stats(control_queue_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_control_queue_stats;
    if (g_control_queue_handle != NULL)
    {
      stats->current_depth =
        (uint32_t)uxQueueMessagesWaiting(g_control_queue_handle);
    }
  }
}

UBaseType_t control_queue_stack_high_water(void) { if (g_control_queue_task_handle == NULL) return 0; return uxTaskGetStackHighWaterMark((TaskHandle_t)g_control_queue_task_handle); }
