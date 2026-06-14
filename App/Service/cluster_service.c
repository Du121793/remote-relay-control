#include "cluster_service.h"
#include "backend_link_service.h"
#include "mqtt_link_service.h"
#include "../Platform/board_relay.h"

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/netif.h"
#include <string.h>

#include "stm32f7xx_hal.h"

extern struct netif gnetif;

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
#define CLUSTER_SERVICE_RTC_SIGNATURE        0x54494D45UL
#define CLUSTER_SERVICE_RTC_MIN_UNIX         1577836800UL
#define CLUSTER_SERVICE_RTC_MAX_UNIX         4102444799UL
#define CLUSTER_SERVICE_RTC_LSE_TIMEOUT_MS   3000UL

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
  uint32_t software_time_base_unix;
  uint32_t software_time_base_tick;
  cluster_service_state_t state;
  cluster_node_t nodes[CLUSTER_SERVICE_NODE_CAPACITY];
  uint32_t banned_node_ids[CLUSTER_SERVICE_BAN_CAPACITY];
  TaskHandle_t health_task_handle;
} cluster_service_context_t;

static cluster_service_context_t g_cluster_context;
static StaticTask_t g_cluster_health_task_buffer;
static StackType_t g_cluster_health_task_stack[1024];

static void cluster_service_health_task(void *argument);
static void cluster_service_ensure_health_task(void);
static uint32_t cluster_service_health_score_from_lateness(uint32_t lateness_ms);

static uint32_t cluster_service_build_gateway_epoch(uint32_t unix_time)
{
  uint32_t epoch = g_cluster_context.state.self_node_id ^
                   unix_time ^
                   HAL_GetTick() ^
                   (g_cluster_context.state.term * 2654435761UL);

  return (epoch != 0U) ? epoch : 1U;
}

static uint8_t cluster_service_bcd_to_bin(uint32_t value)
{
  return (uint8_t)(((value >> 4U) * 10U) + (value & 0x0FU));
}

static uint32_t cluster_service_bin_to_bcd(uint32_t value)
{
  return ((value / 10U) << 4U) | (value % 10U);
}

static int64_t cluster_service_days_from_civil(int32_t year,
                                               uint32_t month,
                                               uint32_t day)
{
  int32_t era;
  uint32_t year_of_era;
  uint32_t day_of_year;
  uint32_t day_of_era;

  year -= (month <= 2U) ? 1 : 0;
  era = (year >= 0) ? (year / 400) : ((year - 399) / 400);
  year_of_era = (uint32_t)(year - (era * 400));
  day_of_year = ((153U * (month + ((month > 2U) ? (uint32_t)-3 : 9U))) + 2U) / 5U;
  day_of_year += day - 1U;
  day_of_era = (year_of_era * 365U) + (year_of_era / 4U) -
               (year_of_era / 100U) + day_of_year;
  return ((int64_t)era * 146097LL) + (int64_t)day_of_era - 719468LL;
}

static void cluster_service_civil_from_days(int64_t days,
                                            int32_t *year,
                                            uint32_t *month,
                                            uint32_t *day)
{
  int64_t era;
  uint32_t day_of_era;
  uint32_t year_of_era;
  int32_t calculated_year;
  uint32_t day_of_year;
  uint32_t month_prime;

  days += 719468LL;
  era = (days >= 0) ? (days / 146097LL) : ((days - 146096LL) / 146097LL);
  day_of_era = (uint32_t)(days - (era * 146097LL));
  year_of_era = (day_of_era - (day_of_era / 1460U) +
                 (day_of_era / 36524U) - (day_of_era / 146096U)) / 365U;
  calculated_year = (int32_t)year_of_era + ((int32_t)era * 400);
  day_of_year = day_of_era - ((365U * year_of_era) +
                              (year_of_era / 4U) -
                              (year_of_era / 100U));
  month_prime = ((5U * day_of_year) + 2U) / 153U;
  *day = day_of_year - (((153U * month_prime) + 2U) / 5U) + 1U;
  *month = month_prime + ((month_prime < 10U) ? 3U : (uint32_t)-9);
  calculated_year += (*month <= 2U) ? 1 : 0;
  *year = calculated_year;
}

