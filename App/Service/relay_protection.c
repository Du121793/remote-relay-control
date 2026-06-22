#include "relay_protection.h"
#include "board_relay.h"
#include "control_queue.h"
#include "../Config/device_config.h"
#include "../Domain/ct_measurement.h"

#include <stdio.h>
#include <string.h>

extern RTC_HandleTypeDef hrtc;
extern UART_HandleTypeDef huart3;

/* DEBUG: minimal UART log for stuck detection troubleshooting */
static void rp_debug_uart(const char *msg)
{
  if (huart3.Instance != NULL)
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)msg, (uint16_t)strlen(msg), 10U);
}

/* ── Internal per-channel state ──────────────────────────────────── */
typedef struct
{
  uint8_t  phase;
  uint8_t  fault_level;
  uint8_t  fault_reason;
  uint8_t  learning_complete;
  uint8_t  protection_enabled;
  uint8_t  device_strategy;
  uint8_t  ct_degraded;
  uint8_t  adc_saturated;
  uint8_t  tripped;
  uint8_t  fault_locked;
  uint8_t  learning_samples_collected;
  uint8_t  rms_stable_index;
  uint8_t  pad[1];

  /* Timing */
  uint32_t relay_on_since_ms;
  uint32_t stable_since_ms;
  uint32_t phase_enter_ms;
  uint32_t cooldown_until_ms;

  /* Current snapshot */
  uint32_t current_milliamps;

  /* Startup tracking */
  uint32_t startup_peak_ma;
  uint32_t startup_duration_ms;

  /* Stability detection: ring buffer of last 10 RMS values */
  uint32_t rms_history[10];

  /* Trip delay tracking */
  uint32_t above_warning_since_ms;
  uint32_t above_slow_since_ms;
  uint32_t above_fast_since_ms;
  uint32_t below_undercurrent_since_ms;
  uint32_t adc_saturation_since_ms;

  /* Stuck detection */
  uint32_t off_since_ms;
  uint32_t stuck_check_pending;
  uint32_t stuck_above_count;  /* consecutive cycles current > noise */

  /* Learning data collection */
  uint32_t learning_baselines[5];
  uint32_t learning_peaks[5];
  uint32_t learning_durations[5];

  /* Established baseline (never overwritten after learning) */
  uint32_t initial_health_baseline_ma;
  uint32_t initial_normal_low_ma;
  uint32_t initial_normal_high_ma;
  uint32_t initial_startup_peak_ma;
  uint32_t initial_startup_duration_ms;
  uint32_t off_rms_noise_ma;
  uint32_t off_dc_bias_mv;

  /* Aging */
  uint32_t aging_offset_ma;
  uint32_t normal_low_ma;
  uint32_t normal_high_ma;

  /* Counters */
  uint32_t trip_count;
  uint32_t startup_fail_count;

  /* Auto-retry */
  uint8_t  retry_pending;
  uint8_t  retry_count;
  uint16_t pad2;
  uint32_t retry_after_ms;
} rp_ch_t;

/* ── Global user config (Phase §14) ──────────────────────────────── */
static relay_protection_user_config_t g_ucfg = {
  .cooldown_seconds = RELAY_PROTECTION_DEFAULT_COOLDOWN_SEC,
  .retry_delay_seconds = RELAY_PROTECTION_DEFAULT_RETRY_DELAY_SEC,
  .max_retry_count = RELAY_PROTECTION_DEFAULT_MAX_RETRY,
  .boot_delay_seconds = RELAY_PROTECTION_DEFAULT_BOOT_DELAY_SEC,
  .learning_samples = RELAY_PROTECTION_DEFAULT_LEARNING_SAMPLES,
  .aging_update_hours = RELAY_PROTECTION_DEFAULT_AGING_HOURS,
  .aging_cap_percent = RELAY_PROTECTION_DEFAULT_AGING_CAP_PERCENT,
  .stall_timeout_multiplier_x10 = RELAY_PROTECTION_DEFAULT_STALL_MULTIPLIER,
};

/* ── Global state ────────────────────────────────────────────────── */
static struct
{
  uint8_t  init_done;
  uint8_t  boot_safety_active;
  uint8_t  fault_locked_any;
  uint8_t  pad;
  uint32_t boot_delay_until_ms;
  uint32_t restart_count;
  uint32_t last_boot_ms;
  uint32_t fault_lock_mask; /* bit N = channel N+1 hardware-locked */
} g_rp;

static rp_ch_t g_ch[RELAY_PROTECTION_CHANNEL_COUNT];

/* ── Forward declarations ────────────────────────────────────────── */
static void rp_assess_startup(rp_ch_t *c, uint32_t now);
static void rp_assess_running(rp_ch_t *c, uint32_t now);
static void rp_trip(rp_ch_t *c, uint8_t channel, uint8_t fault_level, uint8_t fault_reason, uint32_t now);
static uint8_t rp_is_stable(const rp_ch_t *c);
static void rp_update_reference(rp_ch_t *c);

/* ── Helpers ─────────────────────────────────────────────────────── */
static uint32_t rp_abs_diff(uint32_t a, uint32_t b)
{
  return (a > b) ? (a - b) : (b - a);
}

static uint8_t rp_is_lethal_fault(uint8_t fault_level)
{
  return (fault_level >= RELAY_PROTECTION_FAULT_MANUAL_RESET) ? 1U : 0U;
}

