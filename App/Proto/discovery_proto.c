#include "discovery_proto.h"

#include "lwip/ip4_addr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t discovery_proto_extract_value(const char *text,
                                             const char *key,
                                             char *buffer,
                                             size_t buffer_size)
{
  const char *match = text;
  const char *value;
  size_t index = 0U;

  if ((text == NULL) || (key == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  while ((match = strstr(match, key)) != NULL)
  {
    if ((match == text) || (match[-1] == ' '))
    {
      break;
    }
    match += strlen(key);
  }
  if (match == NULL)
  {
    buffer[0] = '\0';
    return 0U;
  }

  value = match + strlen(key);
  while ((*value != '\0') && (*value != ' ') && (index < (buffer_size - 1U)))
  {
    buffer[index++] = *value++;
  }
  buffer[index] = '\0';
  return (index > 0U) ? 1U : 0U;
}

static uint8_t discovery_proto_extract_optional_value(const char *text,
                                                      const char *key,
                                                      char *buffer,
                                                      size_t buffer_size)
{
  return discovery_proto_extract_value(text, key, buffer, buffer_size);
}

static uint8_t discovery_proto_parse_u32(const char *text, uint32_t *value_out)
{
  unsigned long value;
  char *endptr = NULL;

  if ((text == NULL) || (value_out == NULL) || (*text == '\0'))
  {
    return 0U;
  }

  value = strtoul(text, &endptr, 10);
  if ((*endptr != '\0') || (value > 0xFFFFFFFFUL))
  {
    return 0U;
  }

  *value_out = (uint32_t)value;
  return 1U;
}

int discovery_proto_build_announce(char *buffer,
                                   size_t buffer_size,
                                   const char *service,
                                   const char *cluster,
                                   const char *device,
                                   const char *cloud_cluster_id,
                                   const char *cloud_device_id,
                                   const char *cloud_key_id,
                                   uint32_t node_id,
                                   uint8_t role,
                                   uint32_t term,
                                   uint32_t load_score,
                                   uint32_t auth_tag,
                                   uint32_t relay_mask,
                                   uint32_t enabled_mask,
                                   uint32_t scheduler_enabled_mask,
                                   uint32_t scheduler_pause_mask,
                                   uint32_t scheduler_pause_blocks_mask,
                                   uint32_t protection_enabled_mask,
                                   uint32_t protection_tripped_mask,
                                   uint32_t protection_fault_lock_mask,
                                   uint32_t protection_ct_valid_mask,
                                   uint32_t protection_learning_mask,
                                   uint32_t protection_strategy_lsb_mask,
                                   uint32_t protection_strategy_msb_mask,
                                   uint8_t auto_join,
                                   uint32_t unix_time,
                                   uint8_t time_valid,
                                   uint8_t rtc_lse_ready,
                                   uint8_t time_source,
                                   uint8_t time_quality,
                                   uint32_t time_seq,
                                   uint32_t gateway_epoch,
                                   uint32_t gateway_node_id,
                                   uint8_t adc_valid,
                                   uint8_t adc_ct_valid,
                                   uint8_t adc_signal_present,
                                   uint32_t adc_raw,
                                   uint32_t adc_millivolts,
                                   uint32_t adc_min,
                                   uint32_t adc_max,
                                   uint32_t adc_average,
                                   uint32_t adc_raw_rms,
                                   uint32_t adc_rms_millivolts,
                                   uint32_t adc_current_milliamps,
                                   uint32_t adc_sample_rate_hz,
                                   uint32_t adc_window_samples,
                                   uint32_t adc_sample_count,
                                   uint32_t adc_dma_overruns,
                                   uint32_t adc_saturation_count,
                                   const char *ip_text,
                                   uint16_t port,
                                   const uint8_t mac[6])
{
  return snprintf(buffer, buffer_size,
                  "type=announce ver=8 service=%s cluster=%s device=%s cloud_cluster=%s cloud_device=%s cloud_key=%s node=%lu role=%u term=%lu load=%lu auth=%lu relay=%lu enabled=%lu sched_en=%lu sched_pause=%lu sched_blocks=%lu prot_en=%lu prot_trip=%lu prot_lock=%lu prot_ct=%lu prot_learn=%lu prot_s0=%lu prot_s1=%lu auto=%u time=%lu time_valid=%u rtc_lse=%u time_source=%u time_quality=%u time_seq=%lu gateway_epoch=%lu gateway_node=%lu adc_valid=%u adc_ct=%u adc_signal=%u adc_raw=%lu adc_mv=%lu adc_min=%lu adc_max=%lu adc_avg=%lu adc_raw_rms=%lu adc_rms_mv=%lu adc_current_ma=%lu adc_rate=%lu adc_window=%lu adc_samples=%lu adc_overruns=%lu adc_saturation=%lu ip=%s port=%u mac=%02X:%02X:%02X:%02X:%02X:%02X",
                  service,
                  cluster,
                  device,
                  cloud_cluster_id,
                  cloud_device_id,
                  cloud_key_id,
                  (unsigned long)node_id,
                  (unsigned int)role,
                  (unsigned long)term,
                  (unsigned long)load_score,
                  (unsigned long)auth_tag,
                  (unsigned long)relay_mask,
                  (unsigned long)enabled_mask,
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
                  (unsigned int)auto_join,
                  (unsigned long)unix_time,
                  (unsigned int)time_valid,
                  (unsigned int)rtc_lse_ready,
                  (unsigned int)time_source,
                  (unsigned int)time_quality,
                  (unsigned long)time_seq,
                  (unsigned long)gateway_epoch,
                  (unsigned long)gateway_node_id,
                  (unsigned int)adc_valid,
                  (unsigned int)adc_ct_valid,
                  (unsigned int)adc_signal_present,
                  (unsigned long)adc_raw,
                  (unsigned long)adc_millivolts,
                  (unsigned long)adc_min,
                  (unsigned long)adc_max,
                  (unsigned long)adc_average,
                  (unsigned long)adc_raw_rms,
                  (unsigned long)adc_rms_millivolts,
                  (unsigned long)adc_current_milliamps,
                  (unsigned long)adc_sample_rate_hz,
                  (unsigned long)adc_window_samples,
                  (unsigned long)adc_sample_count,
                  (unsigned long)adc_dma_overruns,
                  (unsigned long)adc_saturation_count,
                  ip_text,
                  (unsigned int)port,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

uint8_t discovery_proto_is_discover(const void *payload, uint16_t length)
{
  static const char token[] = "DISCOVER";

  if ((payload == NULL) || (length < (sizeof(token) - 1U)))
  {
    return 0U;
  }

  return (memcmp(payload, token, sizeof(token) - 1U) == 0) ? 1U : 0U;
}

uint8_t discovery_proto_parse_announce(const void *payload,
                                       uint16_t length,
                                       discovery_proto_announce_t *announce)
{
  char text[DISCOVERY_PROTO_ANNOUNCE_MAX_SIZE];
  char value[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  ip4_addr_t ip_addr;
  unsigned int mac[6];
  uint32_t parsed_u32;
  uint8_t version;

  if ((payload == NULL) || (announce == NULL) || (length == 0U))
  {
    return 0U;
  }

  if (length >= sizeof(text))
  {
    length = (uint16_t)(sizeof(text) - 1U);
  }

  memcpy(text, payload, length);
  text[length] = '\0';
  memset(announce, 0, sizeof(*announce));

  if (!discovery_proto_extract_value(text, "type=", value, sizeof(value)) ||
      (strcmp(value, "announce") != 0))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "ver=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &parsed_u32) ||
      (parsed_u32 < 2U) ||
      (parsed_u32 > 8U))
  {
    return 0U;
  }
  version = (uint8_t)parsed_u32;

  if (!discovery_proto_extract_value(text, "service=", announce->service, sizeof(announce->service)))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "cluster=", announce->cluster, sizeof(announce->cluster)))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "device=", announce->device, sizeof(announce->device)))
  {
    return 0U;
  }
  if (version >= 3U)
  {
    (void)discovery_proto_extract_optional_value(text, "cloud_cluster=", announce->cloud_cluster_id, sizeof(announce->cloud_cluster_id));
    (void)discovery_proto_extract_optional_value(text, "cloud_device=", announce->cloud_device_id, sizeof(announce->cloud_device_id));
    (void)discovery_proto_extract_optional_value(text, "cloud_key=", announce->cloud_key_id, sizeof(announce->cloud_key_id));
  }
  if (!discovery_proto_extract_value(text, "node=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &announce->node_id))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "role=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &parsed_u32) ||
      (parsed_u32 > 255U))
  {
    return 0U;
  }
  announce->role = (uint8_t)parsed_u32;
  if (!discovery_proto_extract_value(text, "term=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &announce->term))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "load=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &announce->load_score))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "auth=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &announce->auth_tag))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "relay=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &announce->relay_mask))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "enabled=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &announce->enabled_mask))
  {
    return 0U;
  }
  if (!discovery_proto_extract_value(text, "auto=", value, sizeof(value)) ||
      !discovery_proto_parse_u32(value, &parsed_u32) ||
      (parsed_u32 > 255U))
  {
    return 0U;
  }
  announce->auto_join = (uint8_t)parsed_u32;
  if (version >= 8U)
  {
    if (discovery_proto_extract_optional_value(text, "sched_en=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->scheduler_enabled_mask);
    }
    if (discovery_proto_extract_optional_value(text, "sched_pause=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->scheduler_pause_mask);
    }
    if (discovery_proto_extract_optional_value(text, "sched_blocks=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->scheduler_pause_blocks_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_en=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_enabled_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_trip=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_tripped_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_lock=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_fault_lock_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_ct=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_ct_valid_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_learn=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_learning_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_s0=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_strategy_lsb_mask);
    }
    if (discovery_proto_extract_optional_value(text, "prot_s1=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->protection_strategy_msb_mask);
    }
  }
  if (version >= 4U)
  {
    if (discovery_proto_extract_optional_value(text, "time=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->unix_time);
    }
    if ((discovery_proto_extract_optional_value(text, "time_valid=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 1U))
    {
      announce->time_valid = (uint8_t)parsed_u32;
    }
    if ((discovery_proto_extract_optional_value(text, "rtc_lse=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 1U))
    {
      announce->rtc_lse_ready = (uint8_t)parsed_u32;
    }
  }
  if (version >= 5U)
  {
    if ((discovery_proto_extract_optional_value(text, "time_source=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 255U))
    {
      announce->time_source = (uint8_t)parsed_u32;
    }
    if ((discovery_proto_extract_optional_value(text, "time_quality=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 255U))
    {
      announce->time_quality = (uint8_t)parsed_u32;
    }
    if (discovery_proto_extract_optional_value(text, "time_seq=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->time_seq);
    }
    if (discovery_proto_extract_optional_value(text, "gateway_epoch=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->gateway_epoch);
    }
    if (discovery_proto_extract_optional_value(text, "gateway_node=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->gateway_node_id);
    }
  }
  if (version >= 6U)
  {
    if ((discovery_proto_extract_optional_value(text, "adc_valid=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 1U))
    {
      announce->adc_valid = (uint8_t)parsed_u32;
    }
    if (discovery_proto_extract_optional_value(text, "adc_raw=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_raw);
    }
    if (discovery_proto_extract_optional_value(text, "adc_mv=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_millivolts);
    }
    if (discovery_proto_extract_optional_value(text, "adc_min=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_min);
    }
    if (discovery_proto_extract_optional_value(text, "adc_max=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_max);
    }
    if (discovery_proto_extract_optional_value(text, "adc_avg=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_average);
    }
    if (discovery_proto_extract_optional_value(text, "adc_samples=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_sample_count);
    }
  }
  if (version >= 7U)
  {
    if ((discovery_proto_extract_optional_value(text, "adc_ct=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 1U))
    {
      announce->adc_ct_valid = (uint8_t)parsed_u32;
    }
    if ((discovery_proto_extract_optional_value(text, "adc_signal=", value, sizeof(value)) != 0U) &&
        (discovery_proto_parse_u32(value, &parsed_u32) != 0U) &&
        (parsed_u32 <= 1U))
    {
      announce->adc_signal_present = (uint8_t)parsed_u32;
    }
    if (discovery_proto_extract_optional_value(text, "adc_raw_rms=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_raw_rms);
    }
    if (discovery_proto_extract_optional_value(text, "adc_rms_mv=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_rms_millivolts);
    }
    if (discovery_proto_extract_optional_value(text, "adc_current_ma=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_current_milliamps);
    }
    if (discovery_proto_extract_optional_value(text, "adc_rate=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_sample_rate_hz);
    }
    if (discovery_proto_extract_optional_value(text, "adc_window=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_window_samples);
    }
    if (discovery_proto_extract_optional_value(text, "adc_overruns=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_dma_overruns);
    }
    if (discovery_proto_extract_optional_value(text, "adc_saturation=", value, sizeof(value)) != 0U)
    {
      (void)discovery_proto_parse_u32(value, &announce->adc_saturation_count);
    }
  }
  if (!discovery_proto_extract_value(text, "ip=", value, sizeof(value)))
  {
    return 0U;
  }
  if (!ip4addr_aton(value, &ip_addr))
  {
    return 0U;
  }
  announce->ip_addr = ip4_addr_get_u32(&ip_addr);

  if (!discovery_proto_extract_value(text, "port=", value, sizeof(value)))
  {
    return 0U;
  }
  {
    uint32_t port_value;
    if (!discovery_proto_parse_u32(value, &port_value) || (port_value > 65535U))
    {
      return 0U;
    }
    announce->port = (uint16_t)port_value;
  }

  if (!discovery_proto_extract_value(text, "mac=", value, sizeof(value)))
  {
    return 0U;
  }
  if (sscanf(value, "%02X:%02X:%02X:%02X:%02X:%02X",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6)
  {
    return 0U;
  }

  announce->mac[0] = (uint8_t)mac[0];
  announce->mac[1] = (uint8_t)mac[1];
  announce->mac[2] = (uint8_t)mac[2];
  announce->mac[3] = (uint8_t)mac[3];
  announce->mac[4] = (uint8_t)mac[4];
  announce->mac[5] = (uint8_t)mac[5];
  announce->valid = 1U;
  return 1U;
}
