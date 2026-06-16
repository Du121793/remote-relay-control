#include "device_config.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "stm32f7xx_hal.h"
#include "stm32f7xx_hal_flash.h"
#include "stm32f7xx_hal_flash_ex.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "mbedtls/sha256.h"
#include "mbedtls/md.h"

static device_config_t g_device_config __attribute__((aligned(8)));
static uint32_t g_device_config_last_flash_error = 0U;
static QueueHandle_t g_device_config_queue = NULL;
static TaskHandle_t g_device_config_task_handle = NULL;
static uint8_t g_device_config_initialized = 0U;
static uint8_t g_device_config_runtime_dirty = 0U;
static uint32_t g_device_config_runtime_due_ms = 0U;

#define DEVICE_CONFIG_FLASH_ADDRESS   0x081E0000UL
#define DEVICE_CONFIG_QUEUE_LENGTH    2U
#define DEVICE_CONFIG_WAIT_MS         2000U
#define DEVICE_CONFIG_RUNTIME_SAVE_DELAY_MS  1500U

typedef struct
{
  device_config_t config;
  uint8_t *result;
  TaskHandle_t waiting_task;
} device_config_request_t;

static StaticQueue_t g_device_config_queue_buffer;
static uint8_t g_device_config_queue_storage[
  DEVICE_CONFIG_QUEUE_LENGTH * sizeof(device_config_request_t)];
static StaticTask_t g_device_config_task_buffer;
static StackType_t g_device_config_task_stack[2048];
static StaticSemaphore_t g_device_config_save_mutex_buffer;
static SemaphoreHandle_t g_device_config_save_mutex = NULL;
static uint8_t g_device_config_sync_result = 0U;

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
  device_config_relay_t relay[DEVICE_CONFIG_RELAY_COUNT];
} device_config_v1_t;

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
  uint32_t saved_relay_mask;
  device_config_relay_t relay[DEVICE_CONFIG_RELAY_COUNT];
} device_config_v2_t;

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
  uint32_t saved_relay_mask;
  device_config_relay_t relay[DEVICE_CONFIG_RELAY_COUNT];
} device_config_v3_t;

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
  uint32_t saved_relay_mask;
  device_config_relay_t relay[DEVICE_CONFIG_RELAY_COUNT];
} device_config_v4_t;

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
  uint32_t saved_relay_mask;
  device_config_relay_t relay[DEVICE_CONFIG_RELAY_COUNT];
} device_config_v5_t;

static uint8_t device_config_save_direct(const device_config_t *config);
static void device_config_worker_task(void *argument);
static void device_config_worker_init(void);
static void device_config_copy_locked(device_config_t *dst);
static uint8_t device_config_normalize_runtime_defaults(device_config_t *config);
static uint8_t device_config_v2_is_valid(const device_config_v2_t *config);
static uint8_t device_config_v3_is_valid(const device_config_v3_t *config);
static uint8_t device_config_v4_is_valid(const device_config_v4_t *config);
static uint8_t device_config_v5_is_valid(const device_config_v5_t *config);
static void device_config_build_current_defaults(device_config_t *config);

static uint32_t device_config_flash_sector(void)
{
#if defined(FLASH_OPTCR_nDBANK)
  if ((FLASH->OPTCR & FLASH_OPTCR_nDBANK) != 0U)
  {
    return FLASH_SECTOR_11;
  }
  return FLASH_SECTOR_23;
#else
  return FLASH_SECTOR_11;
#endif
}

static uint32_t device_config_flash_bank(void)
{
#if defined(FLASH_OPTCR_nDBANK)
  if ((FLASH->OPTCR & FLASH_OPTCR_nDBANK) != 0U)
  {
    return FLASH_BANK_1;
  }
  return FLASH_BANK_2;
#else
  return FLASH_BANK_1;
#endif
}

static uint8_t device_config_flash_erase(void)
{
  FLASH_EraseInitTypeDef erase_init;
  uint32_t sector_error = 0U;

  memset(&erase_init, 0, sizeof(erase_init));
  erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase_init.Banks = device_config_flash_bank();
  erase_init.Sector = device_config_flash_sector();
  erase_init.NbSectors = 1U;

  if (HAL_FLASHEx_Erase(&erase_init, &sector_error) != HAL_OK)
  {
    g_device_config_last_flash_error = HAL_FLASH_GetError();
    return 0U;
  }

  g_device_config_last_flash_error = 0U;
  return 1U;
}

