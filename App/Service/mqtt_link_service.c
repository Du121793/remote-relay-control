#include "mqtt_link_service.h"

#include "https_client_service.h"
#include "adc_service.h"
#include "cluster_service.h"
#include "control_queue.h"
#include "fault_service.h"
#include "scheduler.h"
#include "relay_protection.h"
#include "../Config/device_config.h"
#include "../Platform/board_relay.h"

#include "stm32f7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MQTT_LINK_CONNECT_RETRY_MIN_MS   5000UL
#define MQTT_LINK_CONNECT_RETRY_MAX_MS   30000UL
#define MQTT_LINK_HEARTBEAT_INTERVAL_MS  5000UL
#define MQTT_LINK_STATUS_INTERVAL_MS     3000UL
#define MQTT_LINK_TCPIP_STEP_INTERVAL_MS 200UL
#define MQTT_LINK_MASTER_STABLE_MS       5000UL
#define MQTT_LINK_KEEP_ALIVE_SECONDS     20U
#define MQTT_LINK_PSK_MAX_LEN            32U
#define MQTT_LINK_PAYLOAD_MAX_LEN        512U
#define MQTT_LINK_STATUS_PAYLOAD_MAX_LEN 4096U
#define MQTT_LINK_QUERY_RESPONSE_MAX_LEN 4096U
#define MQTT_LINK_COMMAND_MAX_LEN        384U
#define MQTT_LINK_REQUEST_ID_MAX_LEN     64U
#define MQTT_LINK_ACK_TOPIC_MAX_LEN      192U
#define MQTT_LINK_ACK_ERROR_MAX_LEN      32U
#define MQTT_LINK_ACK_QUEUE_LENGTH       32U
#define MQTT_LINK_QUERY_PENDING_SLOTS    4U

#define MQTT_LINK_ERROR_CONFIG           1001U
#define MQTT_LINK_ERROR_NETWORK          1002U
#define MQTT_LINK_ERROR_NOT_MASTER       1003U
#define MQTT_LINK_ERROR_PSK              1101U
#define MQTT_LINK_ERROR_TLS_CONFIG       1102U
#define MQTT_LINK_ERROR_CLIENT_ALLOC     1103U
#define MQTT_LINK_ERROR_DNS              1201U
#define MQTT_LINK_ERROR_CONNECT          1300U
#define MQTT_LINK_ERROR_PUBLISH          1400U
#define MQTT_LINK_ERROR_SUBSCRIBE        1500U
#define MQTT_LINK_ERROR_COMMAND          1600U

typedef enum
{
  MQTT_LINK_PUBLISH_HELLO = 1,
  MQTT_LINK_PUBLISH_HEARTBEAT,
  MQTT_LINK_PUBLISH_STATUS,
  MQTT_LINK_PUBLISH_FAULT,
  MQTT_LINK_PUBLISH_QUERY_RESPONSE
} mqtt_link_publish_kind_t;

typedef struct
{
  char request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];
  char error_text[MQTT_LINK_ACK_ERROR_MAX_LEN];
  uint32_t target_node_id;
  uint8_t ok;
  uint8_t queued;
  uint8_t completed;
} mqtt_link_ack_t;

typedef struct
{
  uint8_t active;
  uint32_t remote_request_id;
  uint32_t started_ms;
  char request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];
} mqtt_link_pending_query_t;

typedef struct
{
  uint8_t started;
  volatile uint8_t status_dirty;
  volatile uint8_t tcpip_step_pending;
  volatile uint8_t reset_requested;
  volatile uint8_t time_request_pending;
  uint8_t connecting;
  uint8_t dns_pending;
  uint8_t rx_accept;
  uint8_t config_valid;
  uint8_t initial_publish_stage;
  uint32_t config_generation;
  uint32_t dns_generation;
  uint32_t master_since_ms;
  uint32_t last_poll_ms;
  uint32_t last_connect_attempt_ms;
  uint32_t connect_retry_ms;
  uint32_t last_heartbeat_ms;
  uint32_t last_status_ms;
  mqtt_client_t *client;
  struct altcp_tls_config *tls_config;
  struct mqtt_connect_client_info_t client_info;
  mqtt_link_stats_t stats;
  char broker_host[DEVICE_CONFIG_CLOUD_HOST_MAX_LEN];
  char cloud_cluster_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_device_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_key_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char binding_secret[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN];
  char will_message[192];
  char rx_topic[MQTT_LINK_TOPIC_MAX_LEN];
  char rx_payload[MQTT_LINK_COMMAND_MAX_LEN];
  uint16_t rx_used;
  uint32_t rx_expected;
  uint32_t pending_unix_time;
  uint32_t pending_time_target_node_id;
  char pending_time_request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];
  mqtt_link_ack_t ack_queue[MQTT_LINK_ACK_QUEUE_LENGTH];
  mqtt_link_pending_query_t pending_queries[MQTT_LINK_QUERY_PENDING_SLOTS];
  uint8_t ack_head;
  uint8_t ack_tail;
  uint8_t ack_count;
  uint8_t ack_in_flight;
  uint8_t psk[MQTT_LINK_PSK_MAX_LEN];
  size_t psk_len;
} mqtt_link_context_t;

static mqtt_link_context_t g_mqtt_link_context;
static char g_mqtt_query_response[MQTT_LINK_QUERY_RESPONSE_MAX_LEN];

extern struct netif gnetif;
extern volatile int altcp_tls_mbedtls_last_config_error;

static void mqtt_link_tcpip_step(void *argument);
static void mqtt_link_connection_callback(mqtt_client_t *client,
                                          void *argument,
                                          mqtt_connection_status_t status);
static void mqtt_link_ack_try_publish(void);
static void mqtt_link_apply_pending_time(void);
static void mqtt_link_release_transport_keep_backoff(uint8_t disconnect_first);

static uint32_t mqtt_link_current_retry_ms(void)
{
  if (g_mqtt_link_context.connect_retry_ms < MQTT_LINK_CONNECT_RETRY_MIN_MS)
  {
    return MQTT_LINK_CONNECT_RETRY_MIN_MS;
  }
  if (g_mqtt_link_context.connect_retry_ms > MQTT_LINK_CONNECT_RETRY_MAX_MS)
  {
    return MQTT_LINK_CONNECT_RETRY_MAX_MS;
  }
  return g_mqtt_link_context.connect_retry_ms;
}

static void mqtt_link_note_connect_failure(void)
{
  uint32_t next_retry_ms = mqtt_link_current_retry_ms();

  if (next_retry_ms < MQTT_LINK_CONNECT_RETRY_MAX_MS)
  {
    next_retry_ms *= 2U;
    if (next_retry_ms > MQTT_LINK_CONNECT_RETRY_MAX_MS)
    {
      next_retry_ms = MQTT_LINK_CONNECT_RETRY_MAX_MS;
    }
  }
  g_mqtt_link_context.connect_retry_ms = next_retry_ms;
}

static void mqtt_link_note_connect_success(void)
{
  g_mqtt_link_context.connect_retry_ms = MQTT_LINK_CONNECT_RETRY_MIN_MS;
}

static uint32_t mqtt_link_tls_config_error_code(void)
{
  const int tls_error = altcp_tls_mbedtls_last_config_error;

  if (tls_error > 0)
  {
    return MQTT_LINK_ERROR_TLS_CONFIG + ((uint32_t)tls_error * 10U);
  }
  if (tls_error < 0)
  {
    return MQTT_LINK_ERROR_TLS_CONFIG + 1000U + (uint32_t)(-tls_error);
  }
  return MQTT_LINK_ERROR_TLS_CONFIG;
}

static void mqtt_link_log(const char *text)
{
  extern UART_HandleTypeDef huart3;

  if ((text != NULL) && (huart3.Instance != NULL))
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)strlen(text), 50U);
  }
}

static void mqtt_link_log_value(const char *prefix, uint32_t value)
{
  char buffer[96];

  (void)snprintf(buffer, sizeof(buffer), "%s%lu\r\n",
                 (prefix != NULL) ? prefix : "[mqtt] ",
                 (unsigned long)value);
  mqtt_link_log(buffer);
}

static void mqtt_link_copy_text(char *destination, size_t destination_size, const char *source)
{
  if ((destination == NULL) || (destination_size == 0U))
  {
    return;
  }

  destination[0] = '\0';
  if (source != NULL)
  {
    strncpy(destination, source, destination_size - 1U);
    destination[destination_size - 1U] = '\0';
  }
}

static uint8_t mqtt_link_network_ready(void)
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

static uint8_t mqtt_link_configured(const device_config_t *config)
{
  if ((config == NULL) || (config->cloud_enabled == 0U))
  {
    return 0U;
  }

  if ((config->cloud_broker_host[0] == '\0') ||
      (config->cloud_broker_port == 0U) ||
      (config->cloud_cluster_id[0] == '\0') ||
      (config->cloud_device_id[0] == '\0') ||
      (config->cloud_key_id[0] == '\0') ||
      (config->cloud_cluster_binding_secret[0] == '\0'))
  {
    return 0U;
  }

  return 1U;
}

static uint8_t mqtt_link_should_run(uint8_t configured)
{
  cluster_service_state_t cluster_state;
  uint32_t now;

  if ((configured == 0U) || (mqtt_link_network_ready() == 0U))
  {
    g_mqtt_link_context.master_since_ms = 0U;
    return 0U;
  }

  cluster_service_get_state(&cluster_state);
  if ((cluster_state.cluster_enabled == 0U) ||
      (cluster_state.role != CLUSTER_ROLE_MASTER) ||
      (cluster_state.master_node_id != cluster_state.self_node_id))
  {
    g_mqtt_link_context.master_since_ms = 0U;
    return 0U;
  }

  now = HAL_GetTick();
  if (g_mqtt_link_context.master_since_ms == 0U)
  {
    g_mqtt_link_context.master_since_ms = now;
    return 0U;
  }
  if ((int32_t)(now - g_mqtt_link_context.master_since_ms) <
      (int32_t)MQTT_LINK_MASTER_STABLE_MS)
  {
    return 0U;
  }

  return 1U;
}

static void mqtt_link_build_topics(const device_config_t *config)
{
  mqtt_link_stats_t *stats = &g_mqtt_link_context.stats;

  (void)snprintf(stats->client_id, sizeof(stats->client_id),
                 "autoControl-%s-%s",
                 config->cloud_cluster_id,
                 config->cloud_device_id);
  (void)snprintf(stats->topic_status, sizeof(stats->topic_status),
                 "cluster/%s/status", config->cloud_cluster_id);
  (void)snprintf(stats->topic_events, sizeof(stats->topic_events),
                 "cluster/%s/events", config->cloud_cluster_id);
  (void)snprintf(stats->topic_measure, sizeof(stats->topic_measure),
                 "cluster/%s/measure", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_relay, sizeof(stats->topic_command_relay),
                 "cluster/%s/command/relay", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_mask, sizeof(stats->topic_command_mask),
                 "cluster/%s/command/mask", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_config_relay,
                 sizeof(stats->topic_command_config_relay),
                 "cluster/%s/command/config/relay", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_config_cluster,
                 sizeof(stats->topic_command_config_cluster),
                 "cluster/%s/command/config/cluster", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_time,
                 sizeof(stats->topic_command_time),
                 "cluster/%s/command/time", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_auto_hold,
                 sizeof(stats->topic_command_auto_hold),
                 "cluster/%s/command/auto-hold", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_control,
                 sizeof(stats->topic_command_control),
                 "cluster/%s/command/control", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_scheduler_set,
                 sizeof(stats->topic_command_scheduler_set),
                 "cluster/%s/command/scheduler/set", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_scheduler_enabled,
                 sizeof(stats->topic_command_scheduler_enabled),
                 "cluster/%s/command/scheduler/enabled", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_scheduler_pause,
                 sizeof(stats->topic_command_scheduler_pause),
                 "cluster/%s/command/scheduler/pause", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_scheduler_resume,
                 sizeof(stats->topic_command_scheduler_resume),
                 "cluster/%s/command/scheduler/resume", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_config_protection,
                 sizeof(stats->topic_command_config_protection),
                 "cluster/%s/command/config/protection", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_protection_strategy,
                 sizeof(stats->topic_command_protection_strategy),
                 "cluster/%s/command/protection/strategy", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_protection_force_stop,
                 sizeof(stats->topic_command_protection_force_stop),
                 "cluster/%s/command/protection/force-stop", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_protection_unlock,
                 sizeof(stats->topic_command_protection_unlock),
                 "cluster/%s/command/protection/unlock", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_protection_clear_learning,
                 sizeof(stats->topic_command_protection_clear_learning),
                 "cluster/%s/command/protection/clear-learning", config->cloud_cluster_id);
  (void)snprintf(stats->topic_query_request,
                 sizeof(stats->topic_query_request),
                 "cluster/%s/query/request", config->cloud_cluster_id);
  (void)snprintf(stats->topic_query_response_prefix,
                 sizeof(stats->topic_query_response_prefix),
                 "cluster/%s/query/response/", config->cloud_cluster_id);
  (void)snprintf(stats->topic_ack_prefix, sizeof(stats->topic_ack_prefix),
                 "cluster/%s/ack/", config->cloud_cluster_id);

  mqtt_link_copy_text(stats->cluster_ca_fingerprint,
                      sizeof(stats->cluster_ca_fingerprint),
                      config->cloud_cluster_ca_fingerprint);
  mqtt_link_copy_text(stats->cluster_binding_secret,
                      sizeof(stats->cluster_binding_secret),
                      config->cloud_cluster_binding_secret);
}