/* ── Init (Phase B: boot safety) ─────────────────────────────────── */
void relay_protection_init(void)
{
  const device_config_t *cfg = device_config_get();
  uint32_t now = HAL_GetTick();

  if (g_rp.init_done) return;
  memset(&g_rp, 0, sizeof(g_rp));
  memset(g_ch, 0, sizeof(g_ch));

  g_rp.boot_safety_active = 1U;
  g_rp.boot_delay_until_ms = now + (g_ucfg.boot_delay_seconds * 1000U);
  g_rp.last_boot_ms = now;

  /* Restart detection: check RTC backup register for recent boot */
  /* If last boot was < 30s ago, increment restart counter */
  /* Simplified: use a static variable in RTC backup domain */
  {
    uint32_t prev_boot = 0U;
    /* Read RTC backup register for previous boot tick */
    if (__HAL_RCC_RTC_IS_CLK_ENABLED())
    {
      prev_boot = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0);
    }
    if (prev_boot != 0U && (now - prev_boot) < 30000U)
    {
      g_rp.restart_count++;
    }
    else
    {
      g_rp.restart_count = 0U;
    }
    /* Store current boot time */
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, now);
  }

  /* Repeated restart → fault lock all channels */
  if (g_rp.restart_count >= 3U)
  {
    g_rp.fault_locked_any = 1U;
    g_rp.fault_lock_mask = 0xFFFFU;
    for (uint8_t i = 0U; i < RELAY_PROTECTION_CHANNEL_COUNT; i++)
    {
      g_ch[i].phase = RELAY_PROTECTION_PHASE_FAULT;
      g_ch[i].fault_level = RELAY_PROTECTION_FAULT_HARDWARE;
      g_ch[i].fault_locked = 1U;
    }
    /* Force all relays OFF */
    for (uint8_t i = 1U; i <= 16U; i++)
      (void)board_relay_set(i, GPIO_PIN_RESET);
  }

  /* Load per-channel config */
  for (uint8_t i = 0U; i < RELAY_PROTECTION_CHANNEL_COUNT; i++)
  {
    g_ch[i].protection_enabled = cfg->current[i].protection_enabled;
    g_ch[i].device_strategy = RELAY_PROTECTION_STRATEGY_SAFETY;
    g_ch[i].phase = RELAY_PROTECTION_PHASE_OFF;
  }

  /* Boot safety: force all relays OFF immediately.
     boot_default_on / remember_state become pending requests
     that are only applied after boot delay expires. */
  for (uint8_t i = 1U; i <= 16U; i++)
    (void)board_relay_set(i, GPIO_PIN_RESET);

  g_rp.init_done = 1U;
}

/* ── Boot safety check ───────────────────────────────────────────── */
uint8_t relay_protection_boot_safety_active(uint32_t now_ms)
{
  if (!g_rp.boot_safety_active) return 0U;
  if ((int32_t)(now_ms - g_rp.boot_delay_until_ms) >= 0)
  {
    g_rp.boot_safety_active = 0U;
    return 0U;
  }
  return 1U;
}

/* ── Fault lock mask ─────────────────────────────────────────────── */
uint32_t relay_protection_fault_lock_mask(void)
{
  uint32_t mask = g_rp.fault_lock_mask;
  for (uint8_t i = 0U; i < RELAY_PROTECTION_CHANNEL_COUNT; i++)
  {
    if (g_ch[i].fault_locked)
      mask |= (1UL << i);
  }
  return mask;
}

/* ── Force stop (Phase A) ────────────────────────────────────────── */
void relay_protection_force_stop(uint8_t channel)
{
  if (channel == 0U || channel > 16U) return;
  uint8_t idx = channel - 1U;
  g_ch[idx].fault_reason = RELAY_PROTECTION_REASON_FORCE_STOP;
  g_ch[idx].fault_level = RELAY_PROTECTION_FAULT_HARDWARE;
  g_ch[idx].fault_locked = 1U;
  g_ch[idx].cooldown_until_ms = HAL_GetTick() + (g_ucfg.cooldown_seconds * 1000U);
  g_ch[idx].phase = RELAY_PROTECTION_PHASE_COOLDOWN;
  g_ch[idx].tripped = 1U;
  /* Also use the control_queue force-stop for GPIO OFF */
  control_queue_force_stop(channel);
}

/* ── Cooldown check ──────────────────────────────────────────────── */
uint8_t relay_protection_is_cooldown(uint8_t channel, uint32_t now_ms)
{
  if (channel == 0U || channel > 16U) return 0U;
  uint8_t idx = channel - 1U;
  return ((int32_t)(now_ms - g_ch[idx].cooldown_until_ms) < 0) ? 1U : 0U;
}

