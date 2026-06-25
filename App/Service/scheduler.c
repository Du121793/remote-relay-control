#include "scheduler.h"
#include "../Config/device_config.h"
#include "control_queue.h"
#include "relay_protection.h"
#include "cluster_service.h"
#include "board_relay.h"
#include "stm32f7xx_hal_rtc.h"
#include "stm32f7xx_hal_flash.h"
#include <string.h>

extern RTC_HandleTypeDef hrtc;

/* ── Per-channel state arrays ── */
static channel_scheduler_state_t g_scheduler_state[SCHEDULER_CHANNEL_COUNT];
static weekly_schedule_t g_weekly_schedule[SCHEDULER_CHANNEL_COUNT];

/* ── Monotonic time helpers ── */
static bool time_after_or_equal(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/* ── init ── */
void scheduler_init(void)
{
    memset(g_scheduler_state, 0, sizeof(g_scheduler_state));
    memset(g_weekly_schedule, 0, sizeof(g_weekly_schedule));
    for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
        g_weekly_schedule[i].channel = i + 1;
    }
    /* Restore persisted state (pause, enabled). Auto Hold not persisted. */
    scheduler_load_from_flash();
}

/* ═══════════════════════════════════════════════════════════════
   Persistence: RTC backup (pause/enable) + Flash (schedule)
   Sector chosen to avoid overlap with device_config at 0x081E0000.
   ═══════════════════════════════════════════════════════════════ */

#define SCHEDULER_FLASH_ADDR_DUAL 0x081C0000UL  /* dual-bank: sector 22 */
#define SCHEDULER_FLASH_ADDR_SINGLE 0x08180000UL /* single-bank: sector 10 */
#define SCHEDULER_FLASH_MAGIC2 0x53434833U  /* "SCH3" — bumped for sector fix */
#define SCHEDULER_FLASH_VERSION 2U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc;
    uint8_t  schedule_enabled[SCHEDULER_CHANNEL_COUNT];
    weekly_schedule_t weekly[SCHEDULER_CHANNEL_COUNT];
} scheduler_flash_data_t;

static uint32_t sched_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1U) crc = (crc >> 1U) ^ 0xEDB88320U;
            else crc >>= 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

#define SCHEDULER_RTC_MAGIC  0x53434845U  /* "SCHE" */
/* RTC backup registers: DR0=relay_protection boot time.
   DR1=magic_low16 | (enabled_bits << 16)
   DR2-DR17=pause_until[0..15] (bit0=pause_blocks_auto_trigger) */
/* Forward declarations */
static uint32_t scheduler_flash_addr(void);
static uint32_t scheduler_flash_sector(void);


void scheduler_load_from_flash(void)
{
    /* RTC backup: pause/enable (fast, every tick) */
    uint32_t magic = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1);
    if ((magic & 0xFFFFU) == (SCHEDULER_RTC_MAGIC & 0xFFFFU)) {
        uint16_t enabled = (uint16_t)(magic >> 16);
        for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
            g_scheduler_state[i].schedule_enabled = (uint8_t)((enabled >> i) & 1U);
            uint32_t pause = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR2 + i);
            g_scheduler_state[i].schedule_pause_until = pause & 0xFFFFFFFEU;
            g_scheduler_state[i].pause_blocks_auto_trigger = (uint8_t)(pause & 1U);
        }
    }

    /* Flash: weekly schedule (persistent across power cycles) */
    const scheduler_flash_data_t *stored =
        (const scheduler_flash_data_t *)scheduler_flash_addr();
    if (stored->magic == SCHEDULER_FLASH_MAGIC2 &&
        stored->version == SCHEDULER_FLASH_VERSION) {
        uint32_t computed = sched_crc32(
            (const uint8_t *)&stored->schedule_enabled,
            sizeof(scheduler_flash_data_t) - offsetof(scheduler_flash_data_t, schedule_enabled));
        if (computed == stored->crc) {
            for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
                if (!g_scheduler_state[i].schedule_enabled)
                    g_scheduler_state[i].schedule_enabled = stored->schedule_enabled[i];
                g_weekly_schedule[i] = stored->weekly[i];
            }
        }
    }
}

void scheduler_save_to_flash(void)
{
    /* RTC backup: pause/enable (fast, every tick) */
    uint32_t magic = (SCHEDULER_RTC_MAGIC & 0xFFFFU);
    uint32_t enabled = 0;
    for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
        if (g_scheduler_state[i].schedule_enabled) enabled |= (1UL << i);
    }
    magic |= (enabled << 16);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, magic);
    for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
        uint32_t pause = g_scheduler_state[i].schedule_pause_until & 0xFFFFFFFEU;
        pause |= (uint32_t)g_scheduler_state[i].pause_blocks_auto_trigger & 1U;
        HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2 + i, pause);
    }
}