static uint8_t mqtt_link_config_changed(const device_config_t *config)
{
  if (g_mqtt_link_context.config_valid == 0U)
  {
    return 1U;
  }

  return ((strcmp(g_mqtt_link_context.broker_host, config->cloud_broker_host) != 0) ||
          (g_mqtt_link_context.stats.broker_port != config->cloud_broker_port) ||
          (strcmp(g_mqtt_link_context.cloud_cluster_id, config->cloud_cluster_id) != 0) ||
          (strcmp(g_mqtt_link_context.cloud_device_id, config->cloud_device_id) != 0) ||
          (strcmp(g_mqtt_link_context.cloud_key_id, config->cloud_key_id) != 0) ||
          (strcmp(g_mqtt_link_context.binding_secret,
                  config->cloud_cluster_binding_secret) != 0)) ? 1U : 0U;
}

static void mqtt_link_capture_config(const device_config_t *config)
{
  if (mqtt_link_config_changed(config) != 0U)
  {
    g_mqtt_link_context.config_generation++;
    g_mqtt_link_context.reset_requested = 1U;
  }

  mqtt_link_copy_text(g_mqtt_link_context.broker_host,
                      sizeof(g_mqtt_link_context.broker_host),
                      config->cloud_broker_host);
  mqtt_link_copy_text(g_mqtt_link_context.cloud_cluster_id,
                      sizeof(g_mqtt_link_context.cloud_cluster_id),
                      config->cloud_cluster_id);
  mqtt_link_copy_text(g_mqtt_link_context.cloud_device_id,
                      sizeof(g_mqtt_link_context.cloud_device_id),
                      config->cloud_device_id);
  mqtt_link_copy_text(g_mqtt_link_context.cloud_key_id,
                      sizeof(g_mqtt_link_context.cloud_key_id),
                      config->cloud_key_id);
  mqtt_link_copy_text(g_mqtt_link_context.binding_secret,
                      sizeof(g_mqtt_link_context.binding_secret),
                      config->cloud_cluster_binding_secret);
  mqtt_link_build_topics(config);
  g_mqtt_link_context.config_valid = 1U;
}

static int mqtt_link_base64url_value(char value)
{
  if ((value >= 'A') && (value <= 'Z'))
  {
    return value - 'A';
  }
  if ((value >= 'a') && (value <= 'z'))
  {
    return value - 'a' + 26;
  }
  if ((value >= '0') && (value <= '9'))
  {
    return value - '0' + 52;
  }
  if (value == '-')
  {
    return 62;
  }
  if (value == '_')
  {
    return 63;
  }
  return -1;
}

static size_t mqtt_link_decode_base64url(const char *text, uint8_t *output, size_t output_size)
{
  uint32_t accumulator = 0U;
  uint8_t bits = 0U;
  size_t output_length = 0U;

  if ((text == NULL) || (output == NULL) || (output_size == 0U))
  {
    return 0U;
  }

  while ((*text != '\0') && (*text != '='))
  {
    const int value = mqtt_link_base64url_value(*text++);
    if (value < 0)
    {
      return 0U;
    }

    accumulator = (accumulator << 6) | (uint32_t)value;
    bits = (uint8_t)(bits + 6U);
    if (bits >= 8U)
    {
      bits = (uint8_t)(bits - 8U);
      if (output_length >= output_size)
      {
        return 0U;
      }
      output[output_length++] = (uint8_t)(accumulator >> bits);
      accumulator &= (bits == 0U) ? 0U : ((1UL << bits) - 1UL);
    }
  }

  return output_length;
}

static uint8_t mqtt_link_extract_json_u32(const char *json, const char *key, uint32_t *value_out)
{
  char pattern[72];
  const char *value;
  char *end_pointer;
  unsigned long parsed;

  if ((json == NULL) || (key == NULL) || (value_out == NULL))
  {
    return 0U;
  }

  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  value = strstr(json, pattern);
  if (value == NULL)
  {
    return 0U;
  }

  value += strlen(pattern);
  while (isspace((unsigned char)*value) != 0)
  {
    value++;
  }
  if (*value++ != ':')
  {
    return 0U;
  }
  while (isspace((unsigned char)*value) != 0)
  {
    value++;
  }

  errno = 0;
  parsed = strtoul(value, &end_pointer, 10);
  if ((end_pointer == value) || (errno == ERANGE))
  {
    return 0U;
  }
  while (isspace((unsigned char)*end_pointer) != 0)
  {
    end_pointer++;
  }
  if ((*end_pointer != ',') &&
      (*end_pointer != '}') &&
      (*end_pointer != ']') &&
      (*end_pointer != '\0'))
  {
    return 0U;
  }

  *value_out = (uint32_t)parsed;
  return 1U;
}

static uint8_t mqtt_link_extract_json_string(const char *json,
                                             const char *key,
                                             char *output,
                                             size_t output_size)
{
  char pattern[72];
  const char *value;
  size_t used = 0U;

  if ((json == NULL) || (key == NULL) || (output == NULL) || (output_size == 0U))
  {
    return 0U;
  }

  output[0] = '\0';
  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  value = strstr(json, pattern);
  if (value == NULL)
  {
    return 0U;
  }

  value += strlen(pattern);
  while (isspace((unsigned char)*value) != 0)
  {
    value++;
  }
  if (*value++ != ':')
  {
    return 0U;
  }
  while (isspace((unsigned char)*value) != 0)
  {
    value++;
  }
  if (*value++ != '\"')
  {
    return 0U;
  }

  while ((*value != '\0') && (*value != '\"') && ((used + 1U) < output_size))
  {
    if (((unsigned char)*value < 0x20U) || (*value == '\\'))
    {
      return 0U;
    }
    output[used++] = *value++;
  }
  if (*value != '\"')
  {
    output[0] = '\0';
    return 0U;
  }
  output[used] = '\0';
  return (used > 0U) ? 1U : 0U;
}

static void mqtt_link_publish_callback(void *argument, err_t error)
{
  const mqtt_link_publish_kind_t kind = (mqtt_link_publish_kind_t)(uintptr_t)argument;

  if (error != ERR_OK)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_PUBLISH + (uint32_t)(-error);
    return;
  }
  if ((g_mqtt_link_context.stats.last_error >= MQTT_LINK_ERROR_PUBLISH) &&
      (g_mqtt_link_context.stats.last_error < MQTT_LINK_ERROR_SUBSCRIBE))
  {
    g_mqtt_link_context.stats.last_error = 0U;
  }

  switch (kind)
  {
    case MQTT_LINK_PUBLISH_HELLO:
      g_mqtt_link_context.stats.published_hello++;
      break;
    case MQTT_LINK_PUBLISH_HEARTBEAT:
      g_mqtt_link_context.stats.published_heartbeat++;
      break;
    case MQTT_LINK_PUBLISH_STATUS:
      g_mqtt_link_context.stats.published_status++;
      break;
    case MQTT_LINK_PUBLISH_FAULT:
      g_mqtt_link_context.stats.published_fault_reports++;
      fault_service_clear();
      break;
    default:
      break;
  }
}

static err_t mqtt_link_publish(const char *topic,
                               const char *payload,
                               uint8_t retain,
                               mqtt_link_publish_kind_t kind)
{
  err_t error;
  size_t payload_length;

  if ((g_mqtt_link_context.client == NULL) ||
      (g_mqtt_link_context.stats.connected == 0U) ||
      (topic == NULL) || (payload == NULL))
  {
    return ERR_CONN;
  }

  payload_length = strlen(payload);
  if (payload_length > 0xFFFFU)
  {
    return ERR_VAL;
  }

  error = mqtt_publish(g_mqtt_link_context.client,
                       topic,
                       payload,
                       (u16_t)payload_length,
                       0U,
                       retain,
                       mqtt_link_publish_callback,
                       (void *)(uintptr_t)kind);
  if (error != ERR_OK)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_PUBLISH + (uint32_t)(-error);
  }
  return error;
}

static err_t mqtt_link_publish_hello(void)
{
  char payload[MQTT_LINK_PAYLOAD_MAX_LEN];
  cluster_service_state_t cluster_state;

  cluster_service_get_state(&cluster_state);
  (void)snprintf(payload, sizeof(payload),
                 "{\"type\":\"hello\",\"cluster_id\":\"%s\",\"device_id\":\"%s\","
                 "\"key_id\":\"%s\",\"node_id\":%lu,\"role\":%u,\"term\":%lu,"
                 "\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u,"
                 "\"time_source\":%u,\"time_quality\":%u}",
                 g_mqtt_link_context.cloud_cluster_id,
                 g_mqtt_link_context.cloud_device_id,
                 g_mqtt_link_context.cloud_key_id,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned int)cluster_state.role,
                 (unsigned long)cluster_state.term,
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)cluster_state.unix_time,
                 (unsigned int)cluster_state.time_valid,
                 (unsigned int)cluster_state.time_source,
                 (unsigned int)cluster_state.time_quality);
  return mqtt_link_publish(g_mqtt_link_context.stats.topic_events,
                           payload,
                           0U,
                           MQTT_LINK_PUBLISH_HELLO);
}

static void mqtt_link_publish_heartbeat(void)
{
  char payload[MQTT_LINK_PAYLOAD_MAX_LEN];
  cluster_service_state_t cluster_state;

  cluster_service_get_state(&cluster_state);
  (void)snprintf(payload, sizeof(payload),
                 "{\"type\":\"heartbeat\",\"device_id\":\"%s\",\"node_id\":%lu,"
                 "\"term\":%lu,\"uptime_ms\":%lu,\"unix_time\":%lu,"
                 "\"time_valid\":%u,\"time_source\":%u,\"time_quality\":%u}",
                 g_mqtt_link_context.cloud_device_id,
                 (unsigned long)cluster_state.self_node_id,
                 (unsigned long)cluster_state.term,
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)cluster_state.unix_time,
                 (unsigned int)cluster_state.time_valid,
                 (unsigned int)cluster_state.time_source,
                 (unsigned int)cluster_state.time_quality);
  if (mqtt_link_publish(g_mqtt_link_context.stats.topic_events,
                        payload,
                        0U,
                        MQTT_LINK_PUBLISH_HEARTBEAT) == ERR_OK)
  {
    g_mqtt_link_context.last_heartbeat_ms = HAL_GetTick();
  }
}