/* ── Notify relay state change ───────────────────────────────────── */
void relay_protection_notify_relay_change(uint8_t channel, uint8_t new_state,
                                          uint32_t now_ms)
{
  if (channel == 0U || channel > 16U) return;
  uint8_t idx = channel - 1U;
  rp_ch_t *c = &g_ch[idx];

  if (new_state != 0U)
  {
    /* Relay turned ON → enter startup phase */
    if (!c->fault_locked && g_rp.boot_safety_active == 0U)
    {
      c->phase = RELAY_PROTECTION_PHASE_STARTUP;
      c->relay_on_since_ms = now_ms;
      c->phase_enter_ms = now_ms;
      c->startup_peak_ma = 0U;
      c->startup_duration_ms = 0U;
      c->stable_since_ms = 0U;
      c->above_warning_since_ms = 0U;
      c->above_slow_since_ms = 0U;
      c->above_fast_since_ms = 0U;
      c->below_undercurrent_since_ms = 0U;
      c->adc_saturation_since_ms = 0U;
      c->stuck_check_pending = 0U;
      c->stuck_above_count = 0U;
      c->retry_pending = 0U;
      memset(c->rms_history, 0, sizeof(c->rms_history));
      c->rms_stable_index = 0U;
    }
  }
  else
  {
    /* Relay turned OFF */
    c->phase = RELAY_PROTECTION_PHASE_OFF;
    c->off_since_ms = now_ms;
    c->stuck_check_pending = 1U;
    {
      char log[48];
      (void)snprintf(log, sizeof(log), "\r\n[STUCK ch%u] OFF detected, start observe\r\n",
                     (unsigned int)channel);
      rp_debug_uart(log);
    }
    c->relay_on_since_ms = 0U;
    c->stable_since_ms = 0U;

    /* Aging update: if was running and no fault, update aging */
    if (c->learning_complete && c->tripped == 0U && c->fault_level == RELAY_PROTECTION_FAULT_NONE)
    {
      /* aging_offset update on shutdown */
      uint32_t ref = c->initial_health_baseline_ma + c->aging_offset_ma;
      if (c->current_milliamps > 0U && ref > 0U)
      {
        uint32_t this_offset = (c->current_milliamps > c->initial_health_baseline_ma)
          ? (c->current_milliamps - c->initial_health_baseline_ma) : 0U;
        /* Only update if not anomalous (>30% above reference) */
        if (c->current_milliamps < (ref + (ref * 3U) / 10U))  /* < ref × 1.3 */
        {
          c->aging_offset_ma = (c->aging_offset_ma * 9U + this_offset) / 10U;
          uint32_t cap = (c->initial_health_baseline_ma * g_ucfg.aging_cap_percent) / 100U;
          if (c->aging_offset_ma > cap) c->aging_offset_ma = cap;
          rp_update_reference(c);
        }
      }
    }
  }
}

/* ── Stability check ─────────────────────────────────────────────── */
static uint8_t rp_is_stable(const rp_ch_t *c)
{
  /* Need at least 6 samples in ring buffer */
  uint32_t min_val = 0xFFFFFFFFUL, max_val = 0U;
  uint8_t count = 0U;
  for (uint8_t i = 0U; i < 10U; i++)
  {
    if (c->rms_history[i] > 0U)
    {
      count++;
      if (c->rms_history[i] < min_val) min_val = c->rms_history[i];
      if (c->rms_history[i] > max_val) max_val = c->rms_history[i];
    }
  }
  if (count < 6U) return 0U;

  /* RMS variation < ±15% → stable */
  if (max_val == 0U) return 0U;
  uint32_t variation = ((max_val - min_val) * 100U) / max_val;
  return (variation < 15U) ? 1U : 0U;
}

/* ── Update normal_low/normal_high after aging ───────────────────── */
static void rp_update_reference(rp_ch_t *c)
{
  if (!c->learning_complete) return;
  c->normal_low_ma = (c->initial_normal_low_ma * 9U + c->normal_low_ma) / 10U;
  c->normal_high_ma = (c->initial_normal_high_ma * 9U + c->normal_high_ma) / 10U;

  /* Clamp normal_low: not below initial × 0.7 */
  uint32_t low_floor = (c->initial_normal_low_ma * 7U) / 10U;
  if (c->normal_low_ma < low_floor) c->normal_low_ma = low_floor;

  /* Clamp normal_high: not above initial × 1.5 */
  uint32_t high_ceil = c->initial_normal_high_ma + (c->initial_normal_high_ma / 2U);
  if (c->normal_high_ma > high_ceil) c->normal_high_ma = high_ceil;
}

/* ── Trip relay ──────────────────────────────────────────────────── */
static void rp_trip(rp_ch_t *c, uint8_t channel, uint8_t fault_level,
                     uint8_t fault_reason, uint32_t now)
{
  /* If already locked or tripped by a lethal fault, skip duplicate trips.
     This prevents trip_count inflation from repeated assess cycles. */
  if (c->fault_locked) return;
  if (c->tripped && rp_is_lethal_fault(c->fault_level)) return;

  c->tripped = 1U;
  c->fault_level = fault_level;
  c->fault_reason = fault_reason;
  c->trip_count++;

  {
    char log[80];
    (void)snprintf(log, sizeof(log),
        "\r\n[FAULT ch%u] level=%u reason=%u mA=%lu\r\n",
        (unsigned int)channel, (unsigned int)fault_level,
        (unsigned int)fault_reason, (unsigned long)c->current_milliamps);
    rp_debug_uart(log);
  }
  c->cooldown_until_ms = now + (g_ucfg.cooldown_seconds * 1000U);

  if (rp_is_lethal_fault(fault_level))
  {
    c->fault_locked = 1U;
    c->phase = RELAY_PROTECTION_PHASE_FAULT;
    g_rp.fault_lock_mask |= (1UL << (channel - 1U));
    g_rp.fault_locked_any = 1U;
  }

  /* Auto-retry for startup failures only */
  if (fault_level == RELAY_PROTECTION_FAULT_AUTO_RECOVER)
  {
    if (c->retry_count < g_ucfg.max_retry_count)
    {
      c->retry_pending = 1U;
      c->retry_count++;
      c->retry_after_ms = now + (g_ucfg.retry_delay_seconds * 1000U);
    }
    else
    {
      c->fault_locked = 1U;
      c->phase = RELAY_PROTECTION_PHASE_FAULT;
    }
  }

  /* Submit OFF command */
  {
    control_proto_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CONTROL_PROTO_CMD_SET_RELAY;
    cmd.channel = channel;
    cmd.value = 0U;
    cmd.error = CONTROL_PROTO_ERR_NONE;
    (void)control_queue_submit_async(&cmd);
  }
}

