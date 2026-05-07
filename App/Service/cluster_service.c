#include "cluster_service.h"

#include <string.h>

#include "stm32f7xx_hal.h"

#define CLUSTER_SERVICE_STALE_TIMEOUT_MS   6000UL
#define CLUSTER_SERVICE_BAN_CAPACITY       16U

typedef struct
{
  uint8_t initialized;
  uint32_t local_auth_tag;
  uint32_t local_load_score;
  cluster_service_state_t state;
  cluster_node_t nodes[CLUSTER_SERVICE_NODE_CAPACITY];
  uint32_t banned_node_ids[CLUSTER_SERVICE_BAN_CAPACITY];
} cluster_service_context_t;

static cluster_service_context_t g_cluster_context;

static uint8_t cluster_service_is_node_banned(uint32_t node_id)
{
  uint32_t index;

  if (node_id == 0U)
  {
    return 0U;
  }

  for (index = 0U; index < CLUSTER_SERVICE_BAN_CAPACITY; ++index)
  {
    if (g_cluster_context.banned_node_ids[index] == node_id)
    {
      return 1U;
    }
  }

  return 0U;
}

static void cluster_service_ban_node(uint32_t node_id)
{
  uint32_t index;

  if ((node_id == 0U) || (node_id == g_cluster_context.state.self_node_id))
  {
    return;
  }

  for (index = 0U; index < CLUSTER_SERVICE_BAN_CAPACITY; ++index)
  {
    if (g_cluster_context.banned_node_ids[index] == node_id)
    {
      return;
    }
  }

  for (index = 0U; index < CLUSTER_SERVICE_BAN_CAPACITY; ++index)
  {
    if (g_cluster_context.banned_node_ids[index] == 0U)
    {
      g_cluster_context.banned_node_ids[index] = node_id;
      return;
    }
  }

  g_cluster_context.banned_node_ids[0] = node_id;
}

static uint32_t cluster_service_build_node_id(const device_config_t *config)
{
  uint32_t hash = 2166136261UL;
  uint32_t uid_words[3];
  const uint8_t *uid_bytes = (const uint8_t *)uid_words;
  uint32_t i;

  (void)config;

  uid_words[0] = HAL_GetUIDw0();
  uid_words[1] = HAL_GetUIDw1();
  uid_words[2] = HAL_GetUIDw2();

  for (i = 0U; i < sizeof(uid_words); ++i)
  {
    hash ^= uid_bytes[i];
    hash *= 16777619UL;
  }

  if (hash == 0U)
  {
    hash = 1U;
  }

  return hash;
}

static uint8_t cluster_service_node_is_eligible(const cluster_node_t *node,
                                                const cluster_service_state_t *state)
{
  if ((node == NULL) || (state == NULL) || (node->valid == 0U))
  {
    return 0U;
  }

  if (state->cluster_enabled == 0U)
  {
    return 0U;
  }

  if ((strcmp(node->cluster_name, state->cluster_name) != 0) ||
      (node->auth_tag != g_cluster_context.local_auth_tag))
  {
    return 0U;
  }

  return 1U;
}

static void cluster_service_recount_nodes(void)
{
  uint32_t index;
  uint32_t count = 0U;

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    if (g_cluster_context.nodes[index].valid != 0U)
    {
      count++;
    }
  }

  g_cluster_context.state.node_count = count;
}

static void cluster_service_drop_stale_nodes(uint32_t now_ms)
{
  uint32_t index;

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    cluster_node_t *node = &g_cluster_context.nodes[index];

    if ((node->valid != 0U) &&
        ((int32_t)(now_ms - node->last_seen_ms) > (int32_t)CLUSTER_SERVICE_STALE_TIMEOUT_MS))
    {
      memset(node, 0, sizeof(*node));
    }
  }
}