/* Flash save: dirty-driven with error checking and correct sector selection. */
static uint32_t g_last_flash_save_ms = 0;
static uint32_t g_last_flash_crc = 0;
static uint8_t  g_scheduler_flash_dirty = 0U;

void scheduler_mark_flash_dirty(void) { g_scheduler_flash_dirty = 1U; }

static uint32_t scheduler_flash_addr(void)
{
#if defined(FLASH_OPTCR_nDBANK)
  if ((FLASH->OPTCR & FLASH_OPTCR_nDBANK) != 0U)
    return SCHEDULER_FLASH_ADDR_SINGLE;  /* single-bank: sector 10 */
  return SCHEDULER_FLASH_ADDR_DUAL;      /* dual-bank:   sector 22 */
#else
  return SCHEDULER_FLASH_ADDR_SINGLE;
#endif
}

static uint32_t scheduler_flash_sector(void)
{
#if defined(FLASH_OPTCR_nDBANK)
  if ((FLASH->OPTCR & FLASH_OPTCR_nDBANK) != 0U)
    return FLASH_SECTOR_10;
  return FLASH_SECTOR_22;
#else
  return FLASH_SECTOR_10;
#endif
}

void scheduler_save_weekly_to_flash(void)
{
    uint32_t now = HAL_GetTick();
    if (g_scheduler_flash_dirty == 0U) return;
    /* Deferred coalescing: wait 2 s since LAST MARK to batch changes */
    if (g_last_flash_save_ms != 0 &&
        (now - g_last_flash_save_ms) < 2000U) return;

    scheduler_flash_data_t data;
    memset(&data, 0, sizeof(data));
    data.magic = SCHEDULER_FLASH_MAGIC2;
    data.version = SCHEDULER_FLASH_VERSION;

    for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
        data.schedule_enabled[i] = g_scheduler_state[i].schedule_enabled;
        data.weekly[i] = g_weekly_schedule[i];
    }
    data.crc = sched_crc32(
        (const uint8_t *)&data.schedule_enabled,
        sizeof(data) - offsetof(scheduler_flash_data_t, schedule_enabled));

    /* Skip write if content unchanged from last successful save */
    if (data.crc == g_last_flash_crc && g_last_flash_crc != 0U) {
        g_scheduler_flash_dirty = 0U;
        g_last_flash_save_ms = now;
        return;
    }

    {
      uint32_t addr = scheduler_flash_addr();
      uint32_t sector = scheduler_flash_sector();
      uint8_t write_ok = 1U;

      HAL_FLASH_Unlock();
      __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP);
      FLASH_Erase_Sector(sector, VOLTAGE_RANGE_3);
      if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_EOP) != RESET)
      {
        for (size_t i = 0; i < sizeof(data); i += 4)
        {
            uint32_t word;
            memcpy(&word, ((uint8_t *)&data) + i, 4);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK)
            {
                write_ok = 0U;
                break;
            }
        }
      }
      else
      {
        write_ok = 0U;
      }
      HAL_FLASH_Lock();

      /* Only clear dirty + update CRC on success */
      if (write_ok != 0U) {
        g_last_flash_crc = data.crc;
        g_scheduler_flash_dirty = 0U;
        g_last_flash_save_ms = now;
      }
      /* On failure, dirty stays set → retry next tick */
    }
}

/* ── state access ── */
channel_scheduler_state_t *scheduler_get_state(uint8_t channel)
{
    if (channel == 0 || channel > SCHEDULER_CHANNEL_COUNT) return NULL;
    return &g_scheduler_state[channel - 1];
}

weekly_schedule_t *scheduler_get_weekly(uint8_t channel)
{
    if (channel == 0 || channel > SCHEDULER_CHANNEL_COUNT) return NULL;
    return &g_weekly_schedule[channel - 1];
}

/* ── interval validation ── */
bool scheduler_validate_intervals(const daily_schedule_t *day)
{
    if (day->interval_count > SCHEDULER_MAX_INTERVALS) return false;
    for (uint8_t i = 0; i < day->interval_count; i++) {
        uint16_t s = day->intervals[i].start_minute;
        uint16_t e = day->intervals[i].end_minute;
        if (s >= 1440 || e == 0 || e > 1440 || s >= e) return false;
        if (i > 0) {
            uint16_t ps = day->intervals[i - 1].start_minute;
            uint16_t pe = day->intervals[i - 1].end_minute;
            /* check sorted and non-overlapping */
            if (s < ps) return false;
            if (s < pe) return false;  /* overlap: can be adjacent (s==pe ok) */
        }
    }
    return true;
}