/* ── Hard 30A limit (always active, basic safety) ────────────────── */
static uint8_t rp_check_hard_limit(rp_ch_t *c, uint8_t channel, uint32_t now)
{
  if (c->current_milliamps >= RELAY_PROTECTION_HARD_CURRENT_LIMIT_MA)
  {
    rp_trip(c, channel, RELAY_PROTECTION_FAULT_HARDWARE, RELAY_PROTECTION_REASON_HARD_30A, now);
    return 1U;
  }
  return 0U;
}

/* ── ADC saturation check (always active, basic safety) ──────────── */
static uint8_t rp_check_saturation(rp_ch_t *c, uint8_t channel, uint32_t now)
{
  if (c->adc_saturated)
  {
    if (c->adc_saturation_since_ms == 0U)
      c->adc_saturation_since_ms = now;
    /* 2 seconds continuous saturation → fault */
    if ((now - c->adc_saturation_since_ms) >= 2000U)
    {
      rp_trip(c, channel, RELAY_PROTECTION_FAULT_HARDWARE, RELAY_PROTECTION_REASON_ADC_SATURATION, now);
      return 1U;
    }
  }
  else
  {
    c->adc_saturation_since_ms = 0U;
  }
  return 0U;
}

static void rp_assess_startup(rp_ch_t *c, uint32_t now)
{
  if (c->relay_on_since_ms == 0U) return;

  /* Track peak */
  if (c->current_milliamps > c->startup_peak_ma)
    c->startup_peak_ma = c->current_milliamps;

  c->startup_duration_ms = now - c->relay_on_since_ms;

  /* ── Startup peak anomaly check (immediate, don't wait for stability) ── */
  if (c->learning_complete)
  {
    /* Peak anomaly: > history × 1.5 */
    if (c->initial_startup_peak_ma > 0U &&
        c->startup_peak_ma > (c->initial_startup_peak_ma + c->initial_startup_peak_ma / 2U))
    {
      if (c->startup_peak_ma > (RELAY_PROTECTION_HARD_CURRENT_LIMIT_MA * 8U / 10U))
      {
        /* Near 30A — escalate to hardware fault */
        rp_trip(c, (uint8_t)((uint8_t)(c - g_ch) + 1U),
                RELAY_PROTECTION_FAULT_HARDWARE,
                RELAY_PROTECTION_REASON_STARTUP_PEAK, now);
        return;
      }
      else
      {
        /* Trip with manual reset required */
        rp_trip(c, (uint8_t)((uint8_t)(c - g_ch) + 1U),
                RELAY_PROTECTION_FAULT_MANUAL_RESET,
                RELAY_PROTECTION_REASON_STARTUP_PEAK, now);
        return;
      }
    }

    /* Stall protection: startup duration > learned × 2.0 */
    if (c->initial_startup_duration_ms > 0U &&
        c->startup_duration_ms > c->initial_startup_duration_ms * 2U)
    {
      rp_trip(c, (uint8_t)((uint8_t)(c - g_ch) + 1U),
              RELAY_PROTECTION_FAULT_MANUAL_RESET, RELAY_PROTECTION_REASON_STALL, now);
      return;
    }
  }
  else
  {
    /* Unlearned channel: fallback stall timeout (10 s) */
    if (c->startup_duration_ms > RELAY_PROTECTION_STALL_FALLBACK_MS)
    {
      rp_trip(c, (uint8_t)((uint8_t)(c - g_ch) + 1U),
              RELAY_PROTECTION_FAULT_MANUAL_RESET, RELAY_PROTECTION_REASON_STALL, now);
      return;
    }
  }

  /* Check if stable */
  if (rp_is_stable(c))
  {
    if (c->stable_since_ms == 0U)
      c->stable_since_ms = now;

    /* Stable for 3 seconds → transition to running */
    if ((now - c->stable_since_ms) >= 3000U)
    {
      c->phase = RELAY_PROTECTION_PHASE_RUNNING;
      c->phase_enter_ms = now;
      c->startup_duration_ms = now - c->relay_on_since_ms;

      /* Learning: collect sample if valid */
      if (!c->learning_complete && c->protection_enabled)
      {
        if (c->current_milliamps > 0U && c->tripped == 0U && !c->ct_degraded)
        {
          uint8_t n = c->learning_samples_collected;
          if (n < 5U)
          {
            c->learning_baselines[n] = c->current_milliamps;
            c->learning_peaks[n] = c->startup_peak_ma;
            c->learning_durations[n] = c->startup_duration_ms;
            c->learning_samples_collected = n + 1U;

            /* Check if we have enough samples to establish baseline */
            if (c->learning_samples_collected >= g_ucfg.learning_samples)
            {
              /* Verify consistency: baseline variation ≤ ±30%, peak variation ≤ ±50% */
              uint32_t b_min = 0xFFFFFFFFUL, b_max = 0U, b_sum = 0U;
              uint32_t p_min = 0xFFFFFFFFUL, p_max = 0U, p_sum = 0U;
              uint32_t d_max = 0U, d_sum = 0U;
              uint8_t valid = 1U;
              uint8_t i;
              for (i = 0U; i < 5U; i++)
              {
                if (c->learning_baselines[i] < b_min) b_min = c->learning_baselines[i];
                if (c->learning_baselines[i] > b_max) b_max = c->learning_baselines[i];
                b_sum += c->learning_baselines[i];
                if (c->learning_peaks[i] < p_min) p_min = c->learning_peaks[i];
                if (c->learning_peaks[i] > p_max) p_max = c->learning_peaks[i];
                p_sum += c->learning_peaks[i];
                if (c->learning_durations[i] > d_max) d_max = c->learning_durations[i];
                d_sum += c->learning_durations[i];
              }
              if (b_max > 0U && ((b_max - b_min) * 100U / b_max) > 30U) valid = 0U;
              if (p_max > 0U && ((p_max - p_min) * 100U / p_max) > 50U) valid = 0U;

              if (valid)
              {
                /* Phase E: Establish baseline (never overwritten) */
                c->initial_health_baseline_ma = b_sum / 5U;
                c->initial_startup_peak_ma = p_sum / 5U;
                c->initial_startup_duration_ms = (d_max * 12U) / 10U; /* max × 1.2 */

                /* normal_low/normal_high: P5/P95 from baseline samples */
                /* Simplified: use ±20% of baseline */
                c->initial_normal_low_ma = (c->initial_health_baseline_ma * 80U) / 100U;
                c->initial_normal_high_ma = (c->initial_health_baseline_ma * 120U) / 100U;
                c->normal_low_ma = c->initial_normal_low_ma;
                c->normal_high_ma = c->initial_normal_high_ma;
                c->aging_offset_ma = 0U;

                c->learning_complete = 1U;

                /* TODO: mark flash dirty for persistence */
              }
              else
              {
                /* Discard first sample and collect more */
                for (i = 1U; i < 5U; i++)
                  c->learning_baselines[i - 1U] = c->learning_baselines[i];
                c->learning_samples_collected = 4U;
              }
            }
          }
        }
      }

    }
  }
  else
  {
    c->stable_since_ms = 0U;
  }

  /* During startup, only check hard limit (no multi-tier) */
  /* Also check for completely no current after 5s */
  if (c->startup_duration_ms > 5000U && c->current_milliamps == 0U)
  {
    /* Startup failure: no current detected */
    rp_trip(c, (uint8_t)((uint8_t)(c - g_ch) + 1U),
            RELAY_PROTECTION_FAULT_AUTO_RECOVER, RELAY_PROTECTION_REASON_STARTUP_FAIL, now);
  }
}