static uint32_t device_config_crc32_bytes(const uint8_t *data, uint32_t length)
{
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t i;
  uint32_t bit;

  for (i = 0U; i < length; ++i)
  {
    crc ^= data[i];
    for (bit = 0U; bit < 8U; ++bit)
    {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }

  return ~crc;
}

static uint32_t device_config_crc32_zeroed_range(const uint8_t *data,
                                                 uint32_t length,
                                                 uint32_t zero_offset,
                                                 uint32_t zero_length)
{
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t i;
  uint32_t bit;

  for (i = 0U; i < length; ++i)
  {
    uint8_t value = data[i];

    if ((i >= zero_offset) && (i < (zero_offset + zero_length)))
    {
      value = 0U;
    }

    crc ^= value;
    for (bit = 0U; bit < 8U; ++bit)
    {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }

  return ~crc;
}

static void device_config_copy_locked(device_config_t *dst)
{
  taskENTER_CRITICAL();
  *dst = g_device_config;
  taskEXIT_CRITICAL();
}

static uint8_t device_config_v1_is_valid(const device_config_v1_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->magic != DEVICE_CONFIG_MAGIC)
  {
    return 0U;
  }

  if ((config->version != 1U) || (config->size != sizeof(*config)))
  {
    return 0U;
  }

  return (config->config_crc32 ==
          device_config_crc32_zeroed_range((const uint8_t *)config,
                                           (uint32_t)sizeof(*config),
                                           (uint32_t)offsetof(device_config_v1_t, config_crc32),
                                           (uint32_t)sizeof(config->config_crc32))) ? 1U : 0U;
}

static uint8_t device_config_v2_is_valid(const device_config_v2_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->magic != DEVICE_CONFIG_MAGIC)
  {
    return 0U;
  }

  if ((config->version != 2U) || (config->size != sizeof(*config)))
  {
    return 0U;
  }

  return (config->config_crc32 ==
          device_config_crc32_zeroed_range((const uint8_t *)config,
                                           (uint32_t)sizeof(*config),
                                           (uint32_t)offsetof(device_config_v2_t, config_crc32),
                                           (uint32_t)sizeof(config->config_crc32))) ? 1U : 0U;
}

static uint8_t device_config_v3_is_valid(const device_config_v3_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->magic != DEVICE_CONFIG_MAGIC)
  {
    return 0U;
  }

  if ((config->version != 3U) || (config->size != sizeof(*config)))
  {
    return 0U;
  }

  return (config->config_crc32 ==
          device_config_crc32_zeroed_range((const uint8_t *)config,
                                           (uint32_t)sizeof(*config),
                                           (uint32_t)offsetof(device_config_v3_t, config_crc32),
                                           (uint32_t)sizeof(config->config_crc32))) ? 1U : 0U;
}

static uint8_t device_config_v4_is_valid(const device_config_v4_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->magic != DEVICE_CONFIG_MAGIC)
  {
    return 0U;
  }

  if ((config->version != 4U) || (config->size != sizeof(*config)))
  {
    return 0U;
  }

  return (config->config_crc32 ==
          device_config_crc32_zeroed_range((const uint8_t *)config,
                                           (uint32_t)sizeof(*config),
                                           (uint32_t)offsetof(device_config_v4_t, config_crc32),
                                           (uint32_t)sizeof(config->config_crc32))) ? 1U : 0U;
}

static uint8_t device_config_v5_is_valid(const device_config_v5_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->magic != DEVICE_CONFIG_MAGIC)
  {
    return 0U;
  }

  if ((config->version != 5U) || (config->size != sizeof(*config)))
  {
    return 0U;
  }

  return (config->config_crc32 ==
          device_config_crc32_zeroed_range((const uint8_t *)config,
                                           (uint32_t)sizeof(*config),
                                           (uint32_t)offsetof(device_config_v5_t, config_crc32),
                                           (uint32_t)sizeof(config->config_crc32))) ? 1U : 0U;
}

static void device_config_build_current_defaults(device_config_t *config)
{
  uint32_t index;

  if (config == NULL)
  {
    return;
  }

  for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
  {
    config->current[index].adc_channel =
      DEVICE_CONFIG_ADC_CHANNEL_UNASSIGNED;
    config->current[index].protection_enabled = 0U;
    config->current[index].reserved = 0U;
    config->current[index].overcurrent_milliamps =
      DEVICE_CONFIG_OVERCURRENT_DEFAULT_MA;
    config->current[index].trip_delay_ms =
      DEVICE_CONFIG_OVERCURRENT_DELAY_MS;
    config->current[index].reset_hysteresis_milliamps =
      DEVICE_CONFIG_OVERCURRENT_HYST_MA;
  }
}

