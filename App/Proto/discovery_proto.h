#ifndef DISCOVERY_PROTO_H
#define DISCOVERY_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "../Config/device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISCOVERY_PROTO_ANNOUNCE_MAX_SIZE 640U

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
                                   const uint8_t mac[6]);
uint8_t discovery_proto_is_discover(const void *payload, uint16_t length);

typedef struct
{
  uint8_t valid;
  char service[24];
  char cluster[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN];
  char device[DEVICE_CONFIG_NAME_MAX_LEN];
  uint32_t node_id;
  uint8_t role;
  uint32_t term;
  uint32_t load_score;
  uint32_t auth_tag;
  uint32_t relay_mask;
  uint32_t enabled_mask;
  uint8_t auto_join;
  char cloud_cluster_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_device_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_key_id[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  uint32_t ip_addr;
  uint16_t port;
  uint8_t mac[6];
} discovery_proto_announce_t;

uint8_t discovery_proto_parse_announce(const void *payload,
                                       uint16_t length,
                                       discovery_proto_announce_t *announce);

#ifdef __cplusplus
}
#endif

#endif /* DISCOVERY_PROTO_H */