/* ── Assess running phase (Phase C: multi-tier) ──────────────────── */
static void rp_assess_running(rp_ch_t *c, uint32_t now)
{
  uint32_t ref;
  const device_config_t *cfg = device_config_get();
  uint8_t idx = (uint8_t)(c - g_ch);

  /* Determine reference current */
  if (c->learning_complete)
  {
    ref = c->initial_health_baseline_ma + c->aging_offset_ma;
  }
  else
  {
    /* Use user-configured threshold as reference */
    ref = cfg->current[idx].overcurrent_milliamps;
  }

  if (ref == 0U) ref = RELAY_PROTECTION_DEFAULT_OVERCURRENT_MA;

  if (c->current_milliamps == 0U)
  {
    /* No current: reset all OC timers */
    c->above_fast_since_ms = 0U;
    c->above_slow_since_ms = 0U;
    c->above_warning_since_ms = 0U;

    /* Startup failure: 0mA for 5s → auto-recover */
    if (c->below_undercurrent_since_ms == 0U) c->below_undercurrent_since_ms = now;
    if ((now - c->below_undercurrent_since_ms) >= 5000U)
    {
      rp_trip(c, (uint8_t)(idx + 1U), RELAY_PROTECTION_FAULT_AUTO_RECOVER,
              RELAY_PROTECTION_REASON_STARTUP_FAIL, now);
    }
    return;
  }

  /* Hard 30A limit (always active, handled by rp_check_hard_limit) */
  if (c->current_milliamps >= RELAY_PROTECTION_HARD_CURRENT_LIMIT_MA)
  {
    c->above_fast_since_ms = (c->above_fast_since_ms == 0U) ? now : c->above_fast_since_ms;
    return;
  }

  /* ═══════════════════════════════════════════════════════════════════
     Phase 1: Independent timer tracking
     OC and UC timers updated independently — no cross-interference.
     ═══════════════════════════════════════════════════════════════════ */

  /* ── OC Fast: > ref × 2.0 ──────────────────────────────────── */
  if (c->current_milliamps > ref * 2U)
  {
    if (c->above_fast_since_ms == 0U) c->above_fast_since_ms = now;
  }
  else { c->above_fast_since_ms = 0U; }

  if (c->learning_complete)
  {
    /* ── OC Slow: > ref × 1.5 ──────────────────────────────── */
    if (c->current_milliamps > (ref + ref / 2U))
    {
      if (c->above_slow_since_ms == 0U) c->above_slow_since_ms = now;
    }
    else { c->above_slow_since_ms = 0U; }

    /* ── OC Warning: > ref × 1.25 ──────────────────────────── */
    if (c->current_milliamps > (ref + ref / 4U))
    {
      if (c->above_warning_since_ms == 0U) c->above_warning_since_ms = now;
    }
    else { c->above_warning_since_ms = 0U; }

    /* ── UC Undercurrent: < normal_low AND < ref/2 ──────────── */
    if (c->current_milliamps < c->normal_low_ma &&
        c->current_milliamps < (ref / 2U))
    {
      if (c->below_undercurrent_since_ms == 0U) c->below_undercurrent_since_ms = now;
    }
    else { c->below_undercurrent_since_ms = 0U; }
  }
  else
  {
    /* ── Unlearned: user-configured OC threshold ────────────── */
    uint32_t threshold = cfg->current[idx].overcurrent_milliamps;
    if (threshold == 0U) threshold = RELAY_PROTECTION_DEFAULT_OVERCURRENT_MA;

    if (c->current_milliamps > threshold)
    {
      if (c->above_slow_since_ms == 0U) c->above_slow_since_ms = now;
    }
    else { c->above_slow_since_ms = 0U; }

    /* Unlearned: no UC protection (no baseline) — reset UC timer */
    c->below_undercurrent_since_ms = 0U;
  }

  /* ═══════════════════════════════════════════════════════════════════
     Phase 2: Decision by fault priority
     Priority: Fast OC > Slow OC > UC undercurrent > OC warning
     All timers have been updated independently above; now decide.
     ═══════════════════════════════════════════════════════════════════ */

  /* Priority 1: Fast OC (300ms) */
  if (c->above_fast_since_ms > 0U && (now - c->above_fast_since_ms) >= 300U)
  {
    rp_trip(c, (uint8_t)(idx + 1U), RELAY_PROTECTION_FAULT_MANUAL_RESET,
            RELAY_PROTECTION_REASON_OVERCURRENT, now);
    return;
  }

  /* Priority 2: Slow OC */
  if (c->above_slow_since_ms > 0U)
  {
    uint32_t oc_delay = c->learning_complete ? 3000U :
      (cfg->current[idx].trip_delay_ms ? cfg->current[idx].trip_delay_ms
                                       : RELAY_PROTECTION_DEFAULT_TRIP_DELAY_MS);
    if ((now - c->above_slow_since_ms) >= oc_delay)
    {
      rp_trip(c, (uint8_t)(idx + 1U), RELAY_PROTECTION_FAULT_MANUAL_RESET,
              RELAY_PROTECTION_REASON_OVERCURRENT, now);
      return;
    }
  }

  /* Priority 3: UC undercurrent (3s, learned only) */
  if (c->below_undercurrent_since_ms > 0U &&
      (now - c->below_undercurrent_since_ms) >= 3000U)
  {
    if (c->device_strategy == RELAY_PROTECTION_STRATEGY_SAFETY)
    {
      rp_trip(c, (uint8_t)(idx + 1U), RELAY_PROTECTION_FAULT_MANUAL_RESET,
              RELAY_PROTECTION_REASON_UNDERCURRENT, now);
      return;
    }
    else
    {
      /* Continuity: keep running, high-priority warning */
      if (c->fault_level < RELAY_PROTECTION_FAULT_WARNING)
        c->fault_level = RELAY_PROTECTION_FAULT_WARNING;
    }
  }

  /* Priority 4: OC warning (5s, warning only — no trip) */
  if (c->above_warning_since_ms > 0U &&
      (now - c->above_warning_since_ms) >= 5000U)
  {
    if (c->fault_level < RELAY_PROTECTION_FAULT_WARNING)
      c->fault_level = RELAY_PROTECTION_FAULT_WARNING;
  }
}

