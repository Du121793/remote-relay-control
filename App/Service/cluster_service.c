#include "cluster_service.h"
#include "backend_link_service.h"

#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#include "stm32f7xx_hal.h"

#define CLUSTER_SERVICE_STALE_TIMEOUT_MS   6000UL
#define CLUSTER_SERVICE_BAN_CAPACITY       16U
#define CLUSTER_SERVICE_MASTER_SWITCH_DELAY_MS  2500UL
#define CLUSTER_SERVICE_STARTUP_PROMOTION_DELAY_MS  4000UL
#define CLUSTER_SERVICE_FAILOVER_PROMOTION_DELAY_MS 3000UL
#define CLUSTER_SERVICE_HEALTH_TASK_PERIOD_MS   500UL
#define CLUSTER_SERVICE_HEALTH_BAD_THRESHOLD    450UL
#define CLUSTER_SERVICE_HEALTH_GOOD_THRESHOLD   700UL
#define CLUSTER_SERVICE_HEALTH_UNFIT_DELAY_MS   15000UL
#define CLUSTER_SERVICE_HEALTH_RECOVER_DELAY_MS 10000UL

typedef struct
{
  uint8_t initialized;
  uint32_t local_auth_tag;
  uint32_t local_load_score;
  uint32_t pending_master_node_id;
  uint32_t pending_master_since_ms;
  uint32_t promotion_candidate_node_id;
  uint32_t promotion_candidate_since_ms;
  uint32_t last_remote_master_seen_ms;
  uint32_t last_remote_master_node_id;
  uint32_t health_bad_since_ms;
  uint32_t health_good_since_ms;
  cluster_service_state_t state;
  cluster_node_t nodes[CLUSTER_SERVICE_NODE_CAPACITY];
  uint32_t banned_node_ids[CLUSTER_SERVICE_BAN_CAPACITY];
  TaskHandle_t health_task_handle;
} cluster_service_context_t;

static cluster_service_context_t g_cluster_context;

static void cluster_service_health_task(void *argument);
static uint32_t cluster_service_health_score_from_lateness(uint32_t lateness_ms);

static void cluster_service_mark_backend_dirty(void)
{
  backend_link_service_mark_status_dirty();
}

static uint8_t cluster_service_device_state_from_relay_mask(uint32_t relay_mask)
{
  (void)relay_mask;
  return CLUSTER_DEVICE_STATE_RUNNING;
}

static uint32_t cluster_service_health_score_from_lateness(uint32_t lateness_ms)
{
  uint32_t penalty = 0U;

  if (lateness_ms <= 20U)
  {
    penalty = 0U;
  }
  else if (lateness_ms <= 50U)
  {
    penalty = lateness_ms - 20U;
  }
  else if (lateness_ms <= 100U)
  {
    penalty = 30U + ((lateness_ms - 50U) * 2U);
  }
  else
  {
    penalty = 130U + ((lateness_ms - 100U) * 8U);
  }

  if (penalty >= 1000U)
  {
    return 0U;
  }

  return 1000U - penalty;
}