static void cluster_service_run_election(void)
{
  const device_config_t *config = device_config_get();
  uint32_t best_node_id = 0U;
  uint32_t best_term = 0U;
  uint32_t best_load = 0xFFFFFFFFUL;
  uint32_t index;
  uint8_t found_master_candidate = 0U;
  uint8_t found_any_candidate = 0U;
  uint8_t self_eligible;
  uint32_t max_seen_term = g_cluster_context.state.term;
  uint32_t self_load = 0U;
  uint8_t self_is_master = (g_cluster_context.state.role == CLUSTER_ROLE_MASTER) ? 1U : 0U;

  g_cluster_context.state.has_master = 0U;
  g_cluster_context.state.master_node_id = 0U;

  if ((config->cluster_enabled == 0U) || (g_cluster_context.state.join_mode == CLUSTER_JOIN_DISABLED))
  {
    g_cluster_context.state.role = CLUSTER_ROLE_STANDALONE;
    return;
  }

  self_eligible = 1U;
  self_load = g_cluster_context.local_load_score;

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    const cluster_node_t *node = &g_cluster_context.nodes[index];

    if (!cluster_service_node_is_eligible(node, &g_cluster_context.state))
    {
      continue;
    }

    if (node->term > max_seen_term)
    {
      max_seen_term = node->term;
    }

    if (node->role == CLUSTER_ROLE_MASTER)
    {
      if ((found_master_candidate == 0U) ||
          (node->term > best_term) ||
          ((node->term == best_term) && (node->load_score < best_load)) ||
          ((node->term == best_term) && (node->load_score == best_load) && (node->node_id < best_node_id)))
      {
        best_node_id = node->node_id;
        best_term = node->term;
        best_load = node->load_score;
        found_master_candidate = 1U;
      }
    }

    found_any_candidate = 1U;
  }

  if ((self_eligible != 0U) && (self_is_master != 0U))
  {
    if ((found_master_candidate == 0U) ||
        (g_cluster_context.state.term > best_term) ||
        ((g_cluster_context.state.term == best_term) && (self_load < best_load)) ||
        ((g_cluster_context.state.term == best_term) && (self_load == best_load) &&
         (g_cluster_context.state.self_node_id < best_node_id)))
    {
      best_node_id = g_cluster_context.state.self_node_id;
      best_term = g_cluster_context.state.term;
      best_load = self_load;
      found_master_candidate = 1U;
    }
  }

  if (found_master_candidate == 0U)
  {
    if (self_eligible != 0U)
    {
      best_node_id = g_cluster_context.state.self_node_id;
      best_load = self_load;
      found_any_candidate = 1U;
    }

    for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
    {
      const cluster_node_t *node = &g_cluster_context.nodes[index];

      if (!cluster_service_node_is_eligible(node, &g_cluster_context.state))
      {
        continue;
      }

      if ((best_node_id == 0U) ||
          (node->load_score < best_load) ||
          ((node->load_score == best_load) && (node->node_id < best_node_id)))
      {
        best_load = node->load_score;
        best_node_id = node->node_id;
      }
    }
  }

  if ((self_eligible == 0U) && (found_any_candidate == 0U) && (best_node_id == 0U))
  {
    g_cluster_context.state.role = CLUSTER_ROLE_STANDALONE;
    return;
  }

  g_cluster_context.state.has_master = 1U;
  g_cluster_context.state.master_node_id = best_node_id;
  if (best_node_id == g_cluster_context.state.self_node_id)
  {
    if (g_cluster_context.state.role != CLUSTER_ROLE_MASTER)
    {
      g_cluster_context.state.term = max_seen_term + 1U;
    }
    g_cluster_context.state.role = CLUSTER_ROLE_MASTER;
  }
  else
  {
    g_cluster_context.state.role = CLUSTER_ROLE_MEMBER;
  }
}

void cluster_service_reload_from_config(void)
{
  const device_config_t *config = device_config_get();

  cluster_service_init();

  g_cluster_context.state.cluster_enabled = config->cluster_enabled;
  g_cluster_context.state.join_mode = (config->cluster_enabled != 0U)
                                      ? ((config->auto_join_cluster != 0U) ? CLUSTER_JOIN_AUTO : CLUSTER_JOIN_MANUAL)
                                      : CLUSTER_JOIN_DISABLED;
  memcpy(g_cluster_context.state.cluster_name, config->cluster_name, sizeof(g_cluster_context.state.cluster_name));
  memset(g_cluster_context.nodes, 0, sizeof(g_cluster_context.nodes));
  memset(g_cluster_context.banned_node_ids, 0, sizeof(g_cluster_context.banned_node_ids));
  g_cluster_context.local_auth_tag = device_config_password_tag(config);
  g_cluster_context.local_load_score = 0U;
  g_cluster_context.state.has_master = 0U;
  g_cluster_context.state.master_node_id = 0U;
  g_cluster_context.state.node_count = 0U;
  g_cluster_context.state.role = CLUSTER_ROLE_STANDALONE;
}

void cluster_service_init(void)
{
  const device_config_t *config;

  if (g_cluster_context.initialized != 0U)
  {
    return;
  }

  device_config_init();
  config = device_config_get();
  memset(&g_cluster_context, 0, sizeof(g_cluster_context));
  g_cluster_context.state.cluster_enabled = config->cluster_enabled;
  g_cluster_context.state.join_mode = (config->cluster_enabled != 0U)
                                      ? ((config->auto_join_cluster != 0U) ? CLUSTER_JOIN_AUTO : CLUSTER_JOIN_MANUAL)
                                      : CLUSTER_JOIN_DISABLED;
  g_cluster_context.state.role = CLUSTER_ROLE_STANDALONE;
  g_cluster_context.state.self_node_id = cluster_service_build_node_id(config);
  g_cluster_context.state.master_node_id = 0U;
  g_cluster_context.state.term = 0U;
  memcpy(g_cluster_context.state.cluster_name, config->cluster_name, sizeof(g_cluster_context.state.cluster_name));
  g_cluster_context.local_auth_tag = device_config_password_tag(config);
  g_cluster_context.local_load_score = 0U;
  memset(g_cluster_context.banned_node_ids, 0, sizeof(g_cluster_context.banned_node_ids));
  g_cluster_context.initialized = 1U;
}