/* ── CT signal quality assessment (Phase G) ──────────────────────── */
static void rp_assess_ct_quality(rp_ch_t *c, uint32_t ac_rms_mv, uint32_t now)
{
  (void)now;
  if (!c->learning_complete) return;

  /* CT degraded: noise floor spike check.
     Compare AC RMS millivolts against OFF-state noise baseline.
     off_dc_bias_mv stores the OFF-state DC bias; we compute
     approximate noise floor from the stored off_rms_noise_ma
     (which is current_milliamps during OFF, converted back to mV). */
  {
    uint32_t off_noise_mv = c->off_rms_noise_ma / CT_MEASUREMENT_MA_PER_MV;
    /* Only trigger if we have a meaningful noise floor and spike is > 3× */
    if (off_noise_mv > 0U && ac_rms_mv > (off_noise_mv * 3U))
    {
      c->ct_degraded = 1U;
    }
  }

  /* If CT degraded and device is safety-first, trip */
  if (c->ct_degraded &&
      c->device_strategy == RELAY_PROTECTION_STRATEGY_SAFETY &&
      c->phase == RELAY_PROTECTION_PHASE_RUNNING)
  {
    rp_trip(c, (uint8_t)((uint8_t)(c - g_ch) + 1U),
            RELAY_PROTECTION_FAULT_MANUAL_RESET, RELAY_PROTECTION_REASON_OVERCURRENT, now);
  }
}

/* ── Stuck relay detection (Phase D) ─────────────────────────────── */
static void rp_check_stuck(rp_ch_t *c, uint8_t channel, uint32_t now,
                           uint32_t current_ma)
{
  uint32_t elapsed;
  uint32_t noise_floor;

  if (!c->stuck_check_pending) return;
  if (c->off_since_ms == 0U) return;

  elapsed = now - c->off_since_ms;

  /* Wait 300ms for current to decay after relay opens */
  if (elapsed < 300U) return;

  /* Observe for 2 seconds total, then make a single determination */
  if (elapsed < 2000U) return;

  /* 2-second verification window elapsed — final decision */
  c->stuck_check_pending = 0U;

  /* Per spec: threshold = max(off_noise × 3, 40mA minimum) */
  noise_floor = c->off_rms_noise_ma * 3U;
  if (noise_floor < 40U) noise_floor = 40U;

  {
    char log[64];
    (void)snprintf(log, sizeof(log), "\r\n[STUCK ch%u] cur=%lu noise=%lu lock=%d\r\n",
                   (unsigned int)channel, (unsigned long)current_ma,
                   (unsigned long)noise_floor, (int)(current_ma > noise_floor));
    rp_debug_uart(log);
  }

  if (current_ma > noise_floor)
  {
    rp_debug_uart("[STUCK] LOCKING!\r\n");
    volatile uint32_t *p_mask = &g_rp.fault_lock_mask;
    c->fault_locked = 1U;
    c->fault_level = RELAY_PROTECTION_FAULT_HARDWARE;
    c->fault_reason = RELAY_PROTECTION_REASON_RELAY_STUCK;
    c->phase = RELAY_PROTECTION_PHASE_FAULT;
    *p_mask |= (1UL << (channel - 1U));
    g_rp.fault_locked_any = 1U;
    __DSB();
  }
  else
  {
    rp_debug_uart("[STUCK] NOT stuck (current <= noise)\r\n");
  }
}

