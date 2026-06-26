#ifndef MQTT_LINK_SERVICE_H
#define MQTT_LINK_SERVICE_H

#include <stdint.h>
#include "../Config/device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_LINK_CLIENT_ID_MAX_LEN 96U
#define MQTT_LINK_TOPIC_MAX_LEN 128U

typedef struct
{
  uint8_t configured;
  uint8_t enabled;
  uint8_t should_run;
  uint8_t connected;
  uint8_t transport;
  uint16_t broker_port;
  uint32_t connect_attempts;
  uint32_t connect_failures;
  uint32_t published_hello;
  uint32_t published_heartbeat;
  uint32_t published_status;
  uint32_t published_fault_reports;
  uint32_t received_commands;
  uint32_t command_errors;
  uint32_t published_acks;
  uint32_t ack_queue_dropped;
  uint32_t ack_queue_depth;
  uint32_t last_error;
  uint32_t status_dirty_count;
  char client_id[MQTT_LINK_CLIENT_ID_MAX_LEN];
  char topic_status[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_events[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_measure[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_relay[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_mask[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_config_relay[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_time[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_auto_hold[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_control[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_scheduler_set[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_scheduler_enabled[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_scheduler_pause[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_protection_force_stop[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_protection_unlock[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_command_protection_clear_learning[MQTT_LINK_TOPIC_MAX_LEN];
  char topic_ack_prefix[MQTT_LINK_TOPIC_MAX_LEN];
  char cluster_ca_fingerprint[DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN];
  char cluster_binding_secret[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN];
} mqtt_link_stats_t;

void mqtt_link_service_init(void);
void mqtt_link_service_poll(void);
void mqtt_link_service_get_stats(mqtt_link_stats_t *stats);
void mqtt_link_service_mark_status_dirty(void);
void mqtt_link_service_complete_relay_config(const char *request_id,
                                             uint32_t target_node_id,
                                             uint8_t ok,
                                             const char *error_text);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LINK_SERVICE_H */