static void cluster_service_health_task(void *argument)
{
  const TickType_t period_ticks = pdMS_TO_TICKS(CLUSTER_SERVICE_HEALTH_TASK_PERIOD_MS);
  uint32_t expected_ms = HAL_GetTick() + CLUSTER_SERVICE_HEALTH_TASK_PERIOD_MS;

  (void)argument;

  for (;;)
  {
    uint32_t current_ms;
    uint32_t lateness_ms;
    uint32_t health_score;
    uint32_t now_ms;
    uint8_t previous_master_eligible;
    uint8_t mark_dirty = 0U;

    vTaskDelay(period_ticks);
    current_ms = HAL_GetTick();
    if ((int32_t)(current_ms - expected_ms) > 0)
    {
      lateness_ms = current_ms - expected_ms;
    }
    else
    {
      lateness_ms = 0U;
    }
    expected_ms += CLUSTER_SERVICE_HEALTH_TASK_PERIOD_MS;

    health_score = cluster_service_health_score_from_lateness(lateness_ms);
    now_ms = current_ms;

    taskENTER_CRITICAL();
    previous_master_eligible = g_cluster_context.state.master_eligible;
    g_cluster_context.state.health_lateness_ms = lateness_ms;
    g_cluster_context.state.health_score = health_score;

    if (health_score <= CLUSTER_SERVICE_HEALTH_BAD_THRESHOLD)
    {
      g_cluster_context.state.health_bad_streak++;
      g_cluster_context.health_good_since_ms = 0U;
      if (g_cluster_context.health_bad_since_ms == 0U)
      {
        g_cluster_context.health_bad_since_ms = now_ms;
      }
      else if ((int32_t)(now_ms - g_cluster_context.health_bad_since_ms) >=
               (int32_t)CLUSTER_SERVICE_HEALTH_UNFIT_DELAY_MS)
      {
        g_cluster_context.state.master_eligible = 0U;
      }
    }
    else
    {
      g_cluster_context.state.health_bad_streak = 0U;
      g_cluster_context.health_bad_since_ms = 0U;
      if (health_score >= CLUSTER_SERVICE_HEALTH_GOOD_THRESHOLD)
      {
        if (g_cluster_context.health_good_since_ms == 0U)
        {
          g_cluster_context.health_good_since_ms = now_ms;
        }
        else if ((int32_t)(now_ms - g_cluster_context.health_good_since_ms) >=
                 (int32_t)CLUSTER_SERVICE_HEALTH_RECOVER_DELAY_MS)
        {
          g_cluster_context.state.master_eligible = 1U;
        }
      }
      else
      {
        g_cluster_context.health_good_since_ms = 0U;
      }
    }
    if (previous_master_eligible != g_cluster_context.state.master_eligible)
    {
      mark_dirty = 1U;
    }
    taskEXIT_CRITICAL();

    if (mark_dirty != 0U)
    {
      cluster_service_mark_backend_dirty();
    }
  }
}

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
  if ((node == NULL) || (state == NULL) || (node->valid == 0U) || (node->online == 0U))
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
      if (node->online != 0U)
      {
        cluster_service_mark_backend_dirty();
      }
      node->online = 0U;
      node->device_state = CLUSTER_DEVICE_STATE_STOPPED;
    }
  }
}