/* ── Main assessment entry point ─────────────────────────────────── */
void relay_protection_assess(uint8_t channel,
                             uint32_t current_milliamps,
                             uint8_t  ct_valid,
                             uint8_t  adc_saturated,
                             uint32_t bias_millivolts,
                             uint32_t ac_rms_millivolts,
                             uint8_t  relay_is_on,
                             uint32_t now_ms)
{
  if (channel == 0U || channel > 16U) return;
  if (!g_rp.init_done) return;

  uint8_t idx = channel - 1U;
  rp_ch_t *c = &g_ch[idx];
  const device_config_t *cfg = device_config_get();

  /* Sync runtime config from device_config (may have changed via HTTP API) */
  c->protection_enabled = cfg->current[idx].protection_enabled;

  /* Update current snapshot */
  c->current_milliamps = current_milliamps;
  /* Only flag saturation if bias is also valid — crosstalk from adjacent
     CT channels can cause saturation on channels with no real CT signal. */
  c->adc_saturated = adc_saturated &&
    (bias_millivolts >= CT_MEASUREMENT_BIAS_MIN_MV) &&
    (bias_millivolts <= CT_MEASUREMENT_BIAS_MAX_MV);
  (void)ct_valid;

  /* Update RMS ring buffer for stability detection */
  c->rms_history[c->rms_stable_index] = current_milliamps;
  c->rms_stable_index = (c->rms_stable_index + 1U) % 10U;

  /* ── Basic safety (always active, regardless of protection_enabled) ── */

  /* Hard 30A limit — always checked */
  if (rp_check_hard_limit(c, channel, now_ms)) return;

  /* If already fault-locked, skip all further checks */
  if (c->fault_locked)
    return;

  /* ADC saturation (2s continuous → fault).
     Only checked when relay is ON — OFF channels may have crosstalk. */
  if (relay_is_on)
  {
    if (rp_check_saturation(c, channel, now_ms)) return;
  }
  else
  {
    c->adc_saturation_since_ms = 0U; /* reset saturation timer when OFF */
  }

  /* ── Stuck relay detection (Phase D) ── */
  if (!relay_is_on)
  {
    rp_check_stuck(c, channel, now_ms, current_milliamps);
  }

  /* ── CT quality check (Phase G) ── */
  if (c->protection_enabled)
    rp_assess_ct_quality(c, ac_rms_millivolts, now_ms);

  /* ── Phase-based assessment ── */
  switch (c->phase)
  {
    case RELAY_PROTECTION_PHASE_OFF:
      /* Record OFF state characteristics */
      if (!c->learning_complete)
      {
        /* Track OFF noise — but NOT during stuck detection window,
           or the injected/noisy current would contaminate the noise floor. */
        if (!c->stuck_check_pending &&
            current_milliamps > c->off_rms_noise_ma)
          c->off_rms_noise_ma = current_milliamps;
        if (bias_millivolts > c->off_dc_bias_mv)
          c->off_dc_bias_mv = bias_millivolts;
      }
      break;

    case RELAY_PROTECTION_PHASE_STARTUP:
      /* Basic safety (hard 30A, saturation) already checked above.
         Startup failure / stall detection requires protection_enabled. */
      if (c->protection_enabled)
        rp_assess_startup(c, now_ms);
      else
      {
        /* Simple phase transition: if current stable, go to RUNNING.
           Without protection, just track the phase for status display. */
        if (rp_is_stable(c) && c->relay_on_since_ms != 0U &&
            (now_ms - c->relay_on_since_ms) >= 3000U)
        {
          c->phase = RELAY_PROTECTION_PHASE_RUNNING;
          c->phase_enter_ms = now_ms;
        }
      }
      break;

    case RELAY_PROTECTION_PHASE_RUNNING:
      if (c->protection_enabled)
        rp_assess_running(c, now_ms);
      else
      {
        /* Even without smart protection, check user threshold */
        const device_config_t *cfg = device_config_get();
        uint32_t threshold = cfg->current[idx].overcurrent_milliamps;
        if (threshold == 0U) threshold = RELAY_PROTECTION_DEFAULT_OVERCURRENT_MA;
        if (c->current_milliamps > threshold)
        {
          uint32_t delay = cfg->current[idx].trip_delay_ms;
          if (delay == 0U) delay = RELAY_PROTECTION_DEFAULT_TRIP_DELAY_MS;
          if (c->above_slow_since_ms == 0U) c->above_slow_since_ms = now_ms;
          if ((now_ms - c->above_slow_since_ms) >= delay)
            rp_trip(c, channel, RELAY_PROTECTION_FAULT_MANUAL_RESET, RELAY_PROTECTION_REASON_OVERCURRENT, now_ms);
        }
        else { c->above_slow_since_ms = 0U; }
      }
      break;

    case RELAY_PROTECTION_PHASE_COOLDOWN:
      /* Check if cooldown expired */
      if ((int32_t)(now_ms - c->cooldown_until_ms) >= 0)
      {
        c->phase = RELAY_PROTECTION_PHASE_OFF;
        c->tripped = 0U;
        c->fault_level = RELAY_PROTECTION_FAULT_NONE;
      }
      break;

    case RELAY_PROTECTION_PHASE_FAULT:
      /* Locked — no auto-recovery */
      break;

    default:
      break;
  }
}