static err_t mqtt_link_publish_status(void)
{
  err_t error;
  int written;
  static char payload[MQTT_LINK_STATUS_PAYLOAD_MAX_LEN];
  cluster_service_state_t cluster_state;
  adc_service_state_t adc_state;
  const cluster_node_t *nodes;
  size_t used;
  uint32_t index;
  uint32_t scheduler_enabled_mask = 0U;
  uint32_t scheduler_pause_mask = 0U;
  uint32_t scheduler_pause_blocks_mask = 0U;
  uint32_t protection_enabled_mask = 0U;
  uint32_t protection_tripped_mask = 0U;
  uint32_t protection_fault_lock_mask = 0U;
  uint32_t protection_ct_valid_mask = 0U;
  uint32_t protection_learning_mask = 0U;
  uint32_t protection_strategy_lsb_mask = 0U;
  uint32_t protection_strategy_msb_mask = 0U;
  uint8_t time_valid = 0U;
  uint32_t now_unix;

  cluster_service_get_state(&cluster_state);
  adc_service_get_state(&adc_state);
  nodes = cluster_service_get_nodes(NULL);
  now_unix = cluster_service_get_unix_time(&time_valid);
  for (index = 1U; index <= SCHEDULER_CHANNEL_COUNT; ++index)
  {
    channel_scheduler_state_t st;
    relay_protection_summary_t prot_state;
    if (scheduler_get_state_copy((uint8_t)index, &st) == 0U)
    {
      continue;
    }
    if (st.schedule_enabled != 0U)
    {
      scheduler_enabled_mask |= (1UL << (index - 1U));
    }
    if ((st.schedule_pause_until != 0U) &&
        ((st.schedule_pause_until == SCHEDULER_PAUSE_FOREVER) ||
         ((time_valid != 0U) && (now_unix < st.schedule_pause_until))))
    {
      scheduler_pause_mask |= (1UL << (index - 1U));
    }
    if (st.pause_blocks_auto_trigger != 0U)
    {
      scheduler_pause_blocks_mask |= (1UL << (index - 1U));
    }
    if (relay_protection_get_summary((uint8_t)index, &prot_state) != 0U)
    {
      if (prot_state.protection_enabled != 0U) protection_enabled_mask |= (1UL << (index - 1U));
      if (prot_state.tripped != 0U) protection_tripped_mask |= (1UL << (index - 1U));
      if (prot_state.fault_locked != 0U) protection_fault_lock_mask |= (1UL << (index - 1U));
      if (prot_state.ct_valid != 0U) protection_ct_valid_mask |= (1UL << (index - 1U));
      if (prot_state.learning_complete != 0U) protection_learning_mask |= (1UL << (index - 1U));
      if ((prot_state.device_strategy & 0x01U) != 0U) protection_strategy_lsb_mask |= (1UL << (index - 1U));
      if ((prot_state.device_strategy & 0x02U) != 0U) protection_strategy_msb_mask |= (1UL << (index - 1U));
    }
  }
  protection_fault_lock_mask |= relay_protection_fault_lock_mask();
  written = snprintf(payload, sizeof(payload),
                     "{\"type\":\"status\",\"online\":1,\"cluster_id\":\"%s\","
                     "\"device_id\":\"%s\",\"node_id\":%lu,\"master_node_id\":%lu,"
                     "\"role\":%u,\"term\":%lu,\"relay_mask\":%lu,"
                     "\"scheduler_enabled_mask\":%lu,\"scheduler_pause_mask\":%lu,"
                     "\"scheduler_pause_blocks_mask\":%lu,"
                     "\"protection_enabled_mask\":%lu,\"protection_tripped_mask\":%lu,"
                     "\"protection_fault_lock_mask\":%lu,\"protection_ct_valid_mask\":%lu,"
                     "\"protection_learning_mask\":%lu,\"protection_strategy_lsb_mask\":%lu,"
                     "\"protection_strategy_msb_mask\":%lu,"
                     "\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u,"
                     "\"time_source\":%u,\"time_quality\":%u,\"time_seq\":%lu,"
                     "\"gateway_epoch\":%lu,\"gateway_node_id\":%lu,"
                     "\"adc_valid\":%u,\"adc_ct_valid\":%u,\"adc_signal_present\":%u,"
                     "\"adc_raw\":%u,\"adc_millivolts\":%u,\"adc_min\":%u,"
                     "\"adc_max\":%u,\"adc_average\":%u,\"adc_raw_rms\":%u,"
                     "\"adc_rms_millivolts\":%u,\"adc_current_milliamps\":%lu,"
                     "\"adc_sample_rate_hz\":%lu,\"adc_window_samples\":%lu,"
                     "\"adc_sample_count\":%lu,\"adc_dma_overruns\":%lu,"
                     "\"adc_saturation_count\":%lu,\"nodes\":[",
                     g_mqtt_link_context.cloud_cluster_id,
                     g_mqtt_link_context.cloud_device_id,
                     (unsigned long)cluster_state.self_node_id,
                     (unsigned long)cluster_state.master_node_id,
                     (unsigned int)cluster_state.role,
                     (unsigned long)cluster_state.term,
                     (unsigned long)board_relay_snapshot(),
                     (unsigned long)scheduler_enabled_mask,
                     (unsigned long)scheduler_pause_mask,
                     (unsigned long)scheduler_pause_blocks_mask,
                     (unsigned long)protection_enabled_mask,
                     (unsigned long)protection_tripped_mask,
                     (unsigned long)protection_fault_lock_mask,
                     (unsigned long)protection_ct_valid_mask,
                     (unsigned long)protection_learning_mask,
                     (unsigned long)protection_strategy_lsb_mask,
                     (unsigned long)protection_strategy_msb_mask,
                     (unsigned long)HAL_GetTick(),
                     (unsigned long)cluster_state.unix_time,
                     (unsigned int)cluster_state.time_valid,
                     (unsigned int)cluster_state.time_source,
                     (unsigned int)cluster_state.time_quality,
                     (unsigned long)cluster_state.time_seq,
                     (unsigned long)cluster_state.gateway_epoch,
                     (unsigned long)cluster_state.gateway_node_id,
                     (unsigned int)adc_state.valid,
                     (unsigned int)adc_state.ct_valid,
                     (unsigned int)adc_state.signal_present,
                     (unsigned int)adc_state.raw_latest,
                     (unsigned int)adc_state.millivolts,
                     (unsigned int)adc_state.raw_min,
                     (unsigned int)adc_state.raw_max,
                     (unsigned int)adc_state.raw_average,
                     (unsigned int)adc_state.raw_rms,
                     (unsigned int)adc_state.ac_rms_millivolts,
                     (unsigned long)adc_state.current_milliamps,
                     (unsigned long)adc_state.sample_rate_hz,
                     (unsigned long)adc_state.window_samples,
                     (unsigned long)adc_state.sample_count,
                     (unsigned long)adc_state.dma_overrun_count,
                     (unsigned long)adc_state.saturation_count);
  if ((written < 0) || ((size_t)written >= sizeof(payload)))
  {
    return ERR_BUF;
  }
  used = (size_t)written;

  if (nodes != NULL)
  {
    uint8_t first_node = 1U;
    for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
    {
      char ip_text[16];
      ip4_addr_t ip_addr;
      int node_written;
      const cluster_node_t *node = &nodes[index];

      if ((node->valid == 0U) || (node->node_id == 0U))
      {
        continue;
      }

      ip_addr.addr = node->ip_addr;
      (void)ip4addr_ntoa_r(&ip_addr, ip_text, sizeof(ip_text));
      node_written = snprintf(&payload[used],
                              sizeof(payload) - used,
                              "%s{\"node_id\":%lu,\"ip_text\":\"%s\","
                              "\"role\":%u,\"term\":%lu,\"relay_mask\":%lu,"
                              "\"scheduler_enabled_mask\":%lu,"
                              "\"scheduler_pause_mask\":%lu,"
                              "\"scheduler_pause_blocks_mask\":%lu,"
                              "\"protection_enabled_mask\":%lu,"
                              "\"protection_tripped_mask\":%lu,"
                              "\"protection_fault_lock_mask\":%lu,"
                              "\"protection_ct_valid_mask\":%lu,"
                              "\"protection_learning_mask\":%lu,"
                              "\"protection_strategy_lsb_mask\":%lu,"
                              "\"protection_strategy_msb_mask\":%lu,"
                              "\"online\":%u,\"device_state\":%u,"
                              "\"name\":\"Node %lu\","
                              "\"cloud_cluster_id\":\"%s\","
                              "\"cloud_device_id\":\"%s\","
                              "\"cloud_key_id\":\"%s\"}",
                              (first_node != 0U) ? "" : ",",
                              (unsigned long)node->node_id,
                              ip_text,
                              (unsigned int)node->role,
                              (unsigned long)node->term,
                              (unsigned long)node->relay_mask,
                              (unsigned long)node->scheduler_enabled_mask,
                              (unsigned long)node->scheduler_pause_mask,
                              (unsigned long)node->scheduler_pause_blocks_mask,
                              (unsigned long)node->protection_enabled_mask,
                              (unsigned long)node->protection_tripped_mask,
                              (unsigned long)node->protection_fault_lock_mask,
                              (unsigned long)node->protection_ct_valid_mask,
                              (unsigned long)node->protection_learning_mask,
                              (unsigned long)node->protection_strategy_lsb_mask,
                              (unsigned long)node->protection_strategy_msb_mask,
                              (unsigned int)node->online,
                              (unsigned int)node->device_state,
                              (unsigned long)node->node_id,
                              node->cloud_cluster_id,
                              node->cloud_device_id,
                              node->cloud_key_id);
      if ((node_written < 0) || ((size_t)node_written >= (sizeof(payload) - used)))
      {
        return ERR_BUF;
      }
      used += (size_t)node_written;
      first_node = 0U;
    }
  }

  written = snprintf(&payload[used], sizeof(payload) - used, "]}");
  if ((written < 0) || ((size_t)written >= (sizeof(payload) - used)))
  {
    return ERR_BUF;
  }
  error = mqtt_link_publish(g_mqtt_link_context.stats.topic_status,
                            payload,
                            1U,
                            MQTT_LINK_PUBLISH_STATUS);
  if (error == ERR_OK)
  {
    g_mqtt_link_context.status_dirty = 0U;
    g_mqtt_link_context.last_status_ms = HAL_GetTick();
  }
  return error;
}

static err_t mqtt_link_publish_fault(void)
{
  char payload[MQTT_LINK_PAYLOAD_MAX_LEN];
  fault_service_record_t record;
  cluster_service_state_t cluster_state;

  if (fault_service_peek(&record) == 0U)
  {
    return ERR_OK;
  }

  cluster_service_get_state(&cluster_state);
  (void)snprintf(payload, sizeof(payload),
                 "{\"type\":\"fault_report\",\"device_id\":\"%s\",\"fault_type\":%lu,"
                 "\"cfsr\":%lu,\"hfsr\":%lu,\"pc\":%lu,\"lr\":%lu,"
                 "\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u}",
                 g_mqtt_link_context.cloud_device_id,
                 (unsigned long)record.type,
                 (unsigned long)record.cfsr,
                 (unsigned long)record.hfsr,
                 (unsigned long)record.pc,
                 (unsigned long)record.lr,
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)cluster_state.unix_time,
                 (unsigned int)cluster_state.time_valid);
  return mqtt_link_publish(g_mqtt_link_context.stats.topic_events,
                           payload,
                           0U,
                           MQTT_LINK_PUBLISH_FAULT);
}

/* Subscription state machine: topics subscribed one-by-one waiting for SUBACK */
static uint8_t  g_mqtt_sub_index = 0U;
static const char *g_mqtt_sub_topics[17];
static uint8_t  g_mqtt_sub_count = 0U;

static void mqtt_link_subscription_callback(void *argument, err_t error)
{
  (void)argument;
  if (error != ERR_OK)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_SUBSCRIBE + (uint32_t)(-error);
    mqtt_link_log_value("[mqtt] subscribe fail=", (uint32_t)(-error));
  }
  /* Chain to next topic */
  g_mqtt_sub_index++;
  if (g_mqtt_sub_index < g_mqtt_sub_count)
  {
    (void)mqtt_subscribe(g_mqtt_link_context.client,
                         g_mqtt_sub_topics[g_mqtt_sub_index],
                         1U, mqtt_link_subscription_callback, NULL);
  }
}

static void mqtt_link_ack_publish_callback(void *argument, err_t error)
{
  (void)argument;

  taskENTER_CRITICAL();
  g_mqtt_link_context.ack_in_flight = 0U;
  if ((error == ERR_OK) && (g_mqtt_link_context.ack_count > 0U))
  {
    g_mqtt_link_context.ack_head =
      (uint8_t)((g_mqtt_link_context.ack_head + 1U) % MQTT_LINK_ACK_QUEUE_LENGTH);
    g_mqtt_link_context.ack_count--;
    g_mqtt_link_context.stats.published_acks++;
    g_mqtt_link_context.stats.ack_queue_depth = g_mqtt_link_context.ack_count;
  }
  else if (error != ERR_OK)
  {
    g_mqtt_link_context.stats.last_error =
      MQTT_LINK_ERROR_PUBLISH + (uint32_t)(-error);
  }
  taskEXIT_CRITICAL();
}

static void mqtt_link_ack_try_publish(void)
{
  mqtt_link_ack_t ack;
  char topic[MQTT_LINK_ACK_TOPIC_MAX_LEN];
  char payload[MQTT_LINK_PAYLOAD_MAX_LEN];
  cluster_service_state_t cluster_state;
  err_t error;

  taskENTER_CRITICAL();
  if ((g_mqtt_link_context.ack_count == 0U) ||
      (g_mqtt_link_context.ack_in_flight != 0U) ||
      (g_mqtt_link_context.client == NULL) ||
      (g_mqtt_link_context.stats.connected == 0U))
  {
    taskEXIT_CRITICAL();
    return;
  }
  ack = g_mqtt_link_context.ack_queue[g_mqtt_link_context.ack_head];
  taskEXIT_CRITICAL();

  (void)snprintf(topic, sizeof(topic), "%s%s",
                 g_mqtt_link_context.stats.topic_ack_prefix,
                 ack.request_id);
  cluster_service_get_state(&cluster_state);
  (void)snprintf(payload, sizeof(payload),
                 "{\"ok\":%s,\"queued\":%u,\"completed\":%u,\"request_id\":\"%s\","
                 "\"target_node_id\":%lu,\"error\":\"%s\","
                 "\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u}",
                 (ack.ok != 0U) ? "true" : "false",
                 (unsigned int)ack.queued,
                 (unsigned int)ack.completed,
                 ack.request_id,
                 (unsigned long)ack.target_node_id,
                 ack.error_text,
                 (unsigned long)HAL_GetTick(),
                 (unsigned long)cluster_state.unix_time,
                 (unsigned int)cluster_state.time_valid);

  error = mqtt_publish(g_mqtt_link_context.client,
                       topic,
                       payload,
                       (u16_t)strlen(payload),
                       0U,
                       0U,
                       mqtt_link_ack_publish_callback,
                       NULL);
  if (error == ERR_OK)
  {
    g_mqtt_link_context.ack_in_flight = 1U;
  }
  else if (error != ERR_MEM)
  {
    g_mqtt_link_context.stats.last_error =
      MQTT_LINK_ERROR_PUBLISH + (uint32_t)(-error);
  }
}

static void mqtt_link_enqueue_ack(const char *request_id,
                                  uint8_t ok,
                                  uint8_t queued,
                                  uint8_t completed,
                                  const char *error_text,
                                  uint32_t target_node_id)
{
  mqtt_link_ack_t ack;
  size_t index;

  memset(&ack, 0, sizeof(ack));
  mqtt_link_copy_text(ack.request_id, sizeof(ack.request_id),
                      ((request_id != NULL) && (request_id[0] != '\0')) ? request_id : "latest");
  for (index = 0U; ack.request_id[index] != '\0'; ++index)
  {
    if ((isalnum((unsigned char)ack.request_id[index]) == 0) &&
        (ack.request_id[index] != '-') &&
        (ack.request_id[index] != '_') &&
        (ack.request_id[index] != '.'))
    {
      ack.request_id[index] = '_';
    }
  }

  mqtt_link_copy_text(ack.error_text, sizeof(ack.error_text), error_text);
  ack.target_node_id = target_node_id;
  ack.ok = ok;
  ack.queued = queued;
  ack.completed = completed;

  taskENTER_CRITICAL();
  if (g_mqtt_link_context.ack_count >= MQTT_LINK_ACK_QUEUE_LENGTH)
  {
    g_mqtt_link_context.stats.ack_queue_dropped++;
    taskEXIT_CRITICAL();
    return;
  }
  g_mqtt_link_context.ack_queue[g_mqtt_link_context.ack_tail] = ack;
  g_mqtt_link_context.ack_tail =
    (uint8_t)((g_mqtt_link_context.ack_tail + 1U) % MQTT_LINK_ACK_QUEUE_LENGTH);
  g_mqtt_link_context.ack_count++;
  g_mqtt_link_context.stats.ack_queue_depth = g_mqtt_link_context.ack_count;
  taskEXIT_CRITICAL();
}

static void mqtt_link_publish_ack(const char *request_id,
                                  uint8_t ok,
                                  const char *error_text,
                                  uint32_t target_node_id)
{
  mqtt_link_enqueue_ack(request_id,
                        ok,
                        ok,
                        0U,
                        error_text,
                        target_node_id);
}