static void cluster_service_run_election(void)
{
  const device_config_t *config = device_config_get();
  const uint32_t now_ms = HAL_GetTick();
  const uint32_t remembered_master_grace_ms = CLUSTER_SERVICE_STALE_TIMEOUT_MS + CLUSTER_SERVICE_MASTER_SWITCH_DELAY_MS;
  uint32_t best_node_id = 0U;
  uint32_t best_term = 0U;
  uint32_t index;
  uint8_t found_master_candidate = 0U;
  uint8_t found_any_candidate = 0U;
  uint8_t self_eligible;
  uint8_t self_master_eligible;
  uint32_t max_seen_term = g_cluster_context.state.term;
  uint8_t self_is_master = (g_cluster_context.state.role == CLUSTER_ROLE_MASTER) ? 1U : 0U;
  uint32_t current_master_node_id = g_cluster_context.state.master_node_id;
  uint32_t remembered_master_node_id = g_cluster_context.last_remote_master_node_id;
  uint32_t promotion_delay_ms = (g_cluster_context.last_remote_master_seen_ms != 0U)
                                ? CLUSTER_SERVICE_FAILOVER_PROMOTION_DELAY_MS
                                : CLUSTER_SERVICE_STARTUP_PROMOTION_DELAY_MS;
  uint8_t next_has_master = g_cluster_context.state.has_master;
  uint32_t next_master_node_id = g_cluster_context.state.master_node_id;
  cluster_role_t next_role = g_cluster_context.state.role;

  g_cluster_context.state.has_master = 0U;
  g_cluster_context.state.master_node_id = 0U;

  if ((config->cluster_enabled == 0U) || (g_cluster_context.state.join_mode == CLUSTER_JOIN_DISABLED))
  {
    g_cluster_context.state.has_master = 0U;
    g_cluster_context.state.master_node_id = 0U;
    g_cluster_context.state.role = CLUSTER_ROLE_STANDALONE;
    return;
  }

  self_eligible = 1U;
  self_master_eligible = g_cluster_context.state.master_eligible;

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
          ((node->term == best_term) && (node->node_id < best_node_id)))
      {
        best_node_id = node->node_id;
        best_term = node->term;
        found_master_candidate = 1U;
      }
    }

    found_any_candidate = 1U;
  }

  if ((self_eligible != 0U) && (self_is_master != 0U) && (self_master_eligible != 0U))
  {
    if ((found_master_candidate == 0U) ||
        (g_cluster_context.state.term > best_term) ||
        ((g_cluster_context.state.term == best_term) &&
         (g_cluster_context.state.self_node_id < best_node_id)))
    {
      best_node_id = g_cluster_context.state.self_node_id;
      best_term = g_cluster_context.state.term;
      found_master_candidate = 1U;
    }
  }

  if (found_master_candidate == 0U)
  {
    if ((self_eligible != 0U) && ((self_master_eligible != 0U) || (found_any_candidate == 0U)))
    {
      best_node_id = g_cluster_context.state.self_node_id;
      found_any_candidate = 1U;
    }

    for (index = 0U; index < CLUSTER_SERVICE_NODE_CAPACITY; ++index)
    {
      const cluster_node_t *node = &g_cluster_context.nodes[index];

      if (!cluster_service_node_is_eligible(node, &g_cluster_context.state))
      {
        continue;
      }

      if ((self_master_eligible == 0U) && (node->node_id == g_cluster_context.state.self_node_id))
      {
        continue;
      }

      if ((best_node_id == 0U) ||
          (node->node_id < best_node_id))
      {
        best_node_id = node->node_id;
      }
    }
  }

  if ((self_eligible == 0U) && (found_any_candidate == 0U) && (best_node_id == 0U))
  {
    if ((remembered_master_node_id != 0U) &&
        (remembered_master_node_id != g_cluster_context.state.self_node_id) &&
        ((int32_t)(now_ms - g_cluster_context.last_remote_master_seen_ms) <
         (int32_t)remembered_master_grace_ms))
    {
      g_cluster_context.state.has_master = 1U;
      g_cluster_context.state.master_node_id = remembered_master_node_id;
      g_cluster_context.state.role = CLUSTER_ROLE_MEMBER;
      return;
    }
    g_cluster_context.state.has_master = 0U;
    g_cluster_context.state.master_node_id = 0U;
    g_cluster_context.state.role = CLUSTER_ROLE_STANDALONE;
    return;
  }

  if ((current_master_node_id == 0U) &&
      (remembered_master_node_id != 0U) &&
      (remembered_master_node_id != g_cluster_context.state.self_node_id) &&
      ((int32_t)(now_ms - g_cluster_context.last_remote_master_seen_ms) <
       (int32_t)remembered_master_grace_ms))
  {
    current_master_node_id = remembered_master_node_id;
  }

  if ((found_master_candidate == 0U) &&
      (best_node_id == 0U) &&
      (current_master_node_id != 0U) &&
      (current_master_node_id != g_cluster_context.state.self_node_id))
  {
    g_cluster_context.state.has_master = 1U;
    g_cluster_context.state.master_node_id = current_master_node_id;
    g_cluster_context.state.role = CLUSTER_ROLE_MEMBER;
    return;
  }

  if ((found_master_candidate == 0U) &&
      (best_node_id == g_cluster_context.state.self_node_id) &&
      (self_is_master == 0U))
  {
    if (g_cluster_context.promotion_candidate_node_id != best_node_id)
    {
      g_cluster_context.promotion_candidate_node_id = best_node_id;
      g_cluster_context.promotion_candidate_since_ms = now_ms;
    }

    if ((int32_t)(now_ms - g_cluster_context.promotion_candidate_since_ms) <
        (int32_t)promotion_delay_ms)
    {
      g_cluster_context.state.has_master = (current_master_node_id != 0U) ? 1U : 0U;
      g_cluster_context.state.master_node_id = current_master_node_id;
      g_cluster_context.state.role = (current_master_node_id != 0U) ? CLUSTER_ROLE_MEMBER : CLUSTER_ROLE_STANDALONE;
      return;
    }
  }
  else
  {
    g_cluster_context.promotion_candidate_node_id = 0U;
    g_cluster_context.promotion_candidate_since_ms = 0U;
  }

  if ((current_master_node_id != 0U) &&
      (best_node_id != 0U) &&
      (best_node_id != current_master_node_id))
  {
    if (g_cluster_context.pending_master_node_id != best_node_id)
    {
      g_cluster_context.pending_master_node_id = best_node_id;
      g_cluster_context.pending_master_since_ms = now_ms;
      best_node_id = current_master_node_id;
    }
    else if ((int32_t)(now_ms - g_cluster_context.pending_master_since_ms) <
             (int32_t)CLUSTER_SERVICE_MASTER_SWITCH_DELAY_MS)
    {
      best_node_id = current_master_node_id;
    }
  }
  else
  {
    g_cluster_context.pending_master_node_id = 0U;
    g_cluster_context.pending_master_since_ms = 0U;
  }

  next_has_master = 1U;
  next_master_node_id = best_node_id;
  if (best_node_id == g_cluster_context.state.self_node_id)
  {
    g_cluster_context.promotion_candidate_node_id = 0U;
    g_cluster_context.promotion_candidate_since_ms = 0U;
    if (g_cluster_context.state.role != CLUSTER_ROLE_MASTER)
    {
      g_cluster_context.state.term = max_seen_term + 1U;
    }
    next_role = CLUSTER_ROLE_MASTER;
  }
  else
  {
    if (best_term > g_cluster_context.state.term)
    {
      g_cluster_context.state.term = best_term;
    }
    next_role = CLUSTER_ROLE_MEMBER;
  }

  g_cluster_context.state.has_master = next_has_master;
  g_cluster_context.state.master_node_id = next_master_node_id;
  g_cluster_context.state.role = next_role;
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
  g_cluster_context.state.master_eligible = 1U;
  g_cluster_context.state.health_score = 1000U;
  g_cluster_context.state.health_lateness_ms = 0U;
  g_cluster_context.state.health_bad_streak = 0U;
  g_cluster_context.pending_master_node_id = 0U;
  g_cluster_context.pending_master_since_ms = 0U;
  g_cluster_context.promotion_candidate_node_id = 0U;
  g_cluster_context.promotion_candidate_since_ms = 0U;
  g_cluster_context.last_remote_master_seen_ms = 0U;
  g_cluster_context.last_remote_master_node_id = 0U;
  g_cluster_context.health_bad_since_ms = 0U;
  g_cluster_context.health_good_since_ms = 0U;
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
  g_cluster_context.state.master_eligible = 1U;
  g_cluster_context.state.health_score = 1000U;
  g_cluster_context.state.health_lateness_ms = 0U;
  g_cluster_context.state.health_bad_streak = 0U;
  memcpy(g_cluster_context.state.cluster_name, config->cluster_name, sizeof(g_cluster_context.state.cluster_name));
  g_cluster_context.local_auth_tag = device_config_password_tag(config);
  g_cluster_context.local_load_score = 0U;
  g_cluster_context.pending_master_node_id = 0U;
  g_cluster_context.pending_master_since_ms = 0U;
  g_cluster_context.promotion_candidate_node_id = 0U;
  g_cluster_context.promotion_candidate_since_ms = 0U;
  g_cluster_context.last_remote_master_seen_ms = 0U;
  g_cluster_context.last_remote_master_node_id = 0U;
  g_cluster_context.health_bad_since_ms = 0U;
  g_cluster_context.health_good_since_ms = 0U;
  memset(g_cluster_context.banned_node_ids, 0, sizeof(g_cluster_context.banned_node_ids));
  (void)xTaskCreate(cluster_service_health_task,
                    "ClusterHealth",
                    256U,
                    NULL,
                    tskIDLE_PRIORITY + 1U,
                    &g_cluster_context.health_task_handle);
  g_cluster_context.initialized = 1U;
}