/* ── interval matching ── */
static bool interval_match(const daily_schedule_t *day, uint16_t minute_of_day)
{
    for (uint8_t i = 0; i < day->interval_count; i++) {
        if (minute_of_day >= day->intervals[i].start_minute &&
            minute_of_day <  day->intervals[i].end_minute) {
            return true;
        }
    }
    return false;
}

/* ── weekday from unix time (Monday=0) ── */
static uint8_t weekday_from_unix(uint32_t unix_time)
{
    /* 1970-01-01 was Thursday (3).  (unix/86400 + 3) % 7 */
    return (uint8_t)(((unix_time / 86400UL) + 3UL) % 7UL);
}

/* ── minute of day from unix time ── */
static uint16_t minute_of_day(uint32_t unix_time)
{
    return (uint16_t)((unix_time % 86400UL) / 60UL);
}

/* ── reason copy with validation ── */
static void copy_reason(char *dst, const char *src)
{
    if (!src || src[0] == '\0') {
        strncpy(dst, "auto-hold control", SCHEDULER_REASON_MAX_LEN);
        dst[SCHEDULER_REASON_MAX_LEN] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len > SCHEDULER_REASON_MAX_LEN) len = SCHEDULER_REASON_MAX_LEN;
    /* validate: only printable ASCII + UTF-8 multi-byte */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 && c != '\0') {
            /* invalid character, use default */
            strncpy(dst, "auto-hold control", SCHEDULER_REASON_MAX_LEN);
            dst[SCHEDULER_REASON_MAX_LEN] = '\0';
            return;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ── request relay state through control queue ── */
static void request_relay(uint8_t channel, uint8_t target)
{
    control_proto_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CONTROL_PROTO_CMD_SET_RELAY;
    cmd.channel = channel;
    cmd.value = target;
    (void)control_queue_submit_async(&cmd);
}

/* ═══════════════════════════════════════════════════════════════
   Auto Hold handler
   ═══════════════════════════════════════════════════════════════ */

static void auto_hold_clear(uint8_t channel)
{
    channel_scheduler_state_t *st = &g_scheduler_state[channel - 1];
    st->auto_hold.session = 0;
    st->auto_hold.target_state = 0;
    st->auto_hold.start_state = 0;
    st->auto_hold.until_ms = 0;
    st->auto_hold.reason[0] = '\0';
}

static uint8_t relay_actual_state(uint8_t channel)
{
    uint32_t mask = board_relay_snapshot();
    return ((mask >> (channel - 1)) & 1U) ? 1U : 0U;
}

void scheduler_handle_auto_hold(const auto_hold_cmd_t *cmd, uint32_t monotonic_ms, uint32_t unix_time)
{
    (void)unix_time;
    if (cmd->channel == 0 || cmd->channel > SCHEDULER_CHANNEL_COUNT) return;
    if (cmd->session == 0) return;  /* session=0 is invalid */

    channel_scheduler_state_t *st = &g_scheduler_state[cmd->channel - 1];
    auto_hold_state_t *hold = &st->auto_hold;

    /* stale packet check */
    if (cmd->command_ts < st->last_auto_hold_command_ts) return;
    st->last_auto_hold_command_ts = cmd->command_ts;

    /* clear request */
    if (cmd->duration_seconds == 0) {
        if (hold->session == cmd->session) {
            auto_hold_clear(cmd->channel);
        }
        return;
    }

    /* validate duration */
    if (cmd->duration_seconds < SCHEDULER_AUTO_HOLD_MIN_SEC ||
        cmd->duration_seconds > SCHEDULER_AUTO_HOLD_MAX_SEC) {
        return;
    }

    /* user pause blocks auto trigger? */
    if (st->schedule_pause_until != 0 &&
        st->pause_blocks_auto_trigger &&
        unix_time < st->schedule_pause_until) {
        return;
    }

    /* different session already active? */
    if (hold->session != 0 && hold->session != cmd->session) {
        return;  /* CHANNEL_BUSY */
    }

    /* same session, different target? */
    if (hold->session == cmd->session && hold->target_state != cmd->target) {
        return;  /* SESSION_TARGET_MISMATCH */
    }

    /* new session */
    if (hold->session == 0) {
        hold->session = cmd->session;
        hold->target_state = cmd->target;
        hold->start_state = relay_actual_state(cmd->channel);
    }

    /* update lease (new or renew) */
    hold->until_ms = monotonic_ms + (cmd->duration_seconds * 1000U);
    copy_reason(hold->reason, cmd->reason);

    /* request target state */
    request_relay(cmd->channel, hold->target_state);
}

/* ═══════════════════════════════════════════════════════════════
   Auto Hold finish
   ═══════════════════════════════════════════════════════════════ */

static void auto_hold_finish(uint8_t channel, uint32_t unix_time, uint8_t time_valid)
{
    channel_scheduler_state_t *st = &g_scheduler_state[channel - 1];
    auto_hold_state_t *hold = &st->auto_hold;

    if (hold->session == 0) return;

    uint8_t restore_state = hold->start_state;

    hold->session = 0;
    hold->target_state = 0;
    hold->start_state = 0;
    hold->until_ms = 0;
    hold->reason[0] = '\0';

    /* don't restore if channel is in fault or cooldown */
    uint32_t fault_mask = relay_protection_fault_lock_mask();
    if ((fault_mask >> (channel - 1)) & 1U) return;
    if (relay_protection_is_cooldown(channel, HAL_GetTick())) return;

    /* hand back to scheduler if possible */
    if (st->schedule_enabled &&
        (st->schedule_pause_until == 0 || unix_time >= st->schedule_pause_until) &&
        time_valid) {
        /* scheduler will pick up desired state on next tick */
        return;
    }

    /* otherwise restore start_state */
    request_relay(channel, restore_state);
}

/* ═══════════════════════════════════════════════════════════════
   User control (atomic)
   ═══════════════════════════════════════════════════════════════ */

void scheduler_user_control(uint8_t channel, uint8_t target,
                            scheduler_pause_mode_t pause_mode,
                            uint32_t pause_duration_seconds,
                            uint32_t pause_until_unix,
                            uint8_t pause_blocks_auto_trigger)
{
    if (channel == 0 || channel > SCHEDULER_CHANNEL_COUNT) return;
    channel_scheduler_state_t *st = &g_scheduler_state[channel - 1];

    /* 1. clear Auto Hold */
    auto_hold_clear(channel);

    /* 2. set pause */
    uint8_t tv;
    switch (pause_mode) {
        case SCHEDULER_PAUSE_CURRENT_SEGMENT:
            /* pause until end of current schedule segment + 1 minute */
            /* simplified: pause for 1 minute */
            st->schedule_pause_until = cluster_service_get_unix_time(&tv) + 60;
            break;
        case SCHEDULER_PAUSE_DURATION:
            st->schedule_pause_until = cluster_service_get_unix_time(&tv) + pause_duration_seconds;
            break;
        case SCHEDULER_PAUSE_UNTIL:
            st->schedule_pause_until = pause_until_unix;
            break;
        case SCHEDULER_PAUSE_FOREVER_MODE:
            st->schedule_pause_until = SCHEDULER_PAUSE_FOREVER;
            break;
    }
    st->pause_blocks_auto_trigger = pause_blocks_auto_trigger;

    /* 3. send relay command */
    request_relay(channel, target);
}

/* ═══════════════════════════════════════════════════════════════
   Main tick
   ═══════════════════════════════════════════════════════════════ */

void scheduler_tick(uint32_t monotonic_ms, uint32_t unix_time, uint8_t time_valid)
{
    for (uint8_t i = 0; i < SCHEDULER_CHANNEL_COUNT; i++) {
        uint8_t ch = i + 1;
        channel_scheduler_state_t *st = &g_scheduler_state[i];

        /* ── Auto Hold expiry check ── */
        if (st->auto_hold.session != 0 &&
            time_after_or_equal(monotonic_ms, st->auto_hold.until_ms)) {
            auto_hold_finish(ch, unix_time, time_valid);
        }

        /* ── Auto Hold active: override scheduler ── */
        if (st->auto_hold.session != 0) {
            uint8_t desired = st->auto_hold.target_state;
            if (relay_actual_state(ch) != desired) {
                request_relay(ch, desired);
            }
            continue;
        }

        /* ── Scheduler disabled ── */
        if (!st->schedule_enabled) continue;

        /* ── Pause active ── */
        if (st->schedule_pause_until != 0 &&
            unix_time < st->schedule_pause_until) continue;

        /* ── Time invalid ── */
        if (!time_valid) continue;

        /* ── Interval match ── */
        uint8_t wd = weekday_from_unix(unix_time);
        uint16_t minute = minute_of_day(unix_time);
        daily_schedule_t *day = &g_weekly_schedule[i].days[wd];

        uint8_t desired = interval_match(day, minute) ? 1 : 0;
        if (relay_actual_state(ch) != desired) {
            request_relay(ch, desired);
        }
    }
    /* Persist: RTC backup every tick, Flash weekly schedule every 60s */
    scheduler_save_to_flash();
    scheduler_save_weekly_to_flash();
}