void cluster_service_poll(void)
{
  cluster_service_init();
  cluster_service_drop_stale_nodes(HAL_GetTick());
  cluster_service_recount_nodes();
  cluster_service_run_election();
}

void cluster_service_reset(void)
{
  memset(&g_cluster_context, 0, sizeof(g_cluster_context));
}

void cluster_service_get_state(cluster_service_state_t *state)
{
  cluster_service_init();

  if (state != NULL)
  {
    *state = g_cluster_context.state;
  }
}

const cluster_node_t *cluster_service_get_nodes(uint32_t *count)
{
  cluster_service_init();

  if (count != NULL)
  {
    *count = g_cluster_context.state.node_count;
  }

  return g_cluster_context.nodes;
}

const cluster_node_t *cluster_service_find_node(uint32_t node_id)
{
  uint32_t index;

  cluster_service_init();
  if (node_id == 0U)
  {
    return NULL;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    if ((g_cluster_context.nodes[index].valid != 0U) &&
        (g_cluster_context.nodes[index].node_id == node_id))
    {
      return &g_cluster_context.nodes[index];
    }
  }

  return NULL;
}

uint8_t cluster_service_remove_node(uint32_t node_id)
{
  uint32_t index;

  cluster_service_init();
  if ((node_id == 0U) || (node_id == g_cluster_context.state.self_node_id))
  {
    return 0U;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    if ((g_cluster_context.nodes[index].valid != 0U) &&
        (g_cluster_context.nodes[index].node_id == node_id))
    {
      memset(&g_cluster_context.nodes[index], 0, sizeof(g_cluster_context.nodes[index]));
      cluster_service_recount_nodes();
      cluster_service_run_election();
      return 1U;
    }
  }

  return 0U;
}

void cluster_service_note_remote_node(const cluster_node_t *node)
{
  uint32_t index;
  cluster_node_t *target = NULL;

  cluster_service_init();

  if ((node == NULL) || (node->valid == 0U))
  {
    return;
  }

  if ((node->node_id == 0U) || (node->node_id == g_cluster_context.state.self_node_id))
  {
    return;
  }

  if (node->node_name[0] == '\0')
  {
    return;
  }

  if (cluster_service_is_node_banned(node->node_id) != 0U)
  {
    return;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    if ((g_cluster_context.nodes[index].valid != 0U) &&
        (g_cluster_context.nodes[index].node_id == node->node_id))
    {
      target = &g_cluster_context.nodes[index];
      break;
    }
  }

  if (target == NULL)
  {
    for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
    {
      if (g_cluster_context.nodes[index].valid == 0U)
      {
        target = &g_cluster_context.nodes[index];
        break;
      }
    }
  }

  if (target == NULL)
  {
    uint32_t oldest_index = 0U;
    uint32_t oldest_seen = 0xFFFFFFFFUL;

    for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
    {
      if (g_cluster_context.nodes[index].last_seen_ms < oldest_seen)
      {
        oldest_seen = g_cluster_context.nodes[index].last_seen_ms;
        oldest_index = index;
      }
    }
    target = &g_cluster_context.nodes[oldest_index];
  }

  *target = *node;
  cluster_service_recount_nodes();
}

uint8_t cluster_service_kick_node(uint32_t node_id)
{
  uint8_t removed;

  cluster_service_ban_node(node_id);
  removed = cluster_service_remove_node(node_id);
  return removed;
}

uint8_t cluster_service_apply_remote_command(uint32_t node_id, const control_proto_command_t *command)
{
  uint32_t index;
  cluster_node_t *node = NULL;

  cluster_service_init();
  if ((node_id == 0U) || (command == NULL))
  {
    return 0U;
  }

  for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
  {
    if ((g_cluster_context.nodes[index].valid != 0U) &&
        (g_cluster_context.nodes[index].node_id == node_id))
    {
      node = &g_cluster_context.nodes[index];
      break;
    }
  }

  if (node == NULL)
  {
    return 0U;
  }

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_SET_RELAY:
      if ((command->channel == 0U) || (command->channel > 32U))
      {
        return 0U;
      }
      if (command->value != 0U)
      {
        node->relay_mask |= (1UL << (command->channel - 1U));
      }
      else
      {
        node->relay_mask &= ~(1UL << (command->channel - 1U));
      }
      return 1U;

    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
      node->relay_mask = command->mask;
      return 1U;

    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      node->relay_mask = (command->value != 0U) ? 0xFFFFUL : 0U;
      return 1U;

    default:
      return 0U;
  }
}

uint32_t cluster_service_compute_load_score(uint32_t http_active_connections,
                                            uint32_t control_connections,
                                            uint32_t pending_commands)
{
  return (http_active_connections * 4U) +
         (control_connections * 3U) +
         (pending_commands * 2U);
}

void cluster_service_set_local_load_score(uint32_t load_score)
{
  cluster_service_init();
  g_cluster_context.local_load_score = load_score;
}