void device_config_hash_password_text(const char *text, uint8_t hash_out[DEVICE_CONFIG_PASSWORD_HASH_LEN])
{
  if (hash_out == NULL)
  {
    return;
  }

  if ((text == NULL) || (text[0] == '\0'))
  {
    memset(hash_out, 0, DEVICE_CONFIG_PASSWORD_HASH_LEN);
    return;
  }

  /* Use mbedTLS SHA-256 instead of CRC32 so the stored password hash
     is a proper cryptographic digest.  An attacker cannot reverse or
     collide SHA-256 the way they could with a 32-bit CRC. */
  {
    int ret = mbedtls_sha256_ret((const unsigned char *)text,
                                 strlen(text),
                                 hash_out,
                                 0 /* 0 = SHA-256, 1 = SHA-224 */);
    if (ret != 0)
    {
      memset(hash_out, 0, DEVICE_CONFIG_PASSWORD_HASH_LEN);
    }
  }
}

uint32_t device_config_password_tag(const device_config_t *config)
{
  uint32_t hash = 2166136261UL;
  uint32_t index;
  uint8_t non_zero = 0U;

  if (config == NULL)
  {
    return 0U;
  }

  for (index = 0U; index < DEVICE_CONFIG_PASSWORD_HASH_LEN; ++index)
  {
    uint8_t value = config->cluster_password_hash[index];
    if (value != 0U)
    {
      non_zero = 1U;
    }
    hash ^= value;
    hash *= 16777619UL;
  }

  return (non_zero != 0U) ? hash : 0U;
}

uint32_t device_config_compute_auth_tag(
  const uint8_t password_hash[DEVICE_CONFIG_PASSWORD_HASH_LEN],
  uint32_t unix_time,
  uint32_t node_id,
  const char *cluster_name)
{
  uint8_t has_password = 0U;
  uint32_t index;

  if ((password_hash == NULL) || (cluster_name == NULL))
  {
    return 0U;
  }

  for (index = 0U; index < DEVICE_CONFIG_PASSWORD_HASH_LEN; ++index)
  {
    if (password_hash[index] != 0U)
    {
      has_password = 1U;
      break;
    }
  }

  if (has_password != 0U)
  {
    uint8_t hmac_out[32];
    char input_buf[128];
    int len = snprintf(input_buf, sizeof(input_buf),
                       "%lu|%lu|%s",
                       (unsigned long)unix_time,
                       (unsigned long)node_id,
                       cluster_name);
    if ((len > 0) && ((size_t)len < sizeof(input_buf)) &&
        (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                         password_hash,
                         DEVICE_CONFIG_PASSWORD_HASH_LEN,
                         (const unsigned char *)input_buf, (size_t)len,
                         hmac_out) == 0))
    {
      uint32_t tag;
      memcpy(&tag, hmac_out, sizeof(tag));
      return tag;
    }
    return 0U;
  }

  /* No password → deterministic FNV1a of the zero hash. */
  {
    uint32_t hash = 2166136261UL;
    for (index = 0U; index < DEVICE_CONFIG_PASSWORD_HASH_LEN; ++index)
    {
      hash ^= password_hash[index];
      hash *= 16777619UL;
    }
    return hash;
  }
}