static void mqtt_link_apply_pending_time(void)
{
  cluster_service_state_t cluster_state;
  uint8_t current_valid;
  uint32_t current_time;
  uint32_t unix_time;
  uint32_t target_node_id;
  char request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];

  taskENTER_CRITICAL();
  if (g_mqtt_link_context.time_request_pending == 0U)
  {
    taskEXIT_CRITICAL();
    return;
  }
  unix_time = g_mqtt_link_context.pending_unix_time;
  target_node_id = g_mqtt_link_context.pending_time_target_node_id;
  memcpy(request_id,
         g_mqtt_link_context.pending_time_request_id,
         sizeof(request_id));
  g_mqtt_link_context.time_request_pending = 0U;
  taskEXIT_CRITICAL();

  cluster_service_get_state(&cluster_state);
  current_time = cluster_service_get_unix_time(&current_valid);
  if ((current_valid != 0U) &&
      ((cluster_state.time_source == CLUSTER_TIME_SOURCE_NTP) ||
       (cluster_state.time_source == CLUSTER_TIME_SOURCE_BACKEND)) &&
      (((int64_t)unix_time - (int64_t)current_time) >= -2LL) &&
      (((int64_t)unix_time - (int64_t)current_time) <= 2LL))
  {
    mqtt_link_enqueue_ack(request_id, 1U, 1U, 1U, "", target_node_id);
    return;
  }

  if (cluster_service_set_unix_time(unix_time,
                                    CLUSTER_TIME_SOURCE_MQTT) == 0U)
  {
    g_mqtt_link_context.stats.command_errors++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 7U;
    mqtt_link_enqueue_ack(request_id, 0U, 1U, 1U,
                          "bad_time_request", target_node_id);
    return;
  }

  mqtt_link_enqueue_ack(request_id, 1U, 1U, 1U, "", target_node_id);
  g_mqtt_link_context.status_dirty = 1U;
}

static HAL_StatusTypeDef mqtt_link_queue_command(const control_proto_command_t *command,
                                                  uint32_t target_node_id)
{
  cluster_service_state_t cluster_state;
  const cluster_node_t *remote_node;

  cluster_service_get_state(&cluster_state);
  if ((target_node_id == 0U) || (target_node_id == cluster_state.self_node_id))
  {
    return control_queue_submit_async(command);
  }

  remote_node = cluster_service_find_node(target_node_id);
  if ((remote_node == NULL) || (remote_node->online == 0U) || (remote_node->ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  return control_queue_submit_remote_async(remote_node->ip_addr,
                                            remote_node->node_id,
                                            command);
}

static HAL_StatusTypeDef mqtt_link_queue_relay_config(
  const control_queue_relay_config_t *relay_config,
  uint32_t target_node_id,
  const char *request_id)
{
  cluster_service_state_t cluster_state;
  const cluster_node_t *remote_node;

  cluster_service_get_state(&cluster_state);
  if ((target_node_id == 0U) || (target_node_id == cluster_state.self_node_id))
  {
    return control_queue_submit_relay_config_async(0U,
                                                   target_node_id,
                                                   relay_config,
                                                   request_id);
  }

  remote_node = cluster_service_find_node(target_node_id);
  if ((remote_node == NULL) ||
      (remote_node->online == 0U) ||
      (remote_node->ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  return control_queue_submit_relay_config_async(remote_node->ip_addr,
                                                 remote_node->node_id,
                                                 relay_config,
                                                 request_id);
}

static uint8_t mqtt_link_is_local_target(uint32_t target_node_id)
{
  cluster_service_state_t cluster_state;

  cluster_service_get_state(&cluster_state);
  return ((target_node_id == 0U) ||
          (target_node_id == cluster_state.self_node_id)) ? 1U : 0U;
}

static HAL_StatusTypeDef mqtt_link_forward_remote_http(uint32_t target_node_id,
                                                       const char *method,
                                                       const char *path,
                                                       const char *body)
{
  const cluster_node_t *remote_node;
  uint32_t remote_request_id = 0U;

  remote_node = cluster_service_find_node(target_node_id);
  if ((remote_node == NULL) ||
      (remote_node->online == 0U) ||
      (remote_node->ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  return control_queue_submit_remote_http_async(remote_node->ip_addr,
                                                method,
                                                path,
                                                body,
                                                &remote_request_id);
}

static void mqtt_link_json_escape_copy(char *dst, size_t dst_size, const char *src, size_t src_max_len)
{
  size_t used = 0U;
  size_t index;

  if ((dst == NULL) || (dst_size == 0U))
  {
    return;
  }
  dst[0] = '\0';
  if (src == NULL)
  {
    return;
  }
  for (index = 0U; (index < src_max_len) && (src[index] != '\0') && ((used + 1U) < dst_size); ++index)
  {
    const char c = src[index];
    if ((c == '\"') || (c == '\\'))
    {
      if ((used + 2U) >= dst_size)
      {
        break;
      }
      dst[used++] = '\\';
      dst[used++] = c;
    }
    else if ((unsigned char)c >= 0x20U)
    {
      dst[used++] = c;
    }
  }
  dst[used] = '\0';
}

static void mqtt_link_sanitize_topic_token(char *text)
{
  size_t index;

  if (text == NULL)
  {
    return;
  }
  for (index = 0U; text[index] != '\0'; ++index)
  {
    if ((isalnum((unsigned char)text[index]) == 0) &&
        (text[index] != '-') &&
        (text[index] != '_') &&
        (text[index] != '.'))
    {
      text[index] = '_';
    }
  }
}

static void mqtt_link_format_query_error(char *buffer,
                                         size_t buffer_size,
                                         const char *request_id,
                                         const char *error_text)
{
  (void)snprintf(buffer, buffer_size,
                 "{\"ok\":false,\"request_id\":\"%s\",\"error\":\"%s\"}",
                 (request_id != NULL) ? request_id : "",
                 (error_text != NULL) ? error_text : "error");
}

static err_t mqtt_link_publish_query_response(const char *request_id,
                                              const char *payload)
{
  char topic[MQTT_LINK_ACK_TOPIC_MAX_LEN];
  char safe_request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];

  if ((request_id == NULL) || (request_id[0] == '\0') || (payload == NULL))
  {
    return ERR_ARG;
  }
  mqtt_link_copy_text(safe_request_id, sizeof(safe_request_id), request_id);
  mqtt_link_sanitize_topic_token(safe_request_id);
  (void)snprintf(topic, sizeof(topic), "%s%s",
                 g_mqtt_link_context.stats.topic_query_response_prefix,
                 safe_request_id);
  return mqtt_link_publish(topic, payload, 0U, MQTT_LINK_PUBLISH_QUERY_RESPONSE);
}

static void mqtt_link_format_relay_config_response(char *buffer, size_t buffer_size, const char *request_id)
{
  device_config_t cfg;
  size_t pos;
  uint32_t index;

  device_config_copy(&cfg);
  pos = (size_t)snprintf(buffer, buffer_size,
                         "{\"ok\":true,\"source\":\"mqtt-query\",\"request_id\":\"%s\","
                         "\"compact\":\"relays-v1\",\"relays\":[",
                         (request_id != NULL) ? request_id : "");
  for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
  {
    char name_json[(DEVICE_CONFIG_NAME_MAX_LEN * 2U) + 4U];
    int written;

    mqtt_link_json_escape_copy(name_json, sizeof(name_json),
                               cfg.relay[index].name,
                               sizeof(cfg.relay[index].name));
    written = snprintf(buffer + pos, buffer_size - pos,
                       "%s[%lu,\"%s\",%u,%u,%u,%u,%u,%u,%lu,%lu,%lu]",
                       (index == 0U) ? "" : ",",
                       (unsigned long)(index + 1U),
                       name_json,
                       (unsigned int)cfg.relay[index].enabled,
                       (unsigned int)cfg.relay[index].remember_state,
                       (unsigned int)cfg.relay[index].boot_default_on,
                       (unsigned int)cfg.current[index].adc_channel,
                       (unsigned int)cfg.current[index].protection_enabled,
                       (unsigned int)cfg.current[index].device_strategy,
                       (unsigned long)cfg.current[index].overcurrent_milliamps,
                       (unsigned long)cfg.current[index].trip_delay_ms,
                       (unsigned long)cfg.current[index].reset_hysteresis_milliamps);
    if ((written < 0) || ((size_t)written >= (buffer_size - pos)))
    {
      mqtt_link_format_query_error(buffer, buffer_size, request_id, "response_too_large");
      return;
    }
    pos += (size_t)written;
  }
  (void)snprintf(buffer + pos, buffer_size - pos, "]}");
}

static void mqtt_link_format_protection_config_response(char *buffer, size_t buffer_size, const char *request_id)
{
  relay_protection_user_config_t cfg;
  relay_protection_get_user_config(&cfg);
  (void)snprintf(buffer, buffer_size,
                 "{\"ok\":true,\"source\":\"mqtt-query\",\"request_id\":\"%s\","
                 "\"cooldown_seconds\":%lu,\"retry_delay_seconds\":%lu,"
                 "\"max_retry_count\":%lu,\"boot_delay_seconds\":%lu,"
                 "\"learning_samples\":%lu,\"aging_update_hours\":%lu,"
                 "\"aging_cap_percent\":%lu,\"relay_on_spacing_ms\":%lu,"
                 "\"same_relay_on_spacing_ms\":%lu,\"stall_multiplier_x10\":%lu}",
                 (request_id != NULL) ? request_id : "",
                 (unsigned long)cfg.cooldown_seconds,
                 (unsigned long)cfg.retry_delay_seconds,
                 (unsigned long)cfg.max_retry_count,
                 (unsigned long)cfg.boot_delay_seconds,
                 (unsigned long)cfg.learning_samples,
                 (unsigned long)cfg.aging_update_hours,
                 (unsigned long)cfg.aging_cap_percent,
                 (unsigned long)cfg.relay_on_spacing_ms,
                 (unsigned long)cfg.same_relay_on_spacing_ms,
                 (unsigned long)cfg.stall_timeout_multiplier_x10);
}

static void mqtt_link_format_protection_response(char *buffer, size_t buffer_size, const char *request_id)
{
  relay_protection_global_summary_t gs;
  size_t pos;
  uint8_t ch;

  relay_protection_get_global_summary(&gs);
  pos = (size_t)snprintf(buffer, buffer_size,
                         "{\"ok\":true,\"source\":\"mqtt-query\",\"request_id\":\"%s\","
                         "\"compact\":\"protection-v1\","
                         "\"boot_safety_active\":%u,\"boot_delay_until_ms\":%lu,"
                         "\"restart_count\":%lu,\"fault_locked_any\":%u,"
                         "\"fault_lock_mask\":%lu,\"channels\":[",
                         (request_id != NULL) ? request_id : "",
                         (unsigned int)gs.boot_safety_active,
                         (unsigned long)gs.boot_delay_until_ms,
                         (unsigned long)gs.restart_count,
                         (unsigned int)gs.fault_locked_any,
                         (unsigned long)relay_protection_fault_lock_mask());
  for (ch = 0U; ch < RELAY_PROTECTION_CHANNEL_COUNT; ++ch)
  {
    relay_protection_summary_t ps;
    int written;
    (void)relay_protection_get_summary((uint8_t)(ch + 1U), &ps);
    written = snprintf(buffer + pos, buffer_size - pos,
                       "%s[%u,%u,%u,%u,%u,%u,%u,%u,"
                       "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                       "%u,%u,%u,%u,%u,%u]",
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
                       (unsigned long)ps.learning_samples_collected,
                       (unsigned int)ps.ct_valid,
                       (unsigned int)ps.ct_degraded,
                       (unsigned int)ps.adc_saturated,
                       (unsigned int)ps.device_strategy,
                       (unsigned int)((ps.off_since_ms != 0U) ? 1U : 0U),
                       (unsigned int)ps.stuck_check_pending);
    if ((written < 0) || ((size_t)written >= (buffer_size - pos)))
    {
      mqtt_link_format_query_error(buffer, buffer_size, request_id, "response_too_large");
      return;
    }
    pos += (size_t)written;
  }
  (void)snprintf(buffer + pos, buffer_size - pos, "]}");
}

static void mqtt_link_format_scheduler_status_response(char *buffer,
                                                       size_t buffer_size,
                                                       const char *request_id,
                                                       uint32_t channel)
{
  channel_scheduler_state_t st;
  uint8_t time_valid = 0U;
  uint32_t now_unix;
  uint32_t remain = 0U;
  char reason_json[96];

  if ((channel == 0U) || (channel > SCHEDULER_CHANNEL_COUNT) ||
      (scheduler_get_state_copy((uint8_t)channel, &st) == 0U))
  {
    mqtt_link_format_query_error(buffer, buffer_size, request_id, "not_found");
    return;
  }
  now_unix = cluster_service_get_unix_time(&time_valid);
  mqtt_link_json_escape_copy(reason_json, sizeof(reason_json),
                             st.auto_hold.reason,
                             sizeof(st.auto_hold.reason));
  if (st.auto_hold.session != 0U)
  {
    const uint32_t now_ms = HAL_GetTick();
    remain = (st.auto_hold.until_ms > now_ms) ?
      ((st.auto_hold.until_ms - now_ms) / 1000U) : 0U;
  }
  (void)snprintf(buffer, buffer_size,
                 "{\"ok\":true,\"source\":\"mqtt-query\",\"request_id\":\"%s\","
                 "\"channel\":%lu,\"schedule_enabled\":%u,"
                 "\"pause_active\":%u,\"schedule_pause_until\":%lu,"
                 "\"pause_blocks_auto_trigger\":%u,\"auto_hold_active\":%u,"
                 "\"auto_hold_target\":%u,\"auto_hold_start_state\":%u,"
                 "\"auto_hold_remaining_seconds\":%lu,\"auto_hold_session\":%lu,"
                 "\"auto_hold_last_command_ts\":%lu,\"auto_hold_reason\":\"%s\","
                 "\"time_valid\":%u}",
                 (request_id != NULL) ? request_id : "",
                 (unsigned long)channel,
                 (unsigned int)st.schedule_enabled,
                 (unsigned int)((st.schedule_pause_until != 0U) &&
                                (now_unix < st.schedule_pause_until)),
                 (unsigned long)st.schedule_pause_until,
                 (unsigned int)st.pause_blocks_auto_trigger,
                 (unsigned int)(st.auto_hold.session != 0U),
                 (unsigned int)st.auto_hold.target_state,
                 (unsigned int)st.auto_hold.start_state,
                 (unsigned long)remain,
                 (unsigned long)st.auto_hold.session,
                 (unsigned long)st.last_auto_hold_command_ts,
                 reason_json,
                 (unsigned int)time_valid);
}

static void mqtt_link_format_scheduler_week_response(char *buffer,
                                                     size_t buffer_size,
                                                     const char *request_id,
                                                     uint32_t channel)
{
  weekly_schedule_t ws;
  channel_scheduler_state_t st;
  size_t pos;
  uint8_t day_index;

  if ((channel == 0U) || (channel > SCHEDULER_CHANNEL_COUNT) ||
      (scheduler_get_weekly_copy((uint8_t)channel, &ws) == 0U) ||
      (scheduler_get_state_copy((uint8_t)channel, &st) == 0U))
  {
    mqtt_link_format_query_error(buffer, buffer_size, request_id, "not_found");
    return;
  }
  pos = (size_t)snprintf(buffer, buffer_size,
                         "{\"ok\":true,\"source\":\"mqtt-query\",\"request_id\":\"%s\","
                         "\"channel\":%lu,\"schedule_enabled\":%u,\"days\":[",
                         (request_id != NULL) ? request_id : "",
                         (unsigned long)channel,
                         (unsigned int)st.schedule_enabled);
  for (day_index = 0U; day_index < 7U; ++day_index)
  {
    const daily_schedule_t *day = &ws.days[day_index];
    uint8_t interval_index;
    int written = snprintf(buffer + pos, buffer_size - pos,
                           "%s{\"weekday\":%u,\"intervals\":[",
                           (day_index == 0U) ? "" : ",",
                           (unsigned int)day_index);
    if ((written < 0) || ((size_t)written >= (buffer_size - pos)))
    {
      mqtt_link_format_query_error(buffer, buffer_size, request_id, "response_too_large");
      return;
    }
    pos += (size_t)written;
    for (interval_index = 0U; interval_index < day->interval_count; ++interval_index)
    {
      written = snprintf(buffer + pos, buffer_size - pos,
                         "%s{\"start_minute\":%u,\"end_minute\":%u}",
                         (interval_index == 0U) ? "" : ",",
                         (unsigned int)day->intervals[interval_index].start_minute,
                         (unsigned int)day->intervals[interval_index].end_minute);
      if ((written < 0) || ((size_t)written >= (buffer_size - pos)))
      {
        mqtt_link_format_query_error(buffer, buffer_size, request_id, "response_too_large");
        return;
      }
      pos += (size_t)written;
    }
    written = snprintf(buffer + pos, buffer_size - pos, "]}");
    if ((written < 0) || ((size_t)written >= (buffer_size - pos)))
    {
      mqtt_link_format_query_error(buffer, buffer_size, request_id, "response_too_large");
      return;
    }
    pos += (size_t)written;
  }
  (void)snprintf(buffer + pos, buffer_size - pos, "]}");
}

static uint8_t mqtt_link_query_path_to_local_response(const char *path,
                                                      const char *request_id,
                                                      char *buffer,
                                                      size_t buffer_size)
{
  uint32_t channel = 0U;

  if ((path == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }
  if (strcmp(path, "/api/config/relays") == 0)
  {
    mqtt_link_format_relay_config_response(buffer, buffer_size, request_id);
    return 1U;
  }
  if (strcmp(path, "/api/protection") == 0)
  {
    mqtt_link_format_protection_response(buffer, buffer_size, request_id);
    return 1U;
  }
  if (strcmp(path, "/api/config/protection") == 0)
  {
    mqtt_link_format_protection_config_response(buffer, buffer_size, request_id);
    return 1U;
  }
  {
    unsigned long parsed_channel = 0UL;
    int consumed = 0;
    if (sscanf(path, "/api/relay/channels/%lu/scheduler/status%n",
               &parsed_channel, &consumed) == 1)
    {
      if ((consumed > 0) && (path[consumed] == '\0'))
      {
        channel = (uint32_t)parsed_channel;
        mqtt_link_format_scheduler_status_response(buffer, buffer_size, request_id, channel);
        return 1U;
      }
    }
  }
  {
    unsigned long parsed_channel = 0UL;
    int consumed = 0;
    if (sscanf(path, "/api/relay/channels/%lu/schedule-week%n",
               &parsed_channel, &consumed) == 1)
    {
      if ((consumed > 0) && (path[consumed] == '\0'))
      {
        channel = (uint32_t)parsed_channel;
        mqtt_link_format_scheduler_week_response(buffer, buffer_size, request_id, channel);
        return 1U;
      }
    }
  }
  return 0U;
}

static int32_t mqtt_link_find_free_pending_query_slot(void)
{
  uint32_t index;
  for (index = 0U; index < MQTT_LINK_QUERY_PENDING_SLOTS; ++index)
  {
    if (g_mqtt_link_context.pending_queries[index].active == 0U)
    {
      return (int32_t)index;
    }
  }
  return -1;
}

static void mqtt_link_pending_query_poll(void)
{
  uint32_t index;
  char *response = g_mqtt_query_response;

  for (index = 0U; index < MQTT_LINK_QUERY_PENDING_SLOTS; ++index)
  {
    mqtt_link_pending_query_t *slot = &g_mqtt_link_context.pending_queries[index];
    uint8_t done = 0U;
    uint8_t remote_ok = 0U;

    if (slot->active == 0U)
    {
      continue;
    }
    if ((int32_t)(HAL_GetTick() - slot->started_ms) > 8000)
    {
      mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN,
                                   slot->request_id, "remote_timeout");
      (void)mqtt_link_publish_query_response(slot->request_id, response);
      memset(slot, 0, sizeof(*slot));
      return;
    }
    if (control_queue_get_remote_http_result(slot->remote_request_id,
                                             &done,
                                             &remote_ok,
                                             response,
                                             MQTT_LINK_QUERY_RESPONSE_MAX_LEN) != HAL_OK)
    {
      continue;
    }
    if (done == 0U)
    {
      continue;
    }
    if (remote_ok == 0U)
    {
      mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN,
                                   slot->request_id, "remote_query_failed");
    }
    (void)mqtt_link_publish_query_response(slot->request_id, response);
    memset(slot, 0, sizeof(*slot));
    return;
  }
}

static void mqtt_link_process_query_request(void)
{
  uint32_t target_node_id = 0U;
  char request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];
  char path[CONTROL_QUEUE_REMOTE_HTTP_PATH_MAX_LEN];
  char *response = g_mqtt_query_response;

  request_id[0] = '\0';
  path[0] = '\0';
  (void)mqtt_link_extract_json_string(g_mqtt_link_context.rx_payload,
                                      "request_id",
                                      request_id,
                                      sizeof(request_id));
  (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                   "node_id",
                                   &target_node_id);
  (void)mqtt_link_extract_json_string(g_mqtt_link_context.rx_payload,
                                      "path",
                                      path,
                                      sizeof(path));
  if (request_id[0] == '\0')
  {
    mqtt_link_copy_text(request_id, sizeof(request_id), "latest");
  }
  mqtt_link_sanitize_topic_token(request_id);
  if (path[0] == '\0')
  {
    mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN, request_id, "bad_path");
    (void)mqtt_link_publish_query_response(request_id, response);
    return;
  }
  if (mqtt_link_is_local_target(target_node_id) != 0U)
  {
    if (mqtt_link_query_path_to_local_response(path, request_id, response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN) == 0U)
    {
      mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN, request_id, "unsupported_path");
    }
    (void)mqtt_link_publish_query_response(request_id, response);
    return;
  }
  {
    const cluster_node_t *remote_node = cluster_service_find_node(target_node_id);
    uint32_t remote_request_id = 0U;
    int32_t slot_index;
    const char *remote_path = path;

    if ((remote_node == NULL) || (remote_node->online == 0U) ||
        (remote_node->ip_addr == 0U))
    {
      mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN, request_id, "target_unavailable");
      (void)mqtt_link_publish_query_response(request_id, response);
      return;
    }
    slot_index = mqtt_link_find_free_pending_query_slot();
    if (slot_index < 0)
    {
      mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN, request_id, "query_busy");
      (void)mqtt_link_publish_query_response(request_id, response);
      return;
    }
    if (strcmp(path, "/api/config/relays") == 0)
    {
      remote_path = "/api/config/relays-compact";
    }
    else if (strcmp(path, "/api/protection") == 0)
    {
      remote_path = "/api/protection-compact";
    }
    if (control_queue_submit_remote_http_async(remote_node->ip_addr,
                                               "GET",
                                               remote_path,
                                               "",
                                               &remote_request_id) != HAL_OK)
    {
      mqtt_link_format_query_error(response, MQTT_LINK_QUERY_RESPONSE_MAX_LEN, request_id, "remote_submit_failed");
      (void)mqtt_link_publish_query_response(request_id, response);
      return;
    }
    memset(&g_mqtt_link_context.pending_queries[(uint32_t)slot_index],
           0,
           sizeof(g_mqtt_link_context.pending_queries[(uint32_t)slot_index]));
    g_mqtt_link_context.pending_queries[(uint32_t)slot_index].active = 1U;
    g_mqtt_link_context.pending_queries[(uint32_t)slot_index].remote_request_id = remote_request_id;
    g_mqtt_link_context.pending_queries[(uint32_t)slot_index].started_ms = HAL_GetTick();
    mqtt_link_copy_text(g_mqtt_link_context.pending_queries[(uint32_t)slot_index].request_id,
                        sizeof(g_mqtt_link_context.pending_queries[(uint32_t)slot_index].request_id),
                        request_id);
  }
}

