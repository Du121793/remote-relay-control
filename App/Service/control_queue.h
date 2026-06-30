#ifndef CONTROL_QUEUE_H
#define CONTROL_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "main.h"
#include "control_proto.h"
#include "relay_domain.h"
#include "../Config/device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t submitted;
  uint32_t executed;
  uint32_t dropped;
  uint32_t queue_full;
  uint32_t busy_rejected;
  uint32_t current_depth;
  uint32_t high_watermark;
  uint32_t capacity;
} control_queue_stats_t;

#define CONTROL_QUEUE_RELAY_CONFIG_NAME_PRESENT       0x01U
#define CONTROL_QUEUE_RELAY_CONFIG_ENABLED_PRESENT    0x02U
#define CONTROL_QUEUE_RELAY_CONFIG_REMEMBER_PRESENT   0x04U
#define CONTROL_QUEUE_RELAY_CONFIG_BOOT_PRESENT       0x08U
#define CONTROL_QUEUE_RELAY_CONFIG_ADC_PRESENT        0x10U
#define CONTROL_QUEUE_RELAY_CONFIG_PROTECTION_PRESENT 0x20U
#define CONTROL_QUEUE_RELAY_CONFIG_THRESHOLD_PRESENT  0x40U
#define CONTROL_QUEUE_RELAY_CONFIG_DELAY_PRESENT      0x80U
#define CONTROL_QUEUE_RELAY_CONFIG_HYSTERESIS_PRESENT 0x100U
#define CONTROL_QUEUE_RELAY_CONFIG_STRATEGY_PRESENT   0x200U
#define CONTROL_QUEUE_REQUEST_ID_MAX_LEN               64U
#define CONTROL_QUEUE_REMOTE_HTTP_METHOD_MAX_LEN        8U
#define CONTROL_QUEUE_REMOTE_HTTP_PATH_MAX_LEN          160U
#define CONTROL_QUEUE_REMOTE_HTTP_BODY_MAX_LEN          1152U

typedef struct
{
  uint8_t channel;
  uint8_t enabled;
  uint8_t remember_state;
  uint8_t boot_default_on;
  uint8_t adc_channel;
  uint8_t protection_enabled;
  uint8_t device_strategy;
  uint16_t fields;
  uint32_t overcurrent_milliamps;
  uint32_t trip_delay_ms;
  uint32_t reset_hysteresis_milliamps;
  char name[DEVICE_CONFIG_NAME_MAX_LEN];
} control_queue_relay_config_t;

void control_queue_init(void);
HAL_StatusTypeDef control_queue_submit(const control_proto_command_t *command, relay_domain_result_t *result);
HAL_StatusTypeDef control_queue_submit_async(const control_proto_command_t *command);
HAL_StatusTypeDef control_queue_submit_remote(uint32_t ip_addr,
                                              const control_proto_command_t *command,
                                              relay_domain_result_t *result);
HAL_StatusTypeDef control_queue_submit_remote_async(uint32_t ip_addr,
                                                    uint32_t node_id,
                                                    const control_proto_command_t *command);
HAL_StatusTypeDef control_queue_submit_backend_sync_async(uint32_t ip_addr,
                                                          const device_config_t *config);
HAL_StatusTypeDef control_queue_submit_relay_config_async(
  uint32_t ip_addr,
  uint32_t node_id,
  const control_queue_relay_config_t *relay_config,
  const char *request_id);
HAL_StatusTypeDef control_queue_submit_remote_http_async(
  uint32_t ip_addr,
  const char *method,
  const char *path,
  const char *body,
  uint32_t *request_id);
HAL_StatusTypeDef control_queue_get_remote_http_result(
  uint32_t request_id,
  uint8_t *done,
  uint8_t *remote_ok,
  char *response_body,
  size_t response_body_size);
void control_queue_poll(void);
void control_queue_get_stats(control_queue_stats_t *stats);
void control_queue_force_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
unsigned long control_queue_stack_high_water(void);

#endif /* CONTROL_QUEUE_H */