void cluster_service_poll(void)
{
  cluster_role_t previous_role;
  uint32_t previous_master_node_id;

  cluster_service_init();
  previous_role = g_cluster_context.state.role;
  previous_master_node_id = g_cluster_context.state.master_node_id;
  cluster_service_drop_stale_nodes(HAL_GetTick());
  cluster_service_recount_nodes();
  cluster_service_run_election();
  if ((previous_role != g_cluster_context.state.role) ||
      (previous_master_node_id != g_cluster_context.state.master_node_id))
  {
    cluster_service_mark_backend_dirty();
  }
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
    taskENTER_CRITICAL();
    *state = g_cluster_context.state;
    taskEXIT_CRITICAL();
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
      cluster_service_mark_backend_dirty();
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
  target->online = 1U;
  target->device_state = cluster_service_device_state_from_relay_mask(target->relay_mask);
  if (target->role == CLUSTER_ROLE_MASTER)
  {
    g_cluster_context.last_remote_master_seen_ms = target->last_seen_ms;
    g_cluster_context.last_remote_master_node_id = target->node_id;
  }
  cluster_service_mark_backend_dirty();
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

  if ((node == NULL) || (node->online == 0U))
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
      node->device_state = cluster_service_device_state_from_relay_mask(node->relay_mask);
      cluster_service_mark_backend_dirty();
      return 1U;

    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
      node->relay_mask = command->mask;
      node->device_state = cluster_service_device_state_from_relay_mask(node->relay_mask);
      cluster_service_mark_backend_dirty();
      return 1U;

    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      node->relay_mask = (command->value != 0U) ? 0xFFFFUL : 0U;
      node->device_state = cluster_service_device_state_from_relay_mask(node->relay_mask);
      cluster_service_mark_backend_dirty();
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