static HAL_StatusTypeDef mqtt_link_forward_or_apply_scheduler_resume(uint32_t target_node_id,
                                                                     uint32_t channel)
{
  char path[96];

  if ((channel == 0U) || (channel > SCHEDULER_CHANNEL_COUNT))
  {
    return HAL_ERROR;
  }
  if (mqtt_link_is_local_target(target_node_id) != 0U)
  {
    scheduler_clear_pause((uint8_t)channel);
    return HAL_OK;
  }
  (void)snprintf(path, sizeof(path),
                 "/api/relay/channels/%lu/scheduler/resume",
                 (unsigned long)channel);
  return mqtt_link_forward_remote_http(target_node_id, "POST", path, "");
}

static HAL_StatusTypeDef mqtt_link_forward_or_apply_protection_action(uint32_t target_node_id,
                                                                      const char *action,
                                                                      uint32_t channel,
                                                                      uint32_t strategy)
{
  char path[64];
  char body[64];

  if ((action == NULL) || (channel == 0U) ||
      (channel > RELAY_PROTECTION_CHANNEL_COUNT))
  {
    return HAL_ERROR;
  }

  if (mqtt_link_is_local_target(target_node_id) != 0U)
  {
    if (strcmp(action, "force-stop") == 0)
    {
      relay_protection_force_stop((uint8_t)channel);
    }
    else if (strcmp(action, "unlock") == 0)
    {
      relay_protection_unlock((uint8_t)channel);
    }
    else if (strcmp(action, "clear-learning") == 0)
    {
      relay_protection_clear_learning((uint8_t)channel);
    }
    else if (strcmp(action, "strategy") == 0)
    {
      if (strategy > RELAY_PROTECTION_STRATEGY_MANUAL)
      {
        return HAL_ERROR;
      }
      relay_protection_set_strategy((uint8_t)channel, (uint8_t)strategy);
    }
    else
    {
      return HAL_ERROR;
    }
    return HAL_OK;
  }

  (void)snprintf(path, sizeof(path), "/api/protection/%s", action);
  if (strcmp(action, "strategy") == 0)
  {
    (void)snprintf(body, sizeof(body), "channel=%lu&strategy=%lu",
                   (unsigned long)channel,
                   (unsigned long)strategy);
  }
  else
  {
    (void)snprintf(body, sizeof(body), "channel=%lu", (unsigned long)channel);
  }
  return mqtt_link_forward_remote_http(target_node_id, "POST", path, body);
}

static HAL_StatusTypeDef mqtt_link_forward_or_apply_cluster_config(uint32_t target_node_id)
{
  device_config_t persisted;
  char device_name[DEVICE_CONFIG_NAME_MAX_LEN];
  char body[DEVICE_CONFIG_NAME_MAX_LEN + 16U];
  int written;

  if (mqtt_link_extract_json_string(g_mqtt_link_context.rx_payload,
                                    "device_name",
                                    device_name,
                                    sizeof(device_name)) == 0U)
  {
    return HAL_ERROR;
  }
  if (device_name[0] == '\0')
  {
    return HAL_ERROR;
  }

  if (mqtt_link_is_local_target(target_node_id) != 0U)
  {
    persisted = *device_config_get();
    memset(persisted.device_name, 0, sizeof(persisted.device_name));
    strncpy(persisted.device_name, device_name, sizeof(persisted.device_name) - 1U);
    return (device_config_update_and_save(&persisted) != 0U) ? HAL_OK : HAL_ERROR;
  }

  written = snprintf(body, sizeof(body), "device_name=%s", device_name);
  if ((written < 0) || ((size_t)written >= sizeof(body)))
  {
    return HAL_ERROR;
  }
  return mqtt_link_forward_remote_http(target_node_id, "POST", "/api/config/cluster", body);
}