void device_config_build_defaults(device_config_t *config)
{
  uint32_t i;

  if (config == NULL)
  {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->magic = DEVICE_CONFIG_MAGIC;
  config->version = DEVICE_CONFIG_VERSION;
  config->size = (uint16_t)sizeof(*config);
  memcpy(config->device_name, "autoControl", sizeof("autoControl"));
  config->auto_join_cluster = 1U;
  config->cluster_enabled = 1U;
  memcpy(config->cluster_name, "default", sizeof("default"));
  memcpy(config->backend_host, "192.168.0.139", sizeof("192.168.0.139"));
  config->backend_port = 42001U;
  memcpy(config->backend_key, "dev-key", sizeof("dev-key"));
  config->cloud_enabled = 0U;
  config->cloud_transport = 1U;
  memcpy(config->cloud_broker_host, "broker.example.com", sizeof("broker.example.com"));
  config->cloud_broker_port = 8883U;
  /* Auto-generate cloud identity from MCU UID so every device has a
     unique, non-editable cloud_device_id.  The UID is 96 bits. */
  {
    const uint32_t uid0 = *(const uint32_t *)0x1FF0F420UL;
    const uint32_t uid1 = *(const uint32_t *)0x1FF0F424UL;
    const uint32_t uid2 = *(const uint32_t *)0x1FF0F428UL;
    (void)snprintf(config->cloud_device_id, sizeof(config->cloud_device_id),
                   "node-%lu-%lu",
                   (unsigned long)(((uid0 ^ uid2) >> 3U) & 0x3FFFFFFFUL),
                   (unsigned long)((uid1 & 0x0FFFFFFFUL)));
    (void)snprintf(config->cloud_key_id, sizeof(config->cloud_key_id),
                   "key-%08lx", (unsigned long)((uid0 ^ uid1 ^ uid2) & 0xFFFFFFFFUL));
  }
  /* cloud_cluster_id follows cluster_name — defaults to "default" cluster */
  memcpy(config->cloud_cluster_id, "default", sizeof("default"));
  config->cloud_cluster_ca_fingerprint[0] = '\0';
  config->cloud_cluster_binding_secret[0] = '\0';
  config->cloud_cluster_ca_cert[0] = '\0';
  config->saved_relay_mask = 0U;

  for (i = 0U; i < DEVICE_CONFIG_RELAY_COUNT; ++i)
  {
    device_config_relay_t *relay = &config->relay[i];
    int name_len;

    relay->enabled = 1U;
    relay->remember_state = 1U;
    relay->boot_default_on = 0U;
    name_len = snprintf(relay->name, sizeof(relay->name), "Relay%lu", (unsigned long)(i + 1U));
    if ((name_len < 0) || ((size_t)name_len >= sizeof(relay->name)))
    {
      memcpy(relay->name, "Relay", sizeof("Relay"));
    }
  }
  device_config_build_current_defaults(config);

  config->config_crc32 = device_config_compute_crc32(config);
}

void device_config_init(void)
{
  device_config_t loaded;

  if (g_device_config_initialized != 0U)
  {
    return;
  }
  g_device_config_initialized = 1U;

  if (device_config_load(&loaded) != 0U)
  {
    g_device_config = loaded;
    if (device_config_normalize_runtime_defaults(&g_device_config) != 0U)
    {
      g_device_config.config_crc32 = device_config_compute_crc32(&g_device_config);
      (void)device_config_save_direct(&g_device_config);
    }
    return;
  }

  device_config_build_defaults(&g_device_config);
  (void)device_config_save_direct(&g_device_config);
}

const device_config_t *device_config_get(void)
{
  return &g_device_config;
}

void device_config_set(const device_config_t *config)
{
  if (config == NULL)
  {
    return;
  }

  taskENTER_CRITICAL();
  g_device_config = *config;
  g_device_config.config_crc32 = device_config_compute_crc32(&g_device_config);
  taskEXIT_CRITICAL();
}

uint8_t device_config_update_and_save(const device_config_t *config)
{
  device_config_request_t request;
  device_config_t normalized;
  uint8_t unchanged;
  uint8_t saved;

  if (config == NULL)
  {
    return 0U;
  }

  normalized = *config;
  normalized.config_crc32 = device_config_compute_crc32(&normalized);

  taskENTER_CRITICAL();
  unchanged = (memcmp(&g_device_config, &normalized, sizeof(normalized)) == 0) ? 1U : 0U;
  g_device_config = normalized;
  request.config = g_device_config;
  taskEXIT_CRITICAL();

  if (unchanged != 0U)
  {
    return 1U;
  }

  device_config_worker_init();

  if ((g_device_config_queue == NULL) ||
      (g_device_config_task_handle == NULL) ||
      (g_device_config_save_mutex == NULL) ||
      (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING))
  {
    return device_config_save_direct(&request.config);
  }

  if (xSemaphoreTake(g_device_config_save_mutex,
                     pdMS_TO_TICKS(DEVICE_CONFIG_WAIT_MS)) != pdTRUE)
  {
    return 0U;
  }

  g_device_config_sync_result = 0U;
  request.result = &g_device_config_sync_result;
  request.waiting_task = xTaskGetCurrentTaskHandle();
  (void)ulTaskNotifyTake(pdTRUE, 0U);

  if (xQueueSend(g_device_config_queue, &request, pdMS_TO_TICKS(DEVICE_CONFIG_WAIT_MS)) != pdPASS)
  {
    (void)xSemaphoreGive(g_device_config_save_mutex);
    return 0U;
  }

  (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  saved = g_device_config_sync_result;
  (void)xSemaphoreGive(g_device_config_save_mutex);
  return saved;
}

uint32_t device_config_compute_crc32(const device_config_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  return device_config_crc32_zeroed_range((const uint8_t *)config,
                                          (uint32_t)sizeof(*config),
                                          (uint32_t)offsetof(device_config_t, config_crc32),
                                          (uint32_t)sizeof(config->config_crc32));
}

uint8_t device_config_is_valid(const device_config_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->magic != DEVICE_CONFIG_MAGIC)
  {
    return 0U;
  }

  if ((config->version != DEVICE_CONFIG_VERSION) || (config->size != sizeof(*config)))
  {
    return 0U;
  }

  return (config->config_crc32 == device_config_compute_crc32(config)) ? 1U : 0U;
}

uint8_t device_config_load(device_config_t *config)
{
  const device_config_t *stored = (const device_config_t *)DEVICE_CONFIG_FLASH_ADDRESS;
  const device_config_v5_t *stored_v5 = (const device_config_v5_t *)DEVICE_CONFIG_FLASH_ADDRESS;
  const device_config_v4_t *stored_v4 = (const device_config_v4_t *)DEVICE_CONFIG_FLASH_ADDRESS;
  const device_config_v3_t *stored_v3 = (const device_config_v3_t *)DEVICE_CONFIG_FLASH_ADDRESS;
  const device_config_v2_t *stored_v2 = (const device_config_v2_t *)DEVICE_CONFIG_FLASH_ADDRESS;
  const device_config_v1_t *stored_v1 = (const device_config_v1_t *)DEVICE_CONFIG_FLASH_ADDRESS;
  uint32_t index;

  if (config == NULL)
  {
    return 0U;
  }

  memcpy(config, stored, sizeof(*config));
  if (device_config_is_valid(config) != 0U)
  {
    return 1U;
  }

  if (device_config_v5_is_valid(stored_v5) != 0U)
  {
    memset(config, 0, sizeof(*config));
    config->magic = DEVICE_CONFIG_MAGIC;
    config->version = DEVICE_CONFIG_VERSION;
    config->size = (uint16_t)sizeof(*config);
    memcpy(config->device_name, stored_v5->device_name, sizeof(config->device_name));
    config->auto_join_cluster = stored_v5->auto_join_cluster;
    config->cluster_enabled = stored_v5->cluster_enabled;
    memcpy(config->cluster_name, stored_v5->cluster_name, sizeof(config->cluster_name));
    memcpy(config->cluster_password_hash, stored_v5->cluster_password_hash,
           sizeof(config->cluster_password_hash));
    memcpy(config->backend_host, stored_v5->backend_host, sizeof(config->backend_host));
    config->backend_port = stored_v5->backend_port;
    memcpy(config->backend_key, stored_v5->backend_key, sizeof(config->backend_key));
    config->cloud_enabled = stored_v5->cloud_enabled;
    config->cloud_transport = stored_v5->cloud_transport;
    memcpy(config->cloud_broker_host, stored_v5->cloud_broker_host,
           sizeof(config->cloud_broker_host));
    config->cloud_broker_port = stored_v5->cloud_broker_port;
    memcpy(config->cloud_cluster_id, stored_v5->cloud_cluster_id,
           sizeof(config->cloud_cluster_id));
    memcpy(config->cloud_device_id, stored_v5->cloud_device_id,
           sizeof(config->cloud_device_id));
    memcpy(config->cloud_key_id, stored_v5->cloud_key_id,
           sizeof(config->cloud_key_id));
    memcpy(config->cloud_cluster_ca_fingerprint,
           stored_v5->cloud_cluster_ca_fingerprint,
           sizeof(config->cloud_cluster_ca_fingerprint));
    memcpy(config->cloud_cluster_binding_secret,
           stored_v5->cloud_cluster_binding_secret,
           sizeof(config->cloud_cluster_binding_secret));
    config->saved_relay_mask = stored_v5->saved_relay_mask;
    memcpy(config->relay, stored_v5->relay, sizeof(config->relay));
    device_config_build_current_defaults(config);
    config->config_crc32 = device_config_compute_crc32(config);
    return 1U;
  }

  if (device_config_v4_is_valid(stored_v4) != 0U)
  {
    memset(config, 0, sizeof(*config));
    config->magic = DEVICE_CONFIG_MAGIC;
    config->version = DEVICE_CONFIG_VERSION;
    config->size = (uint16_t)sizeof(*config);
    memcpy(config->device_name, stored_v4->device_name, sizeof(config->device_name));
    config->auto_join_cluster = stored_v4->auto_join_cluster;
    config->cluster_enabled = stored_v4->cluster_enabled;
    memcpy(config->cluster_name, stored_v4->cluster_name, sizeof(config->cluster_name));
    memcpy(config->cluster_password_hash, stored_v4->cluster_password_hash, sizeof(config->cluster_password_hash));
    memcpy(config->backend_host, stored_v4->backend_host, sizeof(config->backend_host));
    config->backend_port = stored_v4->backend_port;
    memcpy(config->backend_key, stored_v4->backend_key, sizeof(config->backend_key));
    config->cloud_enabled = stored_v4->cloud_enabled;
    config->cloud_transport = stored_v4->cloud_transport;
    memcpy(config->cloud_broker_host, stored_v4->cloud_broker_host, sizeof(config->cloud_broker_host));
    config->cloud_broker_port = stored_v4->cloud_broker_port;
    memcpy(config->cloud_cluster_id, stored_v4->cloud_cluster_id, sizeof(config->cloud_cluster_id));
    memcpy(config->cloud_device_id, stored_v4->cloud_device_id, sizeof(config->cloud_device_id));
    memcpy(config->cloud_key_id, stored_v4->cloud_key_id, sizeof(config->cloud_key_id));
    config->cloud_cluster_ca_fingerprint[0] = '\0';
    config->cloud_cluster_binding_secret[0] = '\0';
    config->saved_relay_mask = stored_v4->saved_relay_mask;
    memcpy(config->relay, stored_v4->relay, sizeof(config->relay));
    device_config_build_current_defaults(config);
    config->config_crc32 = device_config_compute_crc32(config);
    return 1U;
  }

  if (device_config_v3_is_valid(stored_v3) != 0U)
  {
    memset(config, 0, sizeof(*config));
    config->magic = DEVICE_CONFIG_MAGIC;
    config->version = DEVICE_CONFIG_VERSION;
    config->size = (uint16_t)sizeof(*config);
    memcpy(config->device_name, stored_v3->device_name, sizeof(config->device_name));
    config->auto_join_cluster = stored_v3->auto_join_cluster;
    config->cluster_enabled = stored_v3->cluster_enabled;
    memcpy(config->cluster_name, stored_v3->cluster_name, sizeof(config->cluster_name));
    memcpy(config->cluster_password_hash,
           stored_v3->cluster_password_hash,
           sizeof(config->cluster_password_hash));
    memcpy(config->backend_host, stored_v3->backend_host, sizeof(config->backend_host));
    config->backend_port = stored_v3->backend_port;
    memcpy(config->backend_key, stored_v3->backend_key, sizeof(config->backend_key));
    config->cloud_enabled = 0U;
    config->cloud_transport = 1U;
    memcpy(config->cloud_broker_host, "broker.example.com", sizeof("broker.example.com"));
    config->cloud_broker_port = 8883U;
    config->cloud_cluster_id[0] = '\0';
    config->cloud_device_id[0] = '\0';
    config->cloud_key_id[0] = '\0';
    config->cloud_cluster_ca_fingerprint[0] = '\0';
    config->cloud_cluster_binding_secret[0] = '\0';
    config->saved_relay_mask = stored_v3->saved_relay_mask;
    memcpy(config->relay, stored_v3->relay, sizeof(config->relay));
    device_config_build_current_defaults(config);
    config->config_crc32 = device_config_compute_crc32(config);
    return 1U;
  }

  if (device_config_v2_is_valid(stored_v2) != 0U)
  {
    memset(config, 0, sizeof(*config));
    config->magic = DEVICE_CONFIG_MAGIC;
    config->version = DEVICE_CONFIG_VERSION;
    config->size = (uint16_t)sizeof(*config);
    memcpy(config->device_name, stored_v2->device_name, sizeof(config->device_name));
    config->auto_join_cluster = stored_v2->auto_join_cluster;
    config->cluster_enabled = stored_v2->cluster_enabled;
    memcpy(config->cluster_name, stored_v2->cluster_name, sizeof(config->cluster_name));
    memcpy(config->cluster_password_hash,
           stored_v2->cluster_password_hash,
           sizeof(config->cluster_password_hash));
    config->saved_relay_mask = stored_v2->saved_relay_mask;
    memcpy(config->relay, stored_v2->relay, sizeof(config->relay));
    memcpy(config->backend_host, "192.168.0.10", sizeof("192.168.0.10"));
    config->backend_port = 42001U;
    config->cloud_enabled = 0U;
    config->cloud_transport = 1U;
    memcpy(config->cloud_broker_host, "broker.example.com", sizeof("broker.example.com"));
    config->cloud_broker_port = 8883U;
    config->cloud_cluster_id[0] = '\0';
    config->cloud_device_id[0] = '\0';
    config->cloud_key_id[0] = '\0';
    config->cloud_cluster_ca_fingerprint[0] = '\0';
    config->cloud_cluster_binding_secret[0] = '\0';
    device_config_build_current_defaults(config);
    config->config_crc32 = device_config_compute_crc32(config);
    return 1U;
  }

  if (device_config_v1_is_valid(stored_v1) != 0U)
  {
    memset(config, 0, sizeof(*config));
    config->magic = DEVICE_CONFIG_MAGIC;
    config->version = DEVICE_CONFIG_VERSION;
    config->size = (uint16_t)sizeof(*config);
    memcpy(config->device_name, stored_v1->device_name, sizeof(config->device_name));
    config->auto_join_cluster = stored_v1->auto_join_cluster;
    config->cluster_enabled = stored_v1->cluster_enabled;
    memcpy(config->cluster_name, stored_v1->cluster_name, sizeof(config->cluster_name));
    memcpy(config->cluster_password_hash,
           stored_v1->cluster_password_hash,
           sizeof(config->cluster_password_hash));
    memcpy(config->backend_host, "192.168.0.10", sizeof("192.168.0.10"));
    config->backend_port = 42001U;
    config->cloud_enabled = 0U;
    config->cloud_transport = 1U;
    memcpy(config->cloud_broker_host, "broker.example.com", sizeof("broker.example.com"));
    config->cloud_broker_port = 8883U;
    config->cloud_cluster_id[0] = '\0';
    config->cloud_device_id[0] = '\0';
    config->cloud_key_id[0] = '\0';
    config->cloud_cluster_ca_fingerprint[0] = '\0';
    config->cloud_cluster_binding_secret[0] = '\0';
    memcpy(config->relay, stored_v1->relay, sizeof(config->relay));
    config->saved_relay_mask = 0U;
    for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
    {
      if (config->relay[index].boot_default_on != 0U)
      {
        config->saved_relay_mask |= (1UL << index);
      }
    }
    device_config_build_current_defaults(config);
    config->config_crc32 = device_config_compute_crc32(config);
    return 1U;
  }

  return 0U;
}

static uint8_t device_config_normalize_runtime_defaults(device_config_t *config)
{
  uint8_t changed = 0U;

  if (config == NULL)
  {
    return 0U;
  }

  if (config->backend_host[0] == '\0')
  {
    memcpy(config->backend_host, "192.168.0.139", sizeof("192.168.0.139"));
    changed = 1U;
  }

  if (config->backend_port == 0U)
  {
    config->backend_port = 42001U;
    changed = 1U;
  }

  if (config->backend_key[0] == '\0')
  {
    memcpy(config->backend_key, "dev-key", sizeof("dev-key"));
    changed = 1U;
  }

  return changed;
}

uint8_t device_config_save(const device_config_t *config)
{
  return device_config_save_direct(config);
}

static uint8_t device_config_save_direct(const device_config_t *config)
{
  const uint32_t *src32;
  uint32_t offset;
  device_config_t temp;

  if ((config == NULL) || (device_config_is_valid(config) == 0U))
  {
    return 0U;
  }

  temp = *config;
  temp.config_crc32 = device_config_compute_crc32(&temp);
  src32 = (const uint32_t *)&temp;
  g_device_config_last_flash_error = 0U;

  /* Disable interrupts during the entire flash erase+program sequence.
     On STM32F7, any interrupt that tries to fetch code from flash while
     a sector is being erased or programmed will cause an imprecise bus
     fault (CFSR=0x00000400).  The HAL flash helpers do not guarantee
     full interrupt masking for the complete operation window. */
  __disable_irq();

  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_ERSERR);

  if (device_config_flash_erase() == 0U)
  {
    HAL_FLASH_Lock();
    __enable_irq();
    return 0U;
  }

  for (offset = 0U; offset < sizeof(temp); offset += sizeof(uint32_t))
  {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          DEVICE_CONFIG_FLASH_ADDRESS + offset,
                          src32[offset / sizeof(uint32_t)]) != HAL_OK)
    {
      g_device_config_last_flash_error = HAL_FLASH_GetError();
      HAL_FLASH_Lock();
      __enable_irq();
      return 0U;
    }
  }

  HAL_FLASH_Lock();
  __enable_irq();
  return 1U;
}