/* ── Poll ────────────────────────────────────────────────────────── */
void relay_protection_poll(void)
{
  uint32_t now = HAL_GetTick();

  /* Check auto-retry for startup failures */
  for (uint8_t i = 0U; i < RELAY_PROTECTION_CHANNEL_COUNT; i++)
  {
    rp_ch_t *c = &g_ch[i];
    if (c->retry_pending && (int32_t)(now - c->retry_after_ms) >= 0)
    {
      c->retry_pending = 0U;
      if (!c->fault_locked && c->tripped)
      {
        /* Clear trip, allow ON command */
        c->tripped = 0U;
        c->phase = RELAY_PROTECTION_PHASE_OFF;
        c->fault_level = RELAY_PROTECTION_FAULT_NONE;
        /* Re-submit ON command */
        {
          control_proto_command_t cmd;
          memset(&cmd, 0, sizeof(cmd));
          cmd.type = CONTROL_PROTO_CMD_SET_RELAY;
          cmd.channel = (uint8_t)(i + 1U);
          cmd.value = 1U;
          cmd.error = CONTROL_PROTO_ERR_NONE;
          (void)control_queue_submit_async(&cmd);
        }
      }
    }
  }
}

/* ── Query API ───────────────────────────────────────────────────── */
uint8_t relay_protection_get_summary(uint8_t channel,
                                     relay_protection_summary_t *out)
{
  if (channel == 0U || channel > 16U || out == NULL) return 0U;
  rp_ch_t *c = &g_ch[channel - 1U];

  memset(out, 0, sizeof(*out));
  out->phase = c->phase;
  out->fault_level = c->fault_level;
  out->fault_reason = c->fault_reason;
  out->learning_complete = c->learning_complete;
  out->protection_enabled = c->protection_enabled;
  out->ct_degraded = c->ct_degraded;
  out->adc_saturated = c->adc_saturated;
  out->tripped = c->tripped;
  out->fault_locked = c->fault_locked;
  out->device_strategy = c->device_strategy;
  out->current_milliamps = c->current_milliamps;
  out->current_reference_ma = c->initial_health_baseline_ma + c->aging_offset_ma;
  out->initial_health_baseline_ma = c->initial_health_baseline_ma;
  out->aging_offset_ma = c->aging_offset_ma;
  out->normal_low_ma = c->normal_low_ma;
  out->normal_high_ma = c->normal_high_ma;
  out->startup_peak_ma = c->startup_peak_ma;
  out->startup_duration_ms = c->startup_duration_ms;
  out->trip_count = c->trip_count;
  out->cooldown_until_ms = c->cooldown_until_ms;
  out->learning_samples_collected = c->learning_samples_collected;
  out->off_since_ms = c->off_since_ms;
  out->stuck_check_pending = c->stuck_check_pending;
  return 1U;
}

void relay_protection_get_global_summary(relay_protection_global_summary_t *out)
{
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  out->boot_safety_active = g_rp.boot_safety_active;
  out->fault_locked_any = g_rp.fault_locked_any;
  out->boot_delay_until_ms = g_rp.boot_delay_until_ms;
  out->restart_count = g_rp.restart_count;
  out->last_boot_ms = g_rp.last_boot_ms;
}

void relay_protection_clear_learning(uint8_t channel)
{
  if (channel == 0U || channel > 16U) return;
  rp_ch_t *c = &g_ch[channel - 1U];
  c->learning_complete = 0U;
  c->learning_samples_collected = 0U;
  c->aging_offset_ma = 0U;
  memset(c->learning_baselines, 0, sizeof(c->learning_baselines));
  memset(c->learning_peaks, 0, sizeof(c->learning_peaks));
  memset(c->learning_durations, 0, sizeof(c->learning_durations));
}

void relay_protection_unlock(uint8_t channel)
{
  if (channel == 0U || channel > 16U) return;
  rp_ch_t *c = &g_ch[channel - 1U];
  c->fault_locked = 0U;
  c->fault_level = RELAY_PROTECTION_FAULT_NONE;
  c->fault_reason = RELAY_PROTECTION_REASON_NONE;
  c->tripped = 0U;
  c->phase = RELAY_PROTECTION_PHASE_OFF;
  c->trip_count = 0U;
  c->cooldown_until_ms = 0U;
  c->adc_saturation_since_ms = 0U;
  c->retry_pending = 0U;
  c->retry_count = 0U;
  c->off_rms_noise_ma = 0U;  /* reset noise floor for stuck detection */
  c->off_dc_bias_mv = 0U;
  c->stuck_check_pending = 0U;   /* clear stuck detection state */
  c->off_since_ms = 0U;
  c->stuck_above_count = 0U;
  g_rp.fault_lock_mask &= ~(1UL << (channel - 1U));
  g_rp.fault_locked_any = (g_rp.fault_lock_mask != 0U) ? 1U : 0U;
}

/* ── Config setters ──────────────────────────────────────────────── */
void relay_protection_set_strategy(uint8_t channel, uint8_t strategy)
{
  if (channel == 0U || channel > 16U) return;
  g_ch[channel - 1U].device_strategy = strategy;
}

void relay_protection_set_enabled(uint8_t channel, uint8_t enabled)
{
  if (channel == 0U || channel > 16U) return;
  g_ch[channel - 1U].protection_enabled = enabled;
}

void relay_protection_set_overcurrent_ma(uint8_t channel, uint32_t ma)
{
  (void)channel; (void)ma;
  /* Read from device_config at assessment time */
}

void relay_protection_set_trip_delay_ms(uint8_t channel, uint32_t ms)
{
  (void)channel; (void)ms;
  /* Read from device_config at assessment time */
}


/* ── User config accessors (Phase §14) ─────────────────────────────── */
void relay_protection_get_user_config(relay_protection_user_config_t *out)
{
  if (out != NULL) *out = g_ucfg;
}

void relay_protection_set_user_config(const relay_protection_user_config_t *cfg)
{
  if (cfg != NULL) g_ucfg = *cfg;
}