static HAL_StatusTypeDef mqtt_link_forward_or_apply_protection_config(uint32_t target_node_id)
{
  relay_protection_user_config_t cfg;
  device_config_t persisted;
  uint32_t value;
  char body[256];
  int written;

  relay_protection_get_user_config(&cfg);
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "cooldown_seconds", &value) != 0U)
  {
    cfg.cooldown_seconds = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "retry_delay_seconds", &value) != 0U)
  {
    cfg.retry_delay_seconds = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "max_retry_count", &value) != 0U)
  {
    cfg.max_retry_count = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "boot_delay_seconds", &value) != 0U)
  {
    cfg.boot_delay_seconds = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "learning_samples", &value) != 0U)
  {
    cfg.learning_samples = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "aging_update_hours", &value) != 0U)
  {
    cfg.aging_update_hours = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "aging_cap_percent", &value) != 0U)
  {
    cfg.aging_cap_percent = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "relay_on_spacing_ms", &value) != 0U)
  {
    cfg.relay_on_spacing_ms = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "same_relay_on_spacing_ms", &value) != 0U)
  {
    cfg.same_relay_on_spacing_ms = value;
  }
  if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "stall_multiplier_x10", &value) != 0U)
  {
    cfg.stall_timeout_multiplier_x10 = value;
  }

  if (mqtt_link_is_local_target(target_node_id) != 0U)
  {
    relay_protection_set_user_config(&cfg);
    persisted = *device_config_get();
    persisted.protection_global.cooldown_seconds = cfg.cooldown_seconds;
    persisted.protection_global.retry_delay_seconds = cfg.retry_delay_seconds;
    persisted.protection_global.max_retry_count = cfg.max_retry_count;
    persisted.protection_global.boot_delay_seconds = cfg.boot_delay_seconds;
    persisted.protection_global.learning_samples = cfg.learning_samples;
    persisted.protection_global.aging_update_hours = cfg.aging_update_hours;
    persisted.protection_global.aging_cap_percent = cfg.aging_cap_percent;
    persisted.protection_global.stall_timeout_multiplier_x10 = cfg.stall_timeout_multiplier_x10;
    persisted.protection_global.relay_on_spacing_ms = cfg.relay_on_spacing_ms;
    persisted.protection_global.same_relay_on_spacing_ms = cfg.same_relay_on_spacing_ms;
    return (device_config_update_and_save(&persisted) != 0U) ? HAL_OK : HAL_ERROR;
  }

  written = snprintf(body, sizeof(body),
                     "cooldown_seconds=%lu&retry_delay_seconds=%lu&max_retry_count=%lu"
                     "&boot_delay_seconds=%lu&learning_samples=%lu&aging_update_hours=%lu"
                     "&aging_cap_percent=%lu&stall_multiplier_x10=%lu"
                     "&relay_on_spacing_ms=%lu&same_relay_on_spacing_ms=%lu",
                     (unsigned long)cfg.cooldown_seconds,
                     (unsigned long)cfg.retry_delay_seconds,
                     (unsigned long)cfg.max_retry_count,
                     (unsigned long)cfg.boot_delay_seconds,
                     (unsigned long)cfg.learning_samples,
                     (unsigned long)cfg.aging_update_hours,
                     (unsigned long)cfg.aging_cap_percent,
                     (unsigned long)cfg.stall_timeout_multiplier_x10,
                     (unsigned long)cfg.relay_on_spacing_ms,
                     (unsigned long)cfg.same_relay_on_spacing_ms);
  if ((written < 0) || ((size_t)written >= sizeof(body)))
  {
    return HAL_ERROR;
  }
  return mqtt_link_forward_remote_http(target_node_id,
                                       "POST",
                                       "/api/config/protection",
                                       body);
}

static void mqtt_link_process_command(void)
{
  control_proto_command_t command;
  control_queue_relay_config_t relay_config;
  uint32_t channel = 0U;
  uint32_t value = 0U;
  uint32_t mask = 0U;
  uint32_t unix_time = 0U;
  uint32_t target_node_id = 0U;
  char request_id[MQTT_LINK_REQUEST_ID_MAX_LEN];
  HAL_StatusTypeDef queue_status;

  memset(&command, 0, sizeof(command));
  memset(&relay_config, 0, sizeof(relay_config));
  request_id[0] = '\0';
  g_mqtt_link_context.stats.received_commands++;

  (void)mqtt_link_extract_json_string(g_mqtt_link_context.rx_payload,
                                      "request_id",
                                      request_id,
                                      sizeof(request_id));
  (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                   "node_id",
                                   &target_node_id);
  if (strcmp(g_mqtt_link_context.rx_topic,
             g_mqtt_link_context.stats.topic_query_request) == 0)
  {
    mqtt_link_process_query_request();
    return;
  }
  if (request_id[0] == '\0')
  {
    mqtt_link_copy_text(request_id, sizeof(request_id), "latest");
  }

  if (strcmp(g_mqtt_link_context.rx_topic,
             g_mqtt_link_context.stats.topic_command_time) == 0)
  {
    if (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                   "unix_time",
                                   &unix_time) == 0U)
    {
      g_mqtt_link_context.stats.command_errors++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 7U;
      mqtt_link_publish_ack(request_id, 0U, "bad_time_request", target_node_id);
      return;
    }

    taskENTER_CRITICAL();
    if (g_mqtt_link_context.time_request_pending != 0U)
    {
      taskEXIT_CRITICAL();
      mqtt_link_publish_ack(request_id, 0U, "time_busy", target_node_id);
      return;
    }
    g_mqtt_link_context.pending_unix_time = unix_time;
    g_mqtt_link_context.pending_time_target_node_id = target_node_id;
    mqtt_link_copy_text(g_mqtt_link_context.pending_time_request_id,
                        sizeof(g_mqtt_link_context.pending_time_request_id),
                        request_id);
    g_mqtt_link_context.time_request_pending = 1U;
    taskEXIT_CRITICAL();
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                  g_mqtt_link_context.stats.topic_command_config_relay) == 0)
  {
    uint8_t malformed = 0U;

    if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                    "channel",
                                    &channel) == 0U) ||
        (channel == 0U) ||
        (channel > DEVICE_CONFIG_RELAY_COUNT))
    {
      malformed = 1U;
    }
    relay_config.channel = (uint8_t)channel;

    if (strstr(g_mqtt_link_context.rx_payload, "\"name\"") != NULL)
    {
      if (mqtt_link_extract_json_string(g_mqtt_link_context.rx_payload,
                                        "name",
                                        relay_config.name,
                                        sizeof(relay_config.name)) == 0U)
      {
        malformed = 1U;
      }
      else
      {
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_NAME_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"enabled\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "enabled",
                                      &value) == 0U) ||
          (value > 1U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.enabled = (uint8_t)value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_ENABLED_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"remember_state\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "remember_state",
                                      &value) == 0U) ||
          (value > 1U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.remember_state = (uint8_t)value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_REMEMBER_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"boot_default_on\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "boot_default_on",
                                      &value) == 0U) ||
          (value > 1U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.boot_default_on = (uint8_t)value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_BOOT_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"adc_channel\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "adc_channel",
                                      &value) == 0U) ||
          ((value >= ADC_SERVICE_CHANNEL_COUNT) &&
           (value != DEVICE_CONFIG_ADC_CHANNEL_UNASSIGNED)))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.adc_channel = (uint8_t)value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_ADC_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"protection_enabled\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "protection_enabled",
                                      &value) == 0U) ||
          (value > 1U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.protection_enabled = (uint8_t)value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_PROTECTION_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"device_strategy\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "device_strategy",
                                      &value) == 0U) ||
          (value > RELAY_PROTECTION_STRATEGY_MANUAL))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.device_strategy = (uint8_t)value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_STRATEGY_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"overcurrent_milliamps\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "overcurrent_milliamps",
                                      &value) == 0U) ||
          (value < 100U) || (value > 100000U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.overcurrent_milliamps = value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_THRESHOLD_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload, "\"trip_delay_ms\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "trip_delay_ms",
                                      &value) == 0U) ||
          (value > 600000U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.trip_delay_ms = value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_DELAY_PRESENT;
      }
    }
    if (strstr(g_mqtt_link_context.rx_payload,
               "\"reset_hysteresis_milliamps\"") != NULL)
    {
      if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload,
                                      "reset_hysteresis_milliamps",
                                      &value) == 0U) ||
          (value > 100000U))
      {
        malformed = 1U;
      }
      else
      {
        relay_config.reset_hysteresis_milliamps = value;
        relay_config.fields |= CONTROL_QUEUE_RELAY_CONFIG_HYSTERESIS_PRESENT;
      }
    }

    if ((malformed != 0U) || (relay_config.fields == 0U))
    {
      g_mqtt_link_context.stats.command_errors++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 8U;
      mqtt_link_publish_ack(request_id, 0U, "bad_relay_config", target_node_id);
      return;
    }

    queue_status =
      mqtt_link_queue_relay_config(&relay_config, target_node_id, request_id);
    if (queue_status != HAL_OK)
    {
      g_mqtt_link_context.stats.command_errors++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 9U;
      mqtt_link_publish_ack(request_id,
                            0U,
                            (queue_status == HAL_BUSY) ? "queue_full" : "target_unavailable",
                            target_node_id);
    }
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                  g_mqtt_link_context.stats.topic_command_config_cluster) == 0)
  {
    if (mqtt_link_forward_or_apply_cluster_config(target_node_id) != HAL_OK)
    {
      mqtt_link_publish_ack(request_id, 0U, "bad_cluster_config", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
             g_mqtt_link_context.stats.topic_command_relay) == 0)
  {
    if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &channel) == 0U) ||
        (mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "value", &value) == 0U) ||
        (channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT) || (value > 1U))
    {
      g_mqtt_link_context.stats.command_errors++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 1U;
      mqtt_link_publish_ack(request_id, 0U, "bad_relay_request", target_node_id);
      return;
    }
    command.type = CONTROL_PROTO_CMD_SET_RELAY;
    command.channel = (uint8_t)channel;
    command.value = (uint8_t)value;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                  g_mqtt_link_context.stats.topic_command_mask) == 0)
  {
    if ((mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "mask", &mask) == 0U) ||
        (mask > BOARD_RELAY_ALL_MASK))
    {
      g_mqtt_link_context.stats.command_errors++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 2U;
      mqtt_link_publish_ack(request_id, 0U, "bad_mask_request", target_node_id);
      return;
    }
    command.type = CONTROL_PROTO_CMD_SET_RELAY_MASK;
    command.mask = mask;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_auto_hold) == 0)
  {
    /* ── Auto Hold ── */
    auto_hold_cmd_t ah_cmd;
    uint32_t ah_ch = 0, ah_target = 0, ah_dur = 0, ah_session = 0, ah_cts = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &ah_ch);
    memset(&ah_cmd, 0, sizeof(ah_cmd));
    ah_cmd.channel = (uint8_t)ah_ch;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "target", &ah_target);
    ah_cmd.target = (uint8_t)(ah_target ? 1 : 0);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "duration_seconds", &ah_dur);
    ah_cmd.duration_seconds = ah_dur;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "session", &ah_session);
    ah_cmd.session = ah_session;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "command_ts", &ah_cts);
    ah_cmd.command_ts = ah_cts;
    (void)mqtt_link_extract_json_string(g_mqtt_link_context.rx_payload, "reason",
                                        ah_cmd.reason, SCHEDULER_REASON_MAX_LEN + 1);
    { uint8_t tv2; scheduler_handle_auto_hold(&ah_cmd, HAL_GetTick(),
                                               cluster_service_get_unix_time(&tv2)); }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_control) == 0)
  {
    /* ── User control (atomic) ── */
    uint32_t ctrl_ch = 0, ctrl_target = 0, ctrl_pmode = 0, ctrl_pdur = 0, ctrl_puntil = 0, ctrl_pblocks = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &ctrl_ch);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "target", &ctrl_target);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "pause_mode", &ctrl_pmode);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "pause_duration_seconds", &ctrl_pdur);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "pause_until", &ctrl_puntil);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "pause_blocks_auto_trigger", &ctrl_pblocks);
    scheduler_user_control((uint8_t)ctrl_ch, (uint8_t)(ctrl_target ? 1 : 0),
                           (scheduler_pause_mode_t)ctrl_pmode, ctrl_pdur, ctrl_puntil,
                           (uint8_t)(ctrl_pblocks ? 1 : 0));
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_scheduler_set) == 0)
  {
    /* ── Scheduler set weekly ── */
    uint32_t sch_ch = 0, sch_weekday = 0, sch_count = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &sch_ch);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "weekday", &sch_weekday);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "count", &sch_count);
    if (mqtt_link_is_local_target(target_node_id) == 0U)
    {
      char path[96];
      char body[192];
      size_t used;
      int written;
      (void)snprintf(path, sizeof(path),
                     "/api/relay/channels/%lu/schedule-week/%lu",
                     (unsigned long)sch_ch,
                     (unsigned long)sch_weekday);
      written = snprintf(body, sizeof(body), "count=%lu",
                         (unsigned long)sch_count);
      if ((written < 0) || ((size_t)written >= sizeof(body))) {
        mqtt_link_publish_ack(request_id, 0U, "bad_scheduler_set", target_node_id);
        return;
      }
      used = (size_t)written;
      for (uint32_t k = 0; k < sch_count && k < SCHEDULER_MAX_INTERVALS; k++) {
        uint32_t sv = 0, ev = 0;
        char key[8];
        (void)snprintf(key, sizeof(key), "s%lu", (unsigned long)k);
        (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, key, &sv);
        (void)snprintf(key, sizeof(key), "e%lu", (unsigned long)k);
        (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, key, &ev);
        written = snprintf(body + used, sizeof(body) - used,
                           "&s%lu=%lu&e%lu=%lu",
                           (unsigned long)k, (unsigned long)sv,
                           (unsigned long)k, (unsigned long)ev);
        if ((written < 0) || ((size_t)written >= (sizeof(body) - used))) {
          mqtt_link_publish_ack(request_id, 0U, "bad_scheduler_set", target_node_id);
          return;
        }
        used += (size_t)written;
      }
      if (mqtt_link_forward_remote_http(target_node_id, "PUT", path, body) != HAL_OK) {
        mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
        return;
      }
      mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
      return;
    }
    if (sch_weekday > 6 || sch_count > SCHEDULER_MAX_INTERVALS) {
      mqtt_link_publish_ack(request_id, 0U, "bad_scheduler_set", target_node_id);
      return;
    }
    daily_schedule_t new_day;
    memset(&new_day, 0, sizeof(new_day));
    new_day.interval_count = (uint8_t)sch_count;
    char key[8];
    for (uint32_t k = 0; k < sch_count; k++) {
      uint32_t sv = 0, ev = 0;
      (void)snprintf(key, sizeof(key), "s%lu", (unsigned long)k);
      (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, key, &sv);
      (void)snprintf(key, sizeof(key), "e%lu", (unsigned long)k);
      (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, key, &ev);
      if (sv >= 1440 || ev == 0 || ev > 1440 || sv >= ev) {
        mqtt_link_publish_ack(request_id, 0U, "bad_interval", target_node_id);
        return;
      }
      new_day.intervals[k].start_minute = (uint16_t)sv;
      new_day.intervals[k].end_minute = (uint16_t)ev;
    }
    if (scheduler_validate_intervals(&new_day) == false ||
        scheduler_set_weekly_day((uint8_t)sch_ch, (uint8_t)sch_weekday, &new_day) == 0U) {
      mqtt_link_publish_ack(request_id, 0U, "bad_scheduler_set", target_node_id);
      return;
    }
    mqtt_link_service_mark_status_dirty();
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_scheduler_enabled) == 0)
  {
    uint32_t sch_en_ch = 0, sch_enabled = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &sch_en_ch);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "enabled", &sch_enabled);
    if (mqtt_link_is_local_target(target_node_id) == 0U)
    {
      char path[96];
      char body[32];
      (void)snprintf(path, sizeof(path),
                     "/api/relay/channels/%lu/scheduler/enabled",
                     (unsigned long)sch_en_ch);
      (void)snprintf(body, sizeof(body), "enabled=%lu",
                     (unsigned long)(sch_enabled ? 1U : 0U));
      if (mqtt_link_forward_remote_http(target_node_id, "PATCH", path, body) != HAL_OK) {
        mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
        return;
      }
      mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
      return;
    }
    if (scheduler_set_enabled((uint8_t)sch_en_ch, (uint8_t)(sch_enabled ? 1 : 0)) == 0U) {
      mqtt_link_publish_ack(request_id, 0U, "not_found", target_node_id);
      return;
    }
    mqtt_link_service_mark_status_dirty();
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_scheduler_pause) == 0)
  {
    uint32_t sp_ch = 0, sp_mode = 0, sp_dur = 0, sp_until = 0, sp_blocks = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &sp_ch);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "mode", &sp_mode);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "duration_seconds", &sp_dur);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "until", &sp_until);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "pause_blocks_auto_trigger", &sp_blocks);
    if (mqtt_link_is_local_target(target_node_id) == 0U)
    {
      char path[96];
      char body[128];
      (void)snprintf(path, sizeof(path),
                     "/api/relay/channels/%lu/scheduler/pause",
                     (unsigned long)sp_ch);
      (void)snprintf(body, sizeof(body),
                     "mode=%lu&duration_seconds=%lu&until=%lu&pause_blocks_auto_trigger=%lu",
                     (unsigned long)sp_mode,
                     (unsigned long)sp_dur,
                     (unsigned long)sp_until,
                     (unsigned long)(sp_blocks ? 1U : 0U));
      if (mqtt_link_forward_remote_http(target_node_id, "POST", path, body) != HAL_OK) {
        mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
        return;
      }
      mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
      return;
    }
    {
      uint8_t tv;
      uint32_t now_unix = cluster_service_get_unix_time(&tv);
      uint32_t pause_until = now_unix + 60U;
      switch (sp_mode) {
        case 0: pause_until = now_unix + 60U; break;
        case 1: pause_until = now_unix + sp_dur; break;
        case 2: pause_until = sp_until; break;
        case 3: pause_until = SCHEDULER_PAUSE_FOREVER; break;
        default: pause_until = now_unix + 60U; break;
      }
      if (scheduler_set_pause((uint8_t)sp_ch, pause_until, (uint8_t)(sp_blocks ? 1 : 0)) == 0U) {
        mqtt_link_publish_ack(request_id, 0U, "not_found", target_node_id);
        return;
      }
    }
    mqtt_link_service_mark_status_dirty();
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_scheduler_resume) == 0)
  {
    uint32_t sr_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &sr_ch);
    if (mqtt_link_forward_or_apply_scheduler_resume(target_node_id, sr_ch) != HAL_OK) {
      mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_config_protection) == 0)
  {
    if (mqtt_link_forward_or_apply_protection_config(target_node_id) != HAL_OK) {
      mqtt_link_publish_ack(request_id, 0U, "bad_protection_config", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_strategy) == 0)
  {
    uint32_t prot_ch = 0, prot_strategy = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "strategy", &prot_strategy);
    if (mqtt_link_forward_or_apply_protection_action(target_node_id, "strategy",
                                                     prot_ch, prot_strategy) != HAL_OK) {
      mqtt_link_publish_ack(request_id, 0U, "bad_strategy", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_force_stop) == 0)
  {
    uint32_t prot_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    if (mqtt_link_forward_or_apply_protection_action(target_node_id, "force-stop",
                                                     prot_ch, 0U) != HAL_OK) {
      mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_unlock) == 0)
  {
    uint32_t prot_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    if (mqtt_link_forward_or_apply_protection_action(target_node_id, "unlock",
                                                     prot_ch, 0U) != HAL_OK) {
      mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_clear_learning) == 0)
  {
    uint32_t prot_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    if (mqtt_link_forward_or_apply_protection_action(target_node_id, "clear-learning",
                                                     prot_ch, 0U) != HAL_OK) {
      mqtt_link_publish_ack(request_id, 0U, "target_unavailable", target_node_id);
      return;
    }
    if (mqtt_link_is_local_target(target_node_id) != 0U)
    {
      mqtt_link_service_mark_status_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else
  {
    g_mqtt_link_context.stats.command_errors++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 3U;
    mqtt_link_publish_ack(request_id, 0U, "unsupported_command", target_node_id);
    return;
  }

  queue_status = mqtt_link_queue_command(&command, target_node_id);
  if (queue_status != HAL_OK)
  {
    g_mqtt_link_context.stats.command_errors++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 4U;
    mqtt_link_publish_ack(request_id,
                          0U,
                          (queue_status == HAL_BUSY) ? "queue_full" : "queue_failed",
                          target_node_id);
    return;
  }

  mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
  g_mqtt_link_context.status_dirty = 1U;
}

static void mqtt_link_incoming_publish(void *argument, const char *topic, u32_t total_length)
{
  (void)argument;

  g_mqtt_link_context.rx_used = 0U;
  g_mqtt_link_context.rx_expected = total_length;
  g_mqtt_link_context.rx_payload[0] = '\0';
  mqtt_link_copy_text(g_mqtt_link_context.rx_topic,
                      sizeof(g_mqtt_link_context.rx_topic),
                      topic);

  g_mqtt_link_context.rx_accept =
    ((total_length < sizeof(g_mqtt_link_context.rx_payload)) &&
     ((strcmp(topic, g_mqtt_link_context.stats.topic_command_relay) == 0) ||
      (strcmp(topic, g_mqtt_link_context.stats.topic_command_mask) == 0) ||
      (strcmp(topic, g_mqtt_link_context.stats.topic_command_config_relay) == 0) ||
      (strcmp(topic, g_mqtt_link_context.stats.topic_command_time) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_auto_hold) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_control) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_scheduler_set) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_scheduler_enabled) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_scheduler_pause) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_scheduler_resume) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_config_protection) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_strategy) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_force_stop) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_unlock) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_clear_learning) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_query_request) == 0))) ? 1U : 0U;

  if (g_mqtt_link_context.rx_accept == 0U)
  {
    g_mqtt_link_context.stats.command_errors++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 5U;
  }
}