static void device_config_worker_task(void *argument)
{
  device_config_request_t request;

  (void)argument;

  for (;;)
  {
    if (xQueueReceive(g_device_config_queue, &request, portMAX_DELAY) == pdPASS)
    {
      if (request.result != NULL)
      {
        *request.result = device_config_save_direct(&request.config);
      }

      if (request.waiting_task != NULL)
      {
        xTaskNotifyGive(request.waiting_task);
      }
    }
  }
}

static void device_config_worker_init(void)
{
  if ((g_device_config_queue != NULL) &&
      (g_device_config_task_handle != NULL) &&
      (g_device_config_save_mutex != NULL))
  {
    return;
  }

  if (g_device_config_queue == NULL)
  {
    g_device_config_queue =
      xQueueCreateStatic(DEVICE_CONFIG_QUEUE_LENGTH,
                         sizeof(device_config_request_t),
                         g_device_config_queue_storage,
                         &g_device_config_queue_buffer);
  }
  if (g_device_config_save_mutex == NULL)
  {
    g_device_config_save_mutex =
      xSemaphoreCreateMutexStatic(&g_device_config_save_mutex_buffer);
  }
  if ((g_device_config_queue != NULL) &&
      (g_device_config_save_mutex != NULL) &&
      (g_device_config_task_handle == NULL))
  {
    g_device_config_task_handle =
      xTaskCreateStatic(device_config_worker_task,
                        "CfgSave",
                        2048U,
                        NULL,
                        tskIDLE_PRIORITY + 1U,
                        g_device_config_task_stack,
                        &g_device_config_task_buffer);
    if (g_device_config_task_handle == NULL)
    {
      g_device_config_queue = NULL;
    }
  }
}

