#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CONFIG_RELAY_COUNT            16U
#define DEVICE_CONFIG_NAME_MAX_LEN           32U
#define DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN   32U
#define DEVICE_CONFIG_PASSWORD_HASH_LEN      32U
#define DEVICE_CONFIG_BACKEND_HOST_MAX_LEN   64U
#define DEVICE_CONFIG_BACKEND_KEY_MAX_LEN    64U
#define DEVICE_CONFIG_CLOUD_ID_MAX_LEN       64U
#define DEVICE_CONFIG_CLOUD_HOST_MAX_LEN     64U
#define DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN 96U
#define DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN   128U
#define DEVICE_CONFIG_CA_CERT_MAX_LEN        2048U
#define DEVICE_CONFIG_MAGIC                  0x43464731UL
#define DEVICE_CONFIG_VERSION                6U
#define DEVICE_CONFIG_ADC_CHANNEL_UNASSIGNED 255U
#define DEVICE_CONFIG_OVERCURRENT_DEFAULT_MA 4500U
#define DEVICE_CONFIG_OVERCURRENT_DELAY_MS   1000U
#define DEVICE_CONFIG_OVERCURRENT_HYST_MA    500U

typedef struct
{
  uint8_t enabled;
  uint8_t remember_state;
  uint8_t boot_default_on;
  uint8_t reserved;
  char name[DEVICE_CONFIG_NAME_MAX_LEN];
} device_config_relay_t;

typedef struct
{
  uint8_t adc_channel;
  uint8_t protection_enabled;
  uint16_t reserved;
  uint32_t overcurrent_milliamps;
  uint32_t trip_delay_ms;
  uint32_t reset_hysteresis_milliamps;
} device_config_current_t;

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t config_crc32;
  char device_name[DEVICE_CONFIG_NAME_MAX_LEN];
  uint8_t auto_join_cluster;
  uint8_t cluster_enabled;
  uint8_t reserved0[2];
  char cluster_name[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN];
  uint8_t cluster_password_hash[DEVICE_CONFIG_PASSWORD_HASH_LEN];
  char backend_host[DEVICE_CONFIG_BACKEND_HOST_MAX_LEN];
  uint16_t backend_port;
  uint16_t reserved1;
  char backend_key[DEVICE_CONFIG_BACKEND_KEY_MAX_LEN];
  uint8_t cloud_enabled;
  uint8_t cloud_transport;
  uint8_t reserved2[2];
  char cloud_broker_host[DEVICE_CONFIG_CLOUD_HOST_MAX_LEN];
  uint16_t cloud_broker_port;
  uint16_t reserved3;
  char cloud_cluster_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_device_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_key_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_cluster_ca_fingerprint[DEVICE_CONFIG_CLOUD_FINGERPRINT_MAX_LEN];
  char cloud_cluster_binding_secret[DEVICE_CONFIG_CLOUD_SECRET_MAX_LEN];
  char cloud_cluster_ca_cert[DEVICE_CONFIG_CA_CERT_MAX_LEN];
  uint32_t saved_relay_mask;
  device_config_relay_t relay[DEVICE_CONFIG_RELAY_COUNT];
  device_config_current_t current[DEVICE_CONFIG_RELAY_COUNT];
} device_config_t;

void device_config_build_defaults(device_config_t *config);
void device_config_init(void);
const device_config_t *device_config_get(void);
void device_config_set(const device_config_t *config);
uint8_t device_config_update_and_save(const device_config_t *config);
uint32_t device_config_compute_crc32(const device_config_t *config);
uint8_t device_config_is_valid(const device_config_t *config);
uint8_t device_config_load(device_config_t *config);
uint8_t device_config_save(const device_config_t *config);
void device_config_reset(void);
void device_config_hash_password_text(const char *text, uint8_t hash_out[DEVICE_CONFIG_PASSWORD_HASH_LEN]);
uint32_t device_config_password_tag(const device_config_t *config);
uint32_t device_config_last_flash_error(void);
uint32_t device_config_build_startup_relay_mask(const device_config_t *config);
uint8_t device_config_persist_runtime_relay_mask(uint32_t relay_mask);
void device_config_runtime_relay_mask_set(uint32_t relay_mask);
void device_config_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_CONFIG_H */