static void mqtt_link_incoming_data(void *argument,
                                    const u8_t *data,
                                    u16_t length,
                                    u8_t flags)
{
  (void)argument;

  if (g_mqtt_link_context.rx_accept != 0U)
  {
    if (((size_t)g_mqtt_link_context.rx_used + length) <
        sizeof(g_mqtt_link_context.rx_payload))
    {
      memcpy(g_mqtt_link_context.rx_payload + g_mqtt_link_context.rx_used,
             data,
             length);
      g_mqtt_link_context.rx_used = (uint16_t)(g_mqtt_link_context.rx_used + length);
      g_mqtt_link_context.rx_payload[g_mqtt_link_context.rx_used] = '\0';
    }
    else
    {
      g_mqtt_link_context.rx_accept = 0U;
      g_mqtt_link_context.stats.command_errors++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 6U;
    }
  }

  if ((flags & MQTT_DATA_FLAG_LAST) != 0U)
  {
    if ((g_mqtt_link_context.rx_accept != 0U) &&
        (g_mqtt_link_context.rx_used == g_mqtt_link_context.rx_expected))
    {
      mqtt_link_process_command();
    }
    g_mqtt_link_context.rx_accept = 0U;
  }
}

static void mqtt_link_subscribe_commands(void)
{
  /* Build topic array and start SUBACK-driven chain.
     lwIP MQTT client only allows one outstanding subscribe at a time;
     each SUBACK triggers the next subscribe via the callback. */
  g_mqtt_sub_topics[0]  = g_mqtt_link_context.stats.topic_command_relay;
  g_mqtt_sub_topics[1]  = g_mqtt_link_context.stats.topic_command_mask;
  g_mqtt_sub_topics[2]  = g_mqtt_link_context.stats.topic_command_config_relay;
  g_mqtt_sub_topics[3]  = g_mqtt_link_context.stats.topic_command_config_cluster;
  g_mqtt_sub_topics[4]  = g_mqtt_link_context.stats.topic_command_time;
  g_mqtt_sub_topics[5]  = g_mqtt_link_context.stats.topic_command_auto_hold;
  g_mqtt_sub_topics[6]  = g_mqtt_link_context.stats.topic_command_control;
  g_mqtt_sub_topics[7]  = g_mqtt_link_context.stats.topic_command_scheduler_set;
  g_mqtt_sub_topics[8]  = g_mqtt_link_context.stats.topic_command_scheduler_enabled;
  g_mqtt_sub_topics[9]  = g_mqtt_link_context.stats.topic_command_scheduler_pause;
  g_mqtt_sub_topics[10] = g_mqtt_link_context.stats.topic_command_scheduler_resume;
  g_mqtt_sub_topics[11] = g_mqtt_link_context.stats.topic_command_config_protection;
  g_mqtt_sub_topics[12] = g_mqtt_link_context.stats.topic_command_protection_strategy;
  g_mqtt_sub_topics[13] = g_mqtt_link_context.stats.topic_command_protection_force_stop;
  g_mqtt_sub_topics[14] = g_mqtt_link_context.stats.topic_command_protection_unlock;
  g_mqtt_sub_topics[15] = g_mqtt_link_context.stats.topic_command_protection_clear_learning;
  g_mqtt_sub_topics[16] = g_mqtt_link_context.stats.topic_query_request;
  g_mqtt_sub_count = 17U;
  g_mqtt_sub_index = 0U;

  /* Kick off first subscribe — the rest chain via mqtt_link_subscription_callback */
  if (mqtt_subscribe(g_mqtt_link_context.client, g_mqtt_sub_topics[0],
                     1U, mqtt_link_subscription_callback, NULL) != ERR_OK)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_SUBSCRIBE;
  }
}

static void mqtt_link_connection_callback(mqtt_client_t *client,
                                          void *argument,
                                          mqtt_connection_status_t status)
{
  (void)argument;

  g_mqtt_link_context.connecting = 0U;
  if (status != MQTT_CONNECT_ACCEPTED)
  {
    g_mqtt_link_context.stats.connected = 0U;
    g_mqtt_link_context.stats.connect_failures++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_CONNECT + (uint32_t)status;
    mqtt_link_note_connect_failure();
    mqtt_link_log_value("[mqtt] disconnected status=", (uint32_t)status);
    mqtt_link_release_transport_keep_backoff(0U);
    /* TLS/PSK failure may mean bindingSecret changed — re-provision now */
    https_client_service_trigger();
    return;
  }

  g_mqtt_link_context.stats.connected = 1U;
  g_mqtt_link_context.stats.last_error = 0U;
  mqtt_link_note_connect_success();
  g_mqtt_link_context.last_heartbeat_ms = 0U;
  g_mqtt_link_context.last_status_ms = 0U;

  mqtt_set_inpub_callback(client,
                          mqtt_link_incoming_publish,
                          mqtt_link_incoming_data,
                          NULL);
  /* Subscribe BEFORE setting dirty/stage — ensures ring buffer has room
     for all 12 SUBSCRIBE packets before hello/status compete for space. */
  mqtt_link_subscribe_commands();
  g_mqtt_link_context.status_dirty = 1U;
  g_mqtt_link_context.initial_publish_stage = 1U;
  mqtt_link_log("[mqtt] connected\r\n");
}

