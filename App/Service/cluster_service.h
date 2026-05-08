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
  uint32_t auth_tag;
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
  uint32_t self_node_id;
  uint32_t master_node_id;
  uint32_t term;
  uint32_t node_count;
  uint32_t health_score;
  uint32_t health_lateness_ms;
  uint32_t health_bad_streak;
  char cluster_name[CLUSTER_SERVICE_CLUSTER_NAME_LEN];
} cluster_service_state_t;

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

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_SERVICE_H */
