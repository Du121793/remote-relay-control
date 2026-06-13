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
  const char *match;
  const char *value;
  size_t index = 0U;

  if ((text == NULL) || (key == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  match = strstr(text, key);
  if (match == NULL)
  {
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
  const char *match;
  const char *value;
  size_t index = 0U;

  if ((text == NULL) || (key == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return 0U;
  }

  match = strstr(text, key);
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
  return 1U;
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
                                   uint8_t auto_join,
                                   const char *ip_text,
                                   uint16_t port,
                                   const uint8_t mac[6])
{
  return snprintf(buffer, buffer_size,
                  "type=announce ver=3 service=%s cluster=%s device=%s cloud_cluster=%s cloud_device=%s cloud_key=%s node=%lu role=%u term=%lu load=%lu auth=%lu relay=%lu enabled=%lu auto=%u ip=%s port=%u mac=%02X:%02X:%02X:%02X:%02X:%02X",
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
                  (unsigned int)auto_join,
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

  if (strstr(text, "type=announce") == NULL)
  {
    return 0U;
  }
  if (strstr(text, "ver=3") != NULL)
  {
    version = 3U;
  }
  else if (strstr(text, "ver=2") != NULL)
  {
    version = 2U;
  }
  else
  {
    return 0U;
  }

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