static void mqtt_link_start_connect(const ip_addr_t *address)
{
  err_t error;

  if ((address == NULL) ||
      (g_mqtt_link_context.stats.should_run == 0U) ||
      (g_mqtt_link_context.config_generation != g_mqtt_link_context.dns_generation))
  {
    return;
  }

  if (g_mqtt_link_context.client == NULL)
  {
    g_mqtt_link_context.client = mqtt_client_new();
    if (g_mqtt_link_context.client == NULL)
    {
      g_mqtt_link_context.stats.connect_failures++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_CLIENT_ALLOC;
      mqtt_link_note_connect_failure();
      return;
    }
  }

  if (g_mqtt_link_context.tls_config == NULL)
  {
    g_mqtt_link_context.psk_len =
      mqtt_link_decode_base64url(g_mqtt_link_context.binding_secret,
                                 g_mqtt_link_context.psk,
                                 sizeof(g_mqtt_link_context.psk));
    if (g_mqtt_link_context.psk_len == 0U)
    {
      g_mqtt_link_context.stats.connect_failures++;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_PSK;
      mqtt_link_note_connect_failure();
      mqtt_link_release_transport_keep_backoff(0U);
      return;
    }

    g_mqtt_link_context.tls_config =
      altcp_tls_create_config_client_psk(g_mqtt_link_context.psk,
                                         g_mqtt_link_context.psk_len,
                                         (const u8_t *)g_mqtt_link_context.cloud_device_id,
                                         strlen(g_mqtt_link_context.cloud_device_id));
    if (g_mqtt_link_context.tls_config == NULL)
    {
      g_mqtt_link_context.stats.connect_failures++;
      g_mqtt_link_context.stats.last_error = mqtt_link_tls_config_error_code();
      mqtt_link_note_connect_failure();
      mqtt_link_log_value("[mqtt] tls cfg err=", g_mqtt_link_context.stats.last_error);
      mqtt_link_release_transport_keep_backoff(0U);
      return;
    }
  }

  memset(&g_mqtt_link_context.client_info, 0, sizeof(g_mqtt_link_context.client_info));
  (void)snprintf(g_mqtt_link_context.will_message,
                 sizeof(g_mqtt_link_context.will_message),
                 "{\"type\":\"status\",\"online\":0,\"device_id\":\"%s\"}",
                 g_mqtt_link_context.cloud_device_id);
  g_mqtt_link_context.client_info.client_id = g_mqtt_link_context.stats.client_id;
  g_mqtt_link_context.client_info.client_user = g_mqtt_link_context.cloud_key_id;
  g_mqtt_link_context.client_info.client_pass = g_mqtt_link_context.binding_secret;
  g_mqtt_link_context.client_info.keep_alive = MQTT_LINK_KEEP_ALIVE_SECONDS;
  g_mqtt_link_context.client_info.will_topic = g_mqtt_link_context.stats.topic_status;
  g_mqtt_link_context.client_info.will_msg = g_mqtt_link_context.will_message;
  g_mqtt_link_context.client_info.will_qos = 1U;
  g_mqtt_link_context.client_info.will_retain = 1U;
  g_mqtt_link_context.client_info.tls_config = g_mqtt_link_context.tls_config;

  g_mqtt_link_context.connecting = 1U;
  error = mqtt_client_connect(g_mqtt_link_context.client,
                              address,
                              g_mqtt_link_context.stats.broker_port,
                              mqtt_link_connection_callback,
                              NULL,
                              &g_mqtt_link_context.client_info);
  if (error != ERR_OK)
  {
    g_mqtt_link_context.connecting = 0U;
    g_mqtt_link_context.stats.connect_failures++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_CONNECT + (uint32_t)(-error);
    mqtt_link_note_connect_failure();
    mqtt_link_log_value("[mqtt] connect start fail=", (uint32_t)(-error));
    mqtt_link_release_transport_keep_backoff(0U);
  }
}

static void mqtt_link_dns_callback(const char *name,
                                   const ip_addr_t *address,
                                   void *argument)
{
  const uint32_t generation = (uint32_t)(uintptr_t)argument;

  if ((generation != g_mqtt_link_context.config_generation) ||
      (generation != g_mqtt_link_context.dns_generation))
  {
    return;
  }
  g_mqtt_link_context.dns_pending = 0U;
  if ((name == NULL) ||
      (strcmp(name, g_mqtt_link_context.broker_host) != 0) ||
      (address == NULL))
  {
    g_mqtt_link_context.stats.connect_failures++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_DNS;
    mqtt_link_note_connect_failure();
    mqtt_link_log("[mqtt] dns failed\r\n");
    return;
  }

  mqtt_link_start_connect(address);
}

static void mqtt_link_begin_connect(void)
{
  ip_addr_t address;
  err_t error;

  g_mqtt_link_context.last_connect_attempt_ms = HAL_GetTick();
  g_mqtt_link_context.stats.connect_attempts++;
  g_mqtt_link_context.dns_generation = g_mqtt_link_context.config_generation;

  error = dns_gethostbyname(g_mqtt_link_context.broker_host,
                            &address,
                            mqtt_link_dns_callback,
                            (void *)(uintptr_t)g_mqtt_link_context.dns_generation);
  if (error == ERR_OK)
  {
    mqtt_link_start_connect(&address);
  }
  else if (error == ERR_INPROGRESS)
  {
    g_mqtt_link_context.dns_pending = 1U;
  }
  else
  {
    g_mqtt_link_context.stats.connect_failures++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_DNS + (uint32_t)(-error);
    mqtt_link_note_connect_failure();
  }
}

static void mqtt_link_release_transport_keep_backoff(uint8_t disconnect_first)
{
  if (g_mqtt_link_context.client != NULL)
  {
    if (disconnect_first != 0U)
    {
      mqtt_disconnect(g_mqtt_link_context.client);
    }
    mqtt_client_free(g_mqtt_link_context.client);
    g_mqtt_link_context.client = NULL;
  }
  if (g_mqtt_link_context.tls_config != NULL)
  {
    altcp_tls_free_config(g_mqtt_link_context.tls_config);
    g_mqtt_link_context.tls_config = NULL;
  }

  memset(g_mqtt_link_context.psk, 0, sizeof(g_mqtt_link_context.psk));
  g_mqtt_link_context.psk_len = 0U;
  g_mqtt_link_context.connecting = 0U;
  g_mqtt_link_context.dns_pending = 0U;
  g_mqtt_link_context.initial_publish_stage = 0U;
  g_mqtt_link_context.stats.connected = 0U;
}

static void mqtt_link_reset_transport(void)
{
  taskENTER_CRITICAL();
  g_mqtt_link_context.stats.ack_queue_dropped +=
    g_mqtt_link_context.ack_count;
  taskEXIT_CRITICAL();

  if (g_mqtt_link_context.client != NULL)
  {
    mqtt_disconnect(g_mqtt_link_context.client);
    mqtt_client_free(g_mqtt_link_context.client);
    g_mqtt_link_context.client = NULL;
  }
  if (g_mqtt_link_context.tls_config != NULL)
  {
    altcp_tls_free_config(g_mqtt_link_context.tls_config);
    g_mqtt_link_context.tls_config = NULL;
  }

  memset(g_mqtt_link_context.psk, 0, sizeof(g_mqtt_link_context.psk));
  g_mqtt_link_context.psk_len = 0U;
  g_mqtt_link_context.connecting = 0U;
  g_mqtt_link_context.dns_pending = 0U;
  g_mqtt_link_context.initial_publish_stage = 0U;
  g_mqtt_link_context.ack_head = 0U;
  g_mqtt_link_context.ack_tail = 0U;
  g_mqtt_link_context.ack_count = 0U;
  g_mqtt_link_context.ack_in_flight = 0U;
  memset(g_mqtt_link_context.pending_queries, 0, sizeof(g_mqtt_link_context.pending_queries));
  g_mqtt_link_context.stats.ack_queue_depth = 0U;
  g_mqtt_link_context.stats.connected = 0U;
  g_mqtt_link_context.last_connect_attempt_ms = 0U;
  g_mqtt_link_context.connect_retry_ms = MQTT_LINK_CONNECT_RETRY_MIN_MS;
  g_mqtt_link_context.reset_requested = 0U;
}

static void mqtt_link_tcpip_step(void *argument)
{
  const uint32_t now = HAL_GetTick();

  (void)argument;
  g_mqtt_link_context.tcpip_step_pending = 0U;

  if ((g_mqtt_link_context.reset_requested != 0U) ||
      (g_mqtt_link_context.stats.should_run == 0U))
  {
    mqtt_link_reset_transport();
  }
  if (g_mqtt_link_context.stats.should_run == 0U)
  {
    return;
  }

  if ((g_mqtt_link_context.stats.connected == 0U) &&
      (g_mqtt_link_context.connecting == 0U) &&
      (g_mqtt_link_context.dns_pending == 0U) &&
      ((g_mqtt_link_context.last_connect_attempt_ms == 0U) ||
       ((int32_t)(now - g_mqtt_link_context.last_connect_attempt_ms) >=
        (int32_t)mqtt_link_current_retry_ms())))
  {
    mqtt_link_begin_connect();
    return;
  }

  if (g_mqtt_link_context.stats.connected == 0U)
  {
    return;
  }

  if (g_mqtt_link_context.ack_count > 0U)
  {
    mqtt_link_ack_try_publish();
    if (g_mqtt_link_context.ack_in_flight != 0U)
    {
      return;
    }
  }

  mqtt_link_pending_query_poll();

  if (g_mqtt_link_context.initial_publish_stage == 1U)
  {
    if (mqtt_link_publish_hello() == ERR_OK)
    {
      g_mqtt_link_context.initial_publish_stage = 2U;
    }
    return;
  }
  if (g_mqtt_link_context.initial_publish_stage == 2U)
  {
    if (mqtt_link_publish_status() == ERR_OK)
    {
      g_mqtt_link_context.initial_publish_stage = 3U;
    }
    return;
  }
  if (g_mqtt_link_context.initial_publish_stage == 3U)
  {
    if (mqtt_link_publish_fault() == ERR_OK)
    {
      g_mqtt_link_context.initial_publish_stage = 0U;
    }
    return;
  }

  if (g_mqtt_link_context.status_dirty != 0U)
  {
    (void)mqtt_link_publish_status();
    return;
  }
  if ((int32_t)(now - g_mqtt_link_context.last_heartbeat_ms) >=
      (int32_t)MQTT_LINK_HEARTBEAT_INTERVAL_MS)
  {
    mqtt_link_publish_heartbeat();
    return;
  }
  if ((int32_t)(now - g_mqtt_link_context.last_status_ms) >=
      (int32_t)MQTT_LINK_STATUS_INTERVAL_MS)
  {
    (void)mqtt_link_publish_status();
  }
}

void mqtt_link_service_mark_status_dirty(void)
{
  g_mqtt_link_context.status_dirty = 1U;
  g_mqtt_link_context.stats.status_dirty_count++;
}

void mqtt_link_service_complete_relay_config(const char *request_id,
                                             uint32_t target_node_id,
                                             uint8_t ok,
                                             const char *error_text)
{
  if (ok == 0U)
  {
    taskENTER_CRITICAL();
    g_mqtt_link_context.stats.command_errors++;
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_COMMAND + 10U;
    taskEXIT_CRITICAL();
  }

  mqtt_link_enqueue_ack(request_id,
                        ok,
                        1U,
                        1U,
                        error_text,
                        target_node_id);
  mqtt_link_service_mark_status_dirty();
}

void mqtt_link_service_init(void)
{
  if (g_mqtt_link_context.started != 0U)
  {
    return;
  }

  memset(&g_mqtt_link_context, 0, sizeof(g_mqtt_link_context));
  g_mqtt_link_context.started = 1U;
  g_mqtt_link_context.status_dirty = 1U;
}

void mqtt_link_service_poll(void)
{
  const device_config_t *config;
  uint8_t configured;
  uint8_t should_run;
  uint32_t now;
  err_t error;

  mqtt_link_service_init();
  device_config_t cfg_snap; device_config_copy(&cfg_snap); config = &cfg_snap;
  configured = mqtt_link_configured(config);
  should_run = mqtt_link_should_run(configured);
  mqtt_link_capture_config(config);
  mqtt_link_apply_pending_time();

  g_mqtt_link_context.stats.configured = configured;
  g_mqtt_link_context.stats.enabled = (config->cloud_enabled != 0U) ? 1U : 0U;
  g_mqtt_link_context.stats.transport = 1U;  /* MQTT is the only transport */
  g_mqtt_link_context.stats.broker_port = config->cloud_broker_port;
  g_mqtt_link_context.stats.should_run = should_run;

  if (configured == 0U)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_CONFIG;
  }
  else if (mqtt_link_network_ready() == 0U)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_NETWORK;
  }
  else if (should_run == 0U)
  {
    g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_NOT_MASTER;
  }

  now = HAL_GetTick();
  if ((g_mqtt_link_context.last_poll_ms != 0U) &&
      ((int32_t)(now - g_mqtt_link_context.last_poll_ms) <
       (int32_t)MQTT_LINK_TCPIP_STEP_INTERVAL_MS))
  {
    return;
  }
  g_mqtt_link_context.last_poll_ms = now;

  if (g_mqtt_link_context.tcpip_step_pending == 0U)
  {
    g_mqtt_link_context.tcpip_step_pending = 1U;
    error = tcpip_try_callback(mqtt_link_tcpip_step, NULL);
    if (error != ERR_OK)
    {
      g_mqtt_link_context.tcpip_step_pending = 0U;
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_CONNECT + (uint32_t)(-error);
    }
  }
}

void mqtt_link_service_get_stats(mqtt_link_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_mqtt_link_context.stats;
  }
}