static uint8_t cluster_service_rtc_read(uint32_t *unix_time)
{
  uint32_t time_register;
  uint32_t date_register;
  uint32_t year;
  uint32_t month;
  uint32_t day;
  uint32_t hour;
  uint32_t minute;
  uint32_t second;
  int64_t days;
  int64_t value;

  if ((unix_time == NULL) ||
      (g_cluster_context.state.rtc_lse_ready == 0U) ||
      (RTC->BKP16R != CLUSTER_SERVICE_RTC_SIGNATURE))
  {
    return 0U;
  }

  time_register = RTC->TR;
  date_register = RTC->DR;
  year = 2000U + cluster_service_bcd_to_bin((date_register >> 16U) & 0xFFU);
  month = cluster_service_bcd_to_bin((date_register >> 8U) & 0x1FU);
  day = cluster_service_bcd_to_bin(date_register & 0x3FU);
  hour = cluster_service_bcd_to_bin((time_register >> 16U) & 0x3FU);
  minute = cluster_service_bcd_to_bin((time_register >> 8U) & 0x7FU);
  second = cluster_service_bcd_to_bin(time_register & 0x7FU);
  if ((year < 2020U) || (year > 2099U) ||
      (month == 0U) || (month > 12U) ||
      (day == 0U) || (day > 31U) ||
      (hour > 23U) || (minute > 59U) || (second > 59U))
  {
    return 0U;
  }

  days = cluster_service_days_from_civil((int32_t)year, month, day);
  value = (days * 86400LL) + ((int64_t)hour * 3600LL) +
          ((int64_t)minute * 60LL) + (int64_t)second;
  if ((value < (int64_t)CLUSTER_SERVICE_RTC_MIN_UNIX) ||
      (value > (int64_t)CLUSTER_SERVICE_RTC_MAX_UNIX))
  {
    return 0U;
  }

  *unix_time = (uint32_t)value;
  return 1U;
}

static uint8_t cluster_service_rtc_write(uint32_t unix_time)
{
  int64_t days;
  uint32_t seconds_of_day;
  int32_t year;
  uint32_t month;
  uint32_t day;
  uint32_t hour;
  uint32_t minute;
  uint32_t second;
  uint32_t weekday;
  uint32_t start;

  if ((g_cluster_context.state.rtc_lse_ready == 0U) ||
      (unix_time < CLUSTER_SERVICE_RTC_MIN_UNIX) ||
      (unix_time > CLUSTER_SERVICE_RTC_MAX_UNIX))
  {
    return 0U;
  }

  days = (int64_t)(unix_time / 86400UL);
  seconds_of_day = unix_time % 86400UL;
  cluster_service_civil_from_days(days, &year, &month, &day);
  hour = seconds_of_day / 3600UL;
  minute = (seconds_of_day % 3600UL) / 60UL;
  second = seconds_of_day % 60UL;
  weekday = (uint32_t)(((days + 3LL) % 7LL) + 1LL);

  RTC->WPR = 0xCAU;
  RTC->WPR = 0x53U;
  RTC->ISR |= RTC_ISR_INIT;
  start = HAL_GetTick();
  while ((RTC->ISR & RTC_ISR_INITF) == 0U)
  {
    if ((HAL_GetTick() - start) >= 100U)
    {
      RTC->WPR = 0xFFU;
      return 0U;
    }
  }
  RTC->CR &= ~RTC_CR_FMT;
  RTC->PRER = (127UL << 16U) | 255UL;
  RTC->TR = (cluster_service_bin_to_bcd(hour) << 16U) |
            (cluster_service_bin_to_bcd(minute) << 8U) |
            cluster_service_bin_to_bcd(second);
  RTC->DR = (cluster_service_bin_to_bcd((uint32_t)year - 2000U) << 16U) |
            (cluster_service_bin_to_bcd(month) << 8U) |
            (weekday << 13U) |
            cluster_service_bin_to_bcd(day);
  RTC->ISR &= ~RTC_ISR_INIT;
  RTC->WPR = 0xFFU;
  RTC->BKP16R = CLUSTER_SERVICE_RTC_SIGNATURE;
  return 1U;
}

static void cluster_service_rtc_init(void)
{
  uint32_t fault_backup[16];
  uint32_t index;
  uint32_t start;
  uint8_t reset_backup_domain = 0U;

  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) != RCC_BDCR_RTCSEL_0)
  {
    for (index = 0U; index < 16U; ++index)
    {
      fault_backup[index] = (&RTC->BKP0R)[index];
    }
    __HAL_RCC_BACKUPRESET_FORCE();
    __HAL_RCC_BACKUPRESET_RELEASE();
    reset_backup_domain = 1U;
  }

  RCC->BDCR |= RCC_BDCR_LSEON;
  start = HAL_GetTick();
  while ((RCC->BDCR & RCC_BDCR_LSERDY) == 0U)
  {
    if ((HAL_GetTick() - start) >= CLUSTER_SERVICE_RTC_LSE_TIMEOUT_MS)
    {
      if (reset_backup_domain != 0U)
      {
        for (index = 0U; index < 16U; ++index)
        {
          (&RTC->BKP0R)[index] = fault_backup[index];
        }
      }
      return;
    }
  }

  RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) |
              RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCEN;
  if (reset_backup_domain != 0U)
  {
    for (index = 0U; index < 16U; ++index)
    {
      (&RTC->BKP0R)[index] = fault_backup[index];
    }
  }
  g_cluster_context.state.rtc_lse_ready = 1U;
}