void device_config_reset(void)
{
  device_config_build_defaults(&g_device_config);
}

uint32_t device_config_last_flash_error(void)
{
  return g_device_config_last_flash_error;
}

uint32_t device_config_build_startup_relay_mask(const device_config_t *config)
{
  uint32_t mask = 0U;
  uint32_t index;

  if (config == NULL)
  {
    return 0U;
  }

  for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
  {
    if (config->relay[index].enabled == 0U)
    {
      continue;
    }

    if (config->relay[index].remember_state != 0U)
    {
      if ((config->saved_relay_mask & (1UL << index)) != 0U)
      {
        mask |= (1UL << index);
      }
    }
    else if (config->relay[index].boot_default_on != 0U)
    {
      mask |= (1UL << index);
    }
  }

  return mask;
}

uint8_t device_config_persist_runtime_relay_mask(uint32_t relay_mask)
{
  device_config_t config;
  uint32_t index;
  uint32_t new_saved_mask = 0U;

  config = g_device_config;
  for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
  {
    if ((config.relay[index].enabled != 0U) && (config.relay[index].remember_state != 0U))
    {
      if ((relay_mask & (1UL << index)) != 0U)
      {
        new_saved_mask |= (1UL << index);
      }
    }
    else if ((config.saved_relay_mask & (1UL << index)) != 0U)
    {
      new_saved_mask |= (1UL << index);
    }
  }

  if (new_saved_mask == config.saved_relay_mask)
  {
    return 1U;
  }

  config.saved_relay_mask = new_saved_mask;
  return device_config_update_and_save(&config);
}

