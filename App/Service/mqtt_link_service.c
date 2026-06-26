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

#define MQTT_LINK_CONNECT_RETRY_MS       5000UL
#define MQTT_LINK_HEARTBEAT_INTERVAL_MS  5000UL
#define MQTT_LINK_STATUS_INTERVAL_MS     3000UL
#define MQTT_LINK_TCPIP_STEP_INTERVAL_MS 200UL
#define MQTT_LINK_MASTER_STABLE_MS       5000UL
#define MQTT_LINK_KEEP_ALIVE_SECONDS     20U
#define MQTT_LINK_PSK_MAX_LEN            32U
#define MQTT_LINK_PAYLOAD_MAX_LEN        512U
#define MQTT_LINK_STATUS_PAYLOAD_MAX_LEN 1024U
#define MQTT_LINK_COMMAND_MAX_LEN        384U
#define MQTT_LINK_REQUEST_ID_MAX_LEN     64U
#define MQTT_LINK_ACK_TOPIC_MAX_LEN      192U
#define MQTT_LINK_ACK_ERROR_MAX_LEN      32U
#define MQTT_LINK_ACK_QUEUE_LENGTH       32U

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
  MQTT_LINK_PUBLISH_FAULT
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
  uint8_t ack_head;
  uint8_t ack_tail;
  uint8_t ack_count;
  uint8_t ack_in_flight;
  uint8_t psk[MQTT_LINK_PSK_MAX_LEN];
  size_t psk_len;
} mqtt_link_context_t;

static mqtt_link_context_t g_mqtt_link_context;

extern struct netif gnetif;

static void mqtt_link_tcpip_step(void *argument);
static void mqtt_link_connection_callback(mqtt_client_t *client,
                                          void *argument,
                                          mqtt_connection_status_t status);