static void cluster_service_mark_backend_dirty(void)
{
  backend_link_service_mark_status_dirty();
  mqtt_link_service_mark_status_dirty();
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
  TickType_t last_wake_tick = xTaskGetTickCount();
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

    vTaskDelayUntil(&last_wake_tick, period_ticks);
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
  uint32_t restored_unix_time;

  if (g_cluster_context.initialized != 0U)
  {
    return;
  }

  device_config_init();
  config = device_config_get();
  memset(&g_cluster_context, 0, sizeof(g_cluster_context));
  cluster_service_rtc_init();
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
  if (cluster_service_rtc_read(&restored_unix_time) != 0U)
  {
    g_cluster_context.state.time_valid = 1U;
    g_cluster_context.state.time_source = CLUSTER_TIME_SOURCE_RTC;
    g_cluster_context.state.time_quality = CLUSTER_TIME_QUALITY_RTC_HOLDOVER;
    g_cluster_context.state.unix_time = restored_unix_time;
    g_cluster_context.state.time_seq = 1U;
    g_cluster_context.state.gateway_epoch =
      cluster_service_build_gateway_epoch(restored_unix_time);
    g_cluster_context.state.gateway_node_id =
      g_cluster_context.state.self_node_id;
    g_cluster_context.software_time_base_unix = restored_unix_time;
    g_cluster_context.software_time_base_tick = HAL_GetTick();
  }
  memset(g_cluster_context.banned_node_ids, 0, sizeof(g_cluster_context.banned_node_ids));
  g_cluster_context.initialized = 1U;
}

static void cluster_service_ensure_health_task(void)
{
  if (g_cluster_context.health_task_handle != NULL)
  {
    return;
  }
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
  {
    return;
  }
  g_cluster_context.health_task_handle =
    xTaskCreateStatic(cluster_service_health_task,
                      "ClusterHealth",
                      1024U,
                      NULL,
                      tskIDLE_PRIORITY + 1U,
                      g_cluster_health_task_stack,
                      &g_cluster_health_task_buffer);
}