void device_config_runtime_relay_mask_set(uint32_t relay_mask)
{
  uint32_t index;
  uint32_t new_saved_mask = 0U;

  taskENTER_CRITICAL();
  for (index = 0U; index < DEVICE_CONFIG_RELAY_COUNT; ++index)
  {
    if ((g_device_config.relay[index].enabled != 0U) &&
        (g_device_config.relay[index].remember_state != 0U) &&
        ((relay_mask & (1UL << index)) != 0U))
    {
      new_saved_mask |= (1UL << index);
    }
    else if ((g_device_config.relay[index].remember_state == 0U) &&
             ((g_device_config.saved_relay_mask & (1UL << index)) != 0U))
    {
      new_saved_mask |= (1UL << index);
    }
  }

  if (new_saved_mask != g_device_config.saved_relay_mask)
  {
    g_device_config.saved_relay_mask = new_saved_mask;
    g_device_config_runtime_dirty = 1U;
    g_device_config_runtime_due_ms = HAL_GetTick() + DEVICE_CONFIG_RUNTIME_SAVE_DELAY_MS;
  }
  taskEXIT_CRITICAL();
}

void device_config_poll(void)
{
  device_config_t snapshot;

  if (g_device_config_runtime_dirty == 0U)
  {
    return;
  }

  if ((int32_t)(HAL_GetTick() - g_device_config_runtime_due_ms) < 0)
  {
    return;
  }

  device_config_copy_locked(&snapshot);

  if (device_config_update_and_save(&snapshot) != 0U)
  {
    taskENTER_CRITICAL();
    if (g_device_config.saved_relay_mask == snapshot.saved_relay_mask)
    {
      g_device_config_runtime_dirty = 0U;
    }
    else
    {
      g_device_config_runtime_due_ms = HAL_GetTick() + DEVICE_CONFIG_RUNTIME_SAVE_DELAY_MS;
    }
    taskEXIT_CRITICAL();
  }
  else
  {
    g_device_config_runtime_due_ms = HAL_GetTick() + DEVICE_CONFIG_RUNTIME_SAVE_DELAY_MS;
  }
}