static void mqtt_link_ack_try_publish(void);
static void mqtt_link_apply_pending_time(void);

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
  (void)snprintf(stats->topic_command_protection_force_stop,
                 sizeof(stats->topic_command_protection_force_stop),
                 "cluster/%s/command/protection/force-stop", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_protection_unlock,
                 sizeof(stats->topic_command_protection_unlock),
                 "cluster/%s/command/protection/unlock", config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_protection_clear_learning,
                 sizeof(stats->topic_command_protection_clear_learning),
                 "cluster/%s/command/protection/clear-learning", config->cloud_cluster_id);
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

  cluster_service_get_state(&cluster_state);
  adc_service_get_state(&adc_state);
  written = snprintf(payload, sizeof(payload),
                     "{\"type\":\"status\",\"online\":1,\"cluster_id\":\"%s\","
                     "\"device_id\":\"%s\",\"node_id\":%lu,\"master_node_id\":%lu,"
                     "\"role\":%u,\"term\":%lu,\"relay_mask\":%lu,"
                     "\"uptime_ms\":%lu,\"unix_time\":%lu,\"time_valid\":%u,"
                     "\"time_source\":%u,\"time_quality\":%u,\"time_seq\":%lu,"
                     "\"gateway_epoch\":%lu,\"gateway_node_id\":%lu,"
                     "\"adc_valid\":%u,\"adc_ct_valid\":%u,\"adc_signal_present\":%u,"
                     "\"adc_raw\":%u,\"adc_millivolts\":%u,\"adc_min\":%u,"
                     "\"adc_max\":%u,\"adc_average\":%u,\"adc_raw_rms\":%u,"
                     "\"adc_rms_millivolts\":%u,\"adc_current_milliamps\":%lu,"
                     "\"adc_sample_rate_hz\":%lu,\"adc_window_samples\":%lu,"
                     "\"adc_sample_count\":%lu,\"adc_dma_overruns\":%lu,"
                     "\"adc_saturation_count\":%lu}",
                     g_mqtt_link_context.cloud_cluster_id,
                     g_mqtt_link_context.cloud_device_id,
                     (unsigned long)cluster_state.self_node_id,
                     (unsigned long)cluster_state.master_node_id,
                     (unsigned int)cluster_state.role,
                     (unsigned long)cluster_state.term,
                     (unsigned long)board_relay_snapshot(),
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
static const char *g_mqtt_sub_topics[12];
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
    if (sch_weekday > 6 || sch_count > SCHEDULER_MAX_INTERVALS) {
      mqtt_link_publish_ack(request_id, 0U, "bad_scheduler_set", target_node_id);
      return;
    }
    weekly_schedule_t *ws = scheduler_get_weekly((uint8_t)sch_ch);
    if (!ws) {
      mqtt_link_publish_ack(request_id, 0U, "not_found", target_node_id);
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
    scheduler_set_weekly_day((uint8_t)sch_ch, (uint8_t)sch_weekday, &new_day);
      scheduler_mark_flash_dirty(); scheduler_mark_rtc_dirty();
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_scheduler_enabled) == 0)
  {
    uint32_t sch_en_ch = 0, sch_enabled = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &sch_en_ch);
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "enabled", &sch_enabled);
    channel_scheduler_state_t *st = scheduler_get_state((uint8_t)sch_en_ch);
    scheduler_set_enabled((uint8_t)sch_en_ch, (uint8_t)(sch_enabled ? 1 : 0));
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
      scheduler_mark_flash_dirty(); scheduler_mark_rtc_dirty();
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
    channel_scheduler_state_t *st = scheduler_get_state((uint8_t)sp_ch);
    if (st) {
      uint8_t tv; uint32_t now_unix = cluster_service_get_unix_time(&tv);
      switch (sp_mode) {
      uint32_t sp_pause_until;
      switch (sp_mode) {
        case 0: sp_pause_until = now_unix + 60; break;
        case 1: sp_pause_until = now_unix + sp_dur; break;
        case 2: sp_pause_until = sp_until; break;
        case 3: sp_pause_until = SCHEDULER_PAUSE_FOREVER; break;
        default: sp_pause_until = now_unix + 60; break;
      }
      scheduler_set_pause((uint8_t)sp_ch, sp_pause_until, (uint8_t)(sp_blocks ? 1 : 0));
      mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
      return;
      scheduler_mark_rtc_dirty();
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_force_stop) == 0)
  {
    uint32_t prot_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    if (prot_ch >= 1 && prot_ch <= SCHEDULER_CHANNEL_COUNT) {
      relay_protection_force_stop((uint8_t)prot_ch);
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_unlock) == 0)
  {
    uint32_t prot_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    if (prot_ch >= 1 && prot_ch <= SCHEDULER_CHANNEL_COUNT) {
      relay_protection_unlock((uint8_t)prot_ch);
    }
    mqtt_link_publish_ack(request_id, 1U, "", target_node_id);
    return;
  }
  else if (strcmp(g_mqtt_link_context.rx_topic,
                   g_mqtt_link_context.stats.topic_command_protection_clear_learning) == 0)
  {
    uint32_t prot_ch = 0;
    (void)mqtt_link_extract_json_u32(g_mqtt_link_context.rx_payload, "channel", &prot_ch);
    if (prot_ch >= 1 && prot_ch <= SCHEDULER_CHANNEL_COUNT) {
      relay_protection_clear_learning((uint8_t)prot_ch);
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
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_force_stop) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_unlock) == 0) ||
	      (strcmp(topic, g_mqtt_link_context.stats.topic_command_protection_clear_learning) == 0))) ? 1U : 0U;

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
  g_mqtt_sub_topics[3]  = g_mqtt_link_context.stats.topic_command_time;
  g_mqtt_sub_topics[4]  = g_mqtt_link_context.stats.topic_command_auto_hold;
  g_mqtt_sub_topics[5]  = g_mqtt_link_context.stats.topic_command_control;
  g_mqtt_sub_topics[6]  = g_mqtt_link_context.stats.topic_command_scheduler_set;
  g_mqtt_sub_topics[7]  = g_mqtt_link_context.stats.topic_command_scheduler_enabled;
  g_mqtt_sub_topics[8]  = g_mqtt_link_context.stats.topic_command_scheduler_pause;
  g_mqtt_sub_topics[9]  = g_mqtt_link_context.stats.topic_command_protection_force_stop;
  g_mqtt_sub_topics[10] = g_mqtt_link_context.stats.topic_command_protection_unlock;
  g_mqtt_sub_topics[11] = g_mqtt_link_context.stats.topic_command_protection_clear_learning;
  g_mqtt_sub_count = 12U;
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
    mqtt_link_log_value("[mqtt] disconnected status=", (uint32_t)status);
    /* TLS/PSK failure may mean bindingSecret changed — re-provision now */
    https_client_service_trigger();
    return;
  }

  g_mqtt_link_context.stats.connected = 1U;
  g_mqtt_link_context.stats.last_error = 0U;
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
      g_mqtt_link_context.stats.last_error = MQTT_LINK_ERROR_TLS_CONFIG;
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
    mqtt_link_log_value("[mqtt] connect start fail=", (uint32_t)(-error));
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
  }
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
  g_mqtt_link_context.stats.ack_queue_depth = 0U;
  g_mqtt_link_context.stats.connected = 0U;
  g_mqtt_link_context.last_connect_attempt_ms = 0U;
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
        (int32_t)MQTT_LINK_CONNECT_RETRY_MS)))
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
