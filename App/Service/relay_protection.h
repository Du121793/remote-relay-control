#ifndef RELAY_PROTECTION_H
#define RELAY_PROTECTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_PROTECTION_CHANNEL_COUNT 16U

/* ── Fault classification (Phase H) ─────────────────────────────── */
#define RELAY_PROTECTION_FAULT_NONE          0U
#define RELAY_PROTECTION_FAULT_WARNING       1U
#define RELAY_PROTECTION_FAULT_AUTO_RECOVER  2U
#define RELAY_PROTECTION_FAULT_MANUAL_RESET  3U
#define RELAY_PROTECTION_FAULT_HARDWARE      4U

/* Fault reason codes (for display in Web UI) */
#define RELAY_PROTECTION_REASON_NONE              0U
#define RELAY_PROTECTION_REASON_STARTUP_FAIL      1U
#define RELAY_PROTECTION_REASON_OVERCURRENT       2U
#define RELAY_PROTECTION_REASON_UNDERCURRENT      3U
#define RELAY_PROTECTION_REASON_HARD_30A          4U
#define RELAY_PROTECTION_REASON_ADC_SATURATION    5U
#define RELAY_PROTECTION_REASON_RELAY_STUCK       6U
#define RELAY_PROTECTION_REASON_FORCE_STOP        7U
#define RELAY_PROTECTION_REASON_BOOT_RESTART      8U
#define RELAY_PROTECTION_REASON_STALL             9U
#define RELAY_PROTECTION_REASON_STARTUP_PEAK      10U

/* Device strategy */
#define RELAY_PROTECTION_STRATEGY_SAFETY     0U
#define RELAY_PROTECTION_STRATEGY_CONTINUITY 1U

/* Phase enum */
#define RELAY_PROTECTION_PHASE_OFF           0U
#define RELAY_PROTECTION_PHASE_BOOT          1U
#define RELAY_PROTECTION_PHASE_STARTUP       2U
#define RELAY_PROTECTION_PHASE_RUNNING       3U
#define RELAY_PROTECTION_PHASE_FAULT         4U
#define RELAY_PROTECTION_PHASE_COOLDOWN      5U

/* ── User-configurable defaults (Phase §14) ──────────────────────── */
#define RELAY_PROTECTION_DEFAULT_OVERCURRENT_MA     4500U
#define RELAY_PROTECTION_DEFAULT_TRIP_DELAY_MS       1000U
#define RELAY_PROTECTION_DEFAULT_HYST_MA             500U
#define RELAY_PROTECTION_DEFAULT_COOLDOWN_SEC          3U
#define RELAY_PROTECTION_DEFAULT_RETRY_DELAY_SEC      30U
#define RELAY_PROTECTION_DEFAULT_MAX_RETRY             1U
#define RELAY_PROTECTION_DEFAULT_BOOT_DELAY_SEC        5U
#define RELAY_PROTECTION_DEFAULT_LEARNING_SAMPLES      5U
#define RELAY_PROTECTION_DEFAULT_AGING_HOURS          24U
#define RELAY_PROTECTION_DEFAULT_AGING_CAP_PERCENT    50U
#define RELAY_PROTECTION_DEFAULT_STALL_MULTIPLIER     20U  /* ×0.1 → 2.0 */
#define RELAY_PROTECTION_HARD_CURRENT_LIMIT_MA     30000U
#define RELAY_PROTECTION_STALL_FALLBACK_MS         10000U  /* unlearned channel fallback */

/* Runtime-configurable settings (per-channel or global as noted) */
typedef struct {
  uint32_t cooldown_seconds;       /* global */
  uint32_t retry_delay_seconds;    /* global */
  uint32_t max_retry_count;        /* global */
  uint32_t boot_delay_seconds;     /* global */
  uint32_t learning_samples;       /* global */
  uint32_t aging_update_hours;     /* global */
  uint32_t aging_cap_percent;      /* global */
  uint32_t stall_timeout_multiplier_x10; /* global, ×10 (20=2.0) */
} relay_protection_user_config_t;