void cluster_service_poll(void)
{
  cluster_role_t previous_role;
  uint32_t previous_master_node_id;

  cluster_service_init();
  if (netif_is_link_up(&gnetif) != 0)
  {
    cluster_service_ensure_health_task();
  }
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
  TaskHandle_t health_task = g_cluster_context.health_task_handle;
  uint8_t resume_health_task = 0U;

  if ((health_task != NULL) &&
      (health_task != xTaskGetCurrentTaskHandle()) &&
      (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
  {
    vTaskSuspend(health_task);
    resume_health_task = 1U;
  }

  taskENTER_CRITICAL();
  memset(&g_cluster_context, 0, sizeof(g_cluster_context));
  g_cluster_context.health_task_handle = health_task;
  taskEXIT_CRITICAL();
  cluster_service_init();

  if (resume_health_task != 0U)
  {
    vTaskResume(health_task);
  }
}

void cluster_service_get_state(cluster_service_state_t *state)
{
  uint8_t time_valid;
  uint32_t unix_time;

  cluster_service_init();

  if (state != NULL)
  {
    unix_time = cluster_service_get_unix_time(&time_valid);
    taskENTER_CRITICAL();
    g_cluster_context.state.time_valid = time_valid;
    g_cluster_context.state.unix_time = unix_time;
    *state = g_cluster_context.state;
    taskEXIT_CRITICAL();
  }
}

uint32_t cluster_service_get_unix_time(uint8_t *valid)
{
  uint32_t unix_time;

  cluster_service_init();
  if (cluster_service_rtc_read(&unix_time) != 0U)
  {
    if (valid != NULL)
    {
      *valid = 1U;
    }
    return unix_time;
  }
  if (g_cluster_context.software_time_base_unix != 0U)
  {
    unix_time = g_cluster_context.software_time_base_unix +
                ((HAL_GetTick() - g_cluster_context.software_time_base_tick) / 1000UL);
    if (valid != NULL)
    {
      *valid = 1U;
    }
    return unix_time;
  }

  if (valid != NULL)
  {
    *valid = 0U;
  }
  return 0U;
}

uint8_t cluster_service_set_unix_time(uint32_t unix_time, uint8_t source)
{
  uint8_t previous_valid;
  uint8_t previous_source;
  uint32_t previous_time;
  int64_t offset;

  cluster_service_init();
  if ((unix_time < CLUSTER_SERVICE_RTC_MIN_UNIX) ||
      (unix_time > CLUSTER_SERVICE_RTC_MAX_UNIX))
  {
    return 0U;
  }

  previous_time = cluster_service_get_unix_time(&previous_valid);
  previous_source = g_cluster_context.state.time_source;
  offset = (previous_valid != 0U)
           ? ((int64_t)unix_time - (int64_t)previous_time)
           : 0LL;
  if (offset > 2147483647LL)
  {
    offset = 2147483647LL;
  }
  else if (offset < -2147483647LL)
  {
    offset = -2147483647LL;
  }

  (void)cluster_service_rtc_write(unix_time);
  taskENTER_CRITICAL();
  g_cluster_context.software_time_base_unix = unix_time;
  g_cluster_context.software_time_base_tick = HAL_GetTick();
  g_cluster_context.state.time_valid = 1U;
  g_cluster_context.state.time_source = source;
  if (source == CLUSTER_TIME_SOURCE_MQTT)
  {
    if ((g_cluster_context.state.gateway_epoch == 0U) ||
        (g_cluster_context.state.gateway_node_id != g_cluster_context.state.self_node_id) ||
        (previous_source != CLUSTER_TIME_SOURCE_MQTT))
    {
      g_cluster_context.state.gateway_epoch =
        cluster_service_build_gateway_epoch(unix_time);
      g_cluster_context.state.time_seq = 0U;
    }
    g_cluster_context.state.gateway_node_id = g_cluster_context.state.self_node_id;
    g_cluster_context.state.time_quality = CLUSTER_TIME_QUALITY_BROKER;
    g_cluster_context.state.time_seq++;
    if (g_cluster_context.state.time_seq == 0U)
    {
      g_cluster_context.state.time_seq = 1U;
    }
  }
  else if (source == CLUSTER_TIME_SOURCE_MASTER)
  {
    g_cluster_context.state.time_quality = CLUSTER_TIME_QUALITY_CLUSTER;
  }
  else if (source == CLUSTER_TIME_SOURCE_RTC)
  {
    g_cluster_context.state.time_quality = CLUSTER_TIME_QUALITY_RTC_HOLDOVER;
  }
  g_cluster_context.state.unix_time = unix_time;
  g_cluster_context.state.time_sync_count++;
  g_cluster_context.state.last_time_offset_seconds = (int32_t)offset;
  taskEXIT_CRITICAL();
  cluster_service_mark_backend_dirty();
  return 1U;
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
    if ((target->time_valid != 0U) &&
        (target->unix_time >= CLUSTER_SERVICE_RTC_MIN_UNIX) &&
        (cluster_service_node_is_eligible(target, &g_cluster_context.state) != 0U))
    {
      uint8_t local_valid;
      uint32_t local_time = cluster_service_get_unix_time(&local_valid);
      int64_t difference = (int64_t)target->unix_time - (int64_t)local_time;
      uint8_t authority_newer = 0U;
      uint8_t forward_correction = 0U;

      if (target->gateway_epoch != 0U)
      {
        if (target->gateway_epoch == g_cluster_context.state.gateway_epoch)
        {
          authority_newer =
            (target->time_seq > g_cluster_context.state.time_seq) ? 1U : 0U;
        }
        else if ((local_valid == 0U) || (difference >= -2LL))
        {
          authority_newer = 1U;
        }
      }
      if ((local_valid != 0U) && (difference > 2LL))
      {
        forward_correction = 1U;
      }

      if ((local_valid == 0U) ||
          (authority_newer != 0U) ||
          (forward_correction != 0U) ||
          ((target->gateway_epoch == 0U) &&
           ((difference > 2LL) || (difference < -2LL))))
      {
        if (cluster_service_set_unix_time(target->unix_time,
                                          CLUSTER_TIME_SOURCE_MASTER) != 0U)
        {
          taskENTER_CRITICAL();
          g_cluster_context.state.time_quality =
            (target->time_quality != CLUSTER_TIME_QUALITY_INVALID)
            ? target->time_quality
            : CLUSTER_TIME_QUALITY_CLUSTER;
          g_cluster_context.state.time_seq = target->time_seq;
          g_cluster_context.state.gateway_epoch = target->gateway_epoch;
          g_cluster_context.state.gateway_node_id =
            (target->gateway_node_id != 0U)
            ? target->gateway_node_id
            : target->node_id;
          taskEXIT_CRITICAL();
        }
      }
    }
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
      if ((command->channel == 0U) ||
          (command->channel > BOARD_RELAY_CHANNEL_COUNT))
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
