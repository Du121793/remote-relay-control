#ifndef CLUSTER_SERVICE_H
#define CLUSTER_SERVICE_H

#include <stdint.h>

#include "../Config/device_config.h"
#include "control_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLUSTER_SERVICE_NODE_CAPACITY         16U
#define CLUSTER_SERVICE_NODE_ID_LEN           16U
#define CLUSTER_SERVICE_DEVICE_NAME_LEN       DEVICE_CONFIG_NAME_MAX_LEN
#define CLUSTER_SERVICE_CLUSTER_NAME_LEN      DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN

typedef enum
{
  CLUSTER_ROLE_STANDALONE = 0,
  CLUSTER_ROLE_MEMBER = 1,
  CLUSTER_ROLE_MASTER = 2
} cluster_role_t;

typedef enum
{
  CLUSTER_JOIN_DISABLED = 0,
  CLUSTER_JOIN_MANUAL = 1,
  CLUSTER_JOIN_AUTO = 2
} cluster_join_mode_t;

typedef enum
{
  CLUSTER_DEVICE_STATE_STOPPED = 0,
  CLUSTER_DEVICE_STATE_RUNNING = 1,
  CLUSTER_DEVICE_STATE_FROZEN = 2
} cluster_device_state_t;

typedef struct
{
  uint8_t valid;
  uint8_t online;
  uint8_t auto_join;
  uint8_t role;
  uint8_t device_state;
  uint32_t node_id;
  uint32_t ip_addr;
  uint32_t last_seen_ms;
  uint32_t load_score;
  uint32_t term;
  uint32_t relay_mask;
  uint32_t enabled_mask;
  uint32_t scheduler_enabled_mask;
  uint32_t scheduler_pause_mask;
  uint32_t scheduler_pause_blocks_mask;
  uint32_t protection_enabled_mask;
  uint32_t protection_tripped_mask;
  uint32_t protection_fault_lock_mask;
  uint32_t protection_ct_valid_mask;
  uint32_t protection_learning_mask;
  uint32_t protection_strategy_lsb_mask;
  uint32_t protection_strategy_msb_mask;
  uint32_t auth_tag;
  uint32_t unix_time;
  uint8_t time_valid;
  uint8_t time_source;
  uint8_t time_quality;
  uint32_t time_seq;
  uint32_t gateway_epoch;
  uint32_t gateway_node_id;
  uint8_t adc_valid;
  uint8_t adc_ct_valid;
  uint8_t adc_signal_present;
  uint32_t adc_raw;
  uint32_t adc_millivolts;
  uint32_t adc_min;
  uint32_t adc_max;
  uint32_t adc_average;
  uint32_t adc_raw_rms;
  uint32_t adc_rms_millivolts;
  uint32_t adc_current_milliamps;
  uint32_t adc_sample_rate_hz;
  uint32_t adc_window_samples;
  uint32_t adc_sample_count;
  uint32_t adc_dma_overruns;
  uint32_t adc_saturation_count;
  char node_name[CLUSTER_SERVICE_DEVICE_NAME_LEN];
  char cluster_name[CLUSTER_SERVICE_CLUSTER_NAME_LEN];
  char cloud_cluster_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_device_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_key_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  uint8_t mac[6];
} cluster_node_t;

typedef struct
{
  uint8_t cluster_enabled;
  uint8_t join_mode;
  uint8_t role;
  uint8_t has_master;
  uint8_t master_eligible;
  uint8_t time_valid;
  uint8_t rtc_lse_ready;
  uint8_t time_source;
  uint8_t time_quality;
  uint32_t self_node_id;
  uint32_t master_node_id;
  uint32_t term;
  uint32_t node_count;
  uint32_t health_score;
  uint32_t health_lateness_ms;
  uint32_t health_bad_streak;
  uint32_t unix_time;
  uint32_t time_sync_count;
  uint32_t time_seq;
  uint32_t gateway_epoch;
  uint32_t gateway_node_id;
  int32_t last_time_offset_seconds;
  char cluster_name[CLUSTER_SERVICE_CLUSTER_NAME_LEN];
} cluster_service_state_t;

#define CLUSTER_TIME_SOURCE_NONE       0U
#define CLUSTER_TIME_SOURCE_MQTT       1U
#define CLUSTER_TIME_SOURCE_MASTER     2U
#define CLUSTER_TIME_SOURCE_RTC        3U
#define CLUSTER_TIME_SOURCE_NTP        4U
#define CLUSTER_TIME_SOURCE_BACKEND    5U

#define CLUSTER_TIME_QUALITY_INVALID       0U
#define CLUSTER_TIME_QUALITY_CLUSTER       1U
#define CLUSTER_TIME_QUALITY_RTC_HOLDOVER  2U
#define CLUSTER_TIME_QUALITY_BROKER        3U

void cluster_service_init(void);
void cluster_service_poll(void);
void cluster_service_reload_from_config(void);
void cluster_service_reset(void);
void cluster_service_get_state(cluster_service_state_t *state);
const cluster_node_t *cluster_service_get_nodes(uint32_t *count);
const cluster_node_t *cluster_service_find_node(uint32_t node_id);
uint8_t cluster_service_remove_node(uint32_t node_id);
uint8_t cluster_service_kick_node(uint32_t node_id);
uint8_t cluster_service_apply_remote_command(uint32_t node_id, const control_proto_command_t *command);
uint32_t cluster_service_compute_load_score(uint32_t http_active_connections,
                                            uint32_t control_connections,
                                            uint32_t pending_commands);
void cluster_service_set_local_load_score(uint32_t load_score);
void cluster_service_note_remote_node(const cluster_node_t *node);
uint8_t cluster_service_set_unix_time(uint32_t unix_time, uint8_t source);
uint32_t cluster_service_get_unix_time(uint8_t *valid);

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_SERVICE_H */