/* ── Per-channel state visible to HTTP API ──────────────────────── */
typedef struct
{
  uint8_t  phase;
  uint8_t  fault_level;
  uint8_t  fault_reason;
  uint8_t  learning_complete;
  uint8_t  protection_enabled;
  uint8_t  ct_degraded;
  uint8_t  adc_saturated;
  uint8_t  tripped;
  uint8_t  fault_locked;
  uint8_t  device_strategy;
  uint8_t  reserved0[2];
  uint32_t current_milliamps;
  uint32_t current_reference_ma;
  uint32_t initial_health_baseline_ma;
  uint32_t aging_offset_ma;
  uint32_t normal_low_ma;
  uint32_t normal_high_ma;
  uint32_t startup_peak_ma;
  uint32_t startup_duration_ms;
  uint32_t trip_count;
  uint32_t cooldown_until_ms;
  uint32_t learning_samples_collected;
  uint32_t off_since_ms;       /* debug: stuck detection timer */
  uint8_t  stuck_check_pending; /* debug: stuck detection flag */
  uint8_t  reserved1[3];
} relay_protection_summary_t;

/* ── Global state ────────────────────────────────────────────────── */
typedef struct
{
  uint8_t boot_safety_active;
  uint8_t fault_locked_any;
  uint8_t reserved[2];
  uint32_t boot_delay_until_ms;
  uint32_t restart_count;
  uint32_t last_boot_ms;
} relay_protection_global_summary_t;

/* ── Public API ──────────────────────────────────────────────────── */

/* Init: call once at boot. Sets boot delay, detects rapid restarts. */
void relay_protection_init(void);

/* User-configurable runtime settings (Phase §14) */
void relay_protection_get_user_config(relay_protection_user_config_t *out);
void relay_protection_set_user_config(const relay_protection_user_config_t *cfg);

/* Poll: call periodically (e.g. every 100ms from adc_service_poll or main loop). */
void relay_protection_poll(void);

/* Assess one channel after ADC publishes a new measurement window.
   Called from adc_service poll context with the CT result for this channel.
   relay_is_on: 1 if relay GPIO is currently ON, 0 if OFF. */
void relay_protection_assess(uint8_t channel,
                             uint32_t current_milliamps,
                             uint8_t  ct_valid,
                             uint8_t  adc_saturated,
                             uint32_t bias_millivolts,
                             uint32_t ac_rms_millivolts,
                             uint8_t  relay_is_on,
                             uint32_t now_ms);

/* Force-stop a channel (Phase A). Sets flag, worker will shut off relay. */
void relay_protection_force_stop(uint8_t channel);

/* Notify protection module that a relay was commanded ON or OFF.
   Called from control_queue worker when executing a relay command. */
void relay_protection_notify_relay_change(uint8_t channel, uint8_t new_state,
                                          uint32_t now_ms);

/* Query per-channel summary (for HTTP API). */
uint8_t relay_protection_get_summary(uint8_t channel,
                                     relay_protection_summary_t *out);

/* Query global summary. */
void relay_protection_get_global_summary(relay_protection_global_summary_t *out);

/* Clear learning data for one channel. */
void relay_protection_clear_learning(uint8_t channel);

/* Unlock a fault-locked channel (manual reset). */
void relay_protection_unlock(uint8_t channel);

/* Check if a channel is in cooldown (reject ON). */
uint8_t relay_protection_is_cooldown(uint8_t channel, uint32_t now_ms);

/* Check if boot safety delay is still active (reject all ON). */
uint8_t relay_protection_boot_safety_active(uint32_t now_ms);

/* Get the fault lockout mask (bit N = channel N+1 locked). */
uint32_t relay_protection_fault_lock_mask(void);

/* Set device strategy per channel. */
void relay_protection_set_strategy(uint8_t channel, uint8_t strategy);

/* Set protection_enabled per channel. */
void relay_protection_set_enabled(uint8_t channel, uint8_t enabled);

/* Set user overcurrent threshold per channel (used when not learned). */
void relay_protection_set_overcurrent_ma(uint8_t channel, uint32_t ma);

/* Set user trip delay per channel. */
void relay_protection_set_trip_delay_ms(uint8_t channel, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_PROTECTION_H */
