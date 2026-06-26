#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHEDULER_CHANNEL_COUNT     16U
#define SCHEDULER_MAX_INTERVALS      6U
#define SCHEDULER_PAUSE_FOREVER      0xFFFFFFFFU
#define SCHEDULER_AUTO_HOLD_MIN_SEC  1U
#define SCHEDULER_AUTO_HOLD_MAX_SEC  1800U
#define SCHEDULER_REASON_MAX_LEN     47

/* ── Time interval for one ON period ── */
typedef struct {
    uint16_t start_minute;   /* 0..1439 */
    uint16_t end_minute;     /* 1..1440, half-open [start, end) */
} schedule_interval_t;

/* ── One day's schedule ── */
typedef struct {
    uint8_t  interval_count;
    schedule_interval_t intervals[SCHEDULER_MAX_INTERVALS];
} daily_schedule_t;

/* ── Full week schedule for one channel ── */
typedef struct {
    uint8_t channel;
    daily_schedule_t days[7];   /* 0=Mon .. 6=Sun */
} weekly_schedule_t;

/* ── Auto Hold state (backend sensor lease) ── */
typedef struct {
    uint32_t session;        /* 0 = no hold; non-zero = active */
    uint8_t  target_state;   /* 0=OFF, 1=ON */
    uint8_t  start_state;    /* device state when session first accepted */
    uint32_t until_ms;       /* monotonic ms deadline */
    char     reason[SCHEDULER_REASON_MAX_LEN + 1];
} auto_hold_state_t;

/* ── Per-channel scheduler state ── */
typedef struct {
    uint8_t  schedule_enabled;

    uint32_t schedule_pause_until;       /* unix timestamp, 0 = not paused */
    uint8_t  pause_blocks_auto_trigger;  /* 1 = also block Auto Hold */

    uint32_t last_auto_hold_command_ts;  /* for stale-packet rejection */

    auto_hold_state_t auto_hold;
} channel_scheduler_state_t;

/* ── Pause mode enum for API ── */
typedef enum {
    SCHEDULER_PAUSE_CURRENT_SEGMENT = 0,
    SCHEDULER_PAUSE_DURATION        = 1,
    SCHEDULER_PAUSE_UNTIL           = 2,
    SCHEDULER_PAUSE_FOREVER_MODE    = 3
} scheduler_pause_mode_t;

/* ── Public API ── */

void scheduler_init(void);
void scheduler_tick(uint32_t monotonic_ms, uint32_t unix_time, uint8_t time_valid);

/* State access — const pointers for reads, snapshot for long-lived access */
const channel_scheduler_state_t *scheduler_get_state(uint8_t channel);
const weekly_schedule_t         *scheduler_get_weekly(uint8_t channel);
uint8_t scheduler_get_state_copy(uint8_t channel, channel_scheduler_state_t *dst);
uint8_t scheduler_get_weekly_copy(uint8_t channel, weekly_schedule_t *dst);

/* State modifiers — REQUIRED for writes (raw pointer writes blocked by const) */
uint8_t scheduler_set_enabled(uint8_t channel, uint8_t enabled);
uint8_t scheduler_set_pause(uint8_t channel, uint32_t pause_until, uint8_t pause_blocks);
uint8_t scheduler_set_weekly_day(uint8_t channel, uint8_t weekday, const daily_schedule_t *day);
uint8_t scheduler_copy_weekly_day(uint8_t channel, uint8_t src_weekday, const uint8_t *target_wds, uint8_t count);
void    scheduler_clear_pause(uint8_t channel);

/* Auto Hold command from backend */
typedef struct {
    uint8_t  channel;
    uint8_t  target;            /* 0=OFF, 1=ON */
    uint32_t duration_seconds;  /* 0 = clear */
    uint32_t session;
    uint32_t command_ts;
    char     reason[SCHEDULER_REASON_MAX_LEN + 1];
} auto_hold_cmd_t;

void scheduler_handle_auto_hold(const auto_hold_cmd_t *cmd, uint32_t monotonic_ms, uint32_t unix_time);

/* User manual control (atomic: clear Auto Hold + set pause + send relay cmd) */
void scheduler_user_control(uint8_t channel, uint8_t target,
                            scheduler_pause_mode_t pause_mode,
                            uint32_t pause_duration_seconds,
                            uint32_t pause_until_unix,
                            uint8_t pause_blocks_auto_trigger);

/* Schedule management */
bool scheduler_validate_intervals(const daily_schedule_t *day);

/* Persistence */
void scheduler_load_from_flash(void);
void scheduler_save_to_flash(void);
void scheduler_save_weekly_to_flash(void);
void scheduler_mark_flash_dirty(void);
void scheduler_mark_rtc_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULER_H */
