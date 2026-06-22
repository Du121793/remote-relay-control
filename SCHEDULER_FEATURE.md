# 繼電器安全控制 — 排程器與 Auto Hold 融合實作規格 v4

## 1. 核心目標

排程器用來控制每一路 relay 在每週固定時間區間自動 ON / OFF，同時能與以下來源共存：

```text
使用者手動控制
後端感測器 Auto Hold
安全保護
```

本版採用：

```text
Auto Hold：後端要求 MCU 暫時維持某一路 relay 在指定狀態一段時間。
```

後端感測器判斷層可以只產生：

```text
channel
target：ON / OFF
duration_seconds
```

但後端送到 MCU 的 Auto Hold 命令必須補齊：

```text
channel
target
duration_seconds
session
command_ts
reason
```

核心要求：

```text
排程器不直接寫 GPIO。
Auto Hold 不直接寫 GPIO。
所有 ON / OFF 都必須送進relay protection check 與 control queue。
GPIO 最終只能由 worker 寫入。
pause / Auto Hold 不能影響安全保護。
Auto Hold 有效期間，排程器不干預該 channel。
```

安全保護永遠最高優先：

```text
強制停止
急停
30A 上限
ADC 飽和
relay 黏住
過流
故障鎖定
冷卻時間
```

---

## 2. 控制責任分工

### 2.1 感測器 trigger 負責什麼

感測器 trigger 只需要表達：

```text
哪一路 channel
想要變成 ON 或 OFF
持續多久
```

例如：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600
}
```

這是後端內部感測器判斷結果，不建議直接裸送給 MCU。

---

### 2.2 後端負責補齊什麼

後端在送給 MCU 前，補齊：

```text
session
command_ts
reason
```

完整 Auto Hold 命令：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 1782196200,
  "reason": "溶氧過低，自動增氧中"
}
```

---

### 2.3 MCU 由誰統一管理控制

本規格建議使用一個 `control_task` 統一管理每個 channel 的控制狀態。

也就是：

```text
不是 scheduler_task 和 auto_hold_task 兩個 task 搶控制。
而是 control_task 統一判斷目前該 channel 應該聽誰的。
```

Auto Hold 的開關動作不交給排程器任務執行。

正確流程：

```text
後端 Auto Hold 命令
  ↓
MCU Auto Hold handler
  ↓
更新 auto_hold 狀態
  ↓
control_task 判斷 desired_state
  ↓
relay protection check
  ↓
control queue
  ↓
worker
  ↓
GPIO
```

Scheduler 只負責計算：

```text
如果照 weekly schedule，現在應該 ON 還是 OFF
```

Auto Hold handler 只負責更新：

```text
session
target_state
start_state
until_ms
reason
```

真正決定 relay 要不要改狀態的是：

```text
control_task
```

Auto Hold 有效時：

```text
control_task 看到 auto_hold.session != 0
→ desired_state = auto_hold.target_state
→ scheduler 不介入
```

沒有 Auto Hold 時，control_task 才檢查：

```text
schedule_enabled
schedule_pause_until
time_valid
weekly schedule
```

也就是：

```text
Auto Hold 是一個控制來源。
Scheduler 是另一個控制來源。
control_task 是唯一的控制決策點。
worker 是唯一寫 GPIO 的地方。
```

---

## 3. 名詞定義

### 3.1 Schedule

排程器依照每週固定時段送 ON / OFF 請求。

```text
schedule_enabled = 1 → 排程器啟用
schedule_enabled = 0 → 排程器關閉
```

排程器不直接寫 GPIO。

---

### 3.2 Pause

Pause 是使用者手動控制後，暫時不讓排程器重新接管。

Pause 也可以同步禁止後端感測器 Auto Hold。

```text
pause_blocks_auto_trigger = 0
→ 只暫停排程器，感測器 Auto Hold 仍可介入

pause_blocks_auto_trigger = 1
→ 暫停排程器，且禁止感測器 Auto Hold
```

預設：

```text
pause_blocks_auto_trigger = 0
```

---

### 3.3 Auto Hold

Auto Hold 是後端感測器送給 MCU 的短期控制租約。

範例：

```text
溶氧過低
→ 後端要求 channel 3 ON 10 分鐘
→ 後端每 5 分鐘用同 session 續約一次
→ 感測器恢復後送 duration_seconds = 0 結束
→ 若後端斷線，10 分鐘後 MCU 自動到期收斂
```

Auto Hold 不是安全保護。

Auto Hold 不是使用者手動控制。

Auto Hold 有效時：

```text
排程器不對該 channel 送 ON
排程器不對該 channel 送 OFF
設備狀態由 Auto Hold target_state 暫時維持
```

---

## 4. 排程器狀態

每個 channel 的排程器只有兩個本體狀態：

```text
schedule_enabled = 1 → 排程器啟用
schedule_enabled = 0 → 排程器關閉
```

沒有每日 enabled / disabled。

每日只是一份 ON 區間表：

```text
某天 intervals 為空 → 該天全天排程 OFF
```

---

## 5. Pause 狀態

### 5.1 欄位

```c
#define SCHEDULE_PAUSE_FOREVER 0xFFFFFFFFu

uint32_t schedule_pause_until;
uint8_t  pause_blocks_auto_trigger;
```

`pause_blocks_auto_trigger`：

```text
0 = 只暫停排程器，感測器 Auto Hold 仍可介入
1 = 暫停排程器，且禁止感測器 Auto Hold
```

pause 有效時：

```text
排程器不送 ON
排程器不送 OFF
排程器不干預設備
```

若 `pause_blocks_auto_trigger = 1` 且 pause 尚未到期：

```text
後端 Auto Hold 不得建立
後端 Auto Hold 不得續約
MCU 收到 Auto Hold 也必須拒絕
```

---

### 5.2 schedule_enabled = 0 時的 pause_blocks_auto_trigger

即使 `schedule_enabled = 0`，`schedule_pause_until + pause_blocks_auto_trigger` 仍可作為 Auto Hold 禁止條件。

原因：

```text
排程器 disabled 只代表排程器不接管設備。
不代表使用者不能禁止感測器 Auto Hold。
```

因此使用者手動控制時，若勾選：

```text
同步禁止感測器自動觸發
```

即使排程器已關閉，仍要寫入：

```c
schedule_pause_until = user_selected_until;
pause_blocks_auto_trigger = 1;
```

此時 `schedule_pause_until` 不用來暫停排程器，而是作為：

```text
Auto Hold block until
```

判斷仍然使用：

```c
bool auto_hold_blocked_by_user_pause =
    (now_time < schedule_pause_until) &&
    (pause_blocks_auto_trigger == 1);
```

---

### 5.3 Pause 時間來源

`schedule_pause_until` 使用系統目前選定的 cluster time / RTC time 判斷。

時間來源由 time sync service 決定，優先序：

```text
1. 後端時間
2. 外網 NTP
3. 預設閘道
4. 叢集設備 master
```

只要 time sync service 提供有效時間，pause 判斷維持：

```c
now_time < schedule_pause_until
```

---

## 6. Auto Hold 狀態

### 6.1 欄位

每個 channel 只保存一個 Auto Hold。

```c
typedef struct {
    uint32_t session;       // 0 = 沒有 Auto Hold；非 0 = Auto Hold 有效
    uint8_t  target_state;  // ON / OFF
    uint8_t  start_state;   // 第一次接到此 session 時，設備當下狀態
    uint32_t until_ms;      // 使用 MCU monotonic time，非 RTC 絕對時間
    char     reason[64];    // 顯示 / 紀錄用，不參與核心判斷
} auto_hold_state_t;
```

每個 channel 另記錄：

```c
uint32_t last_auto_hold_command_ts;
```

完整 channel 狀態：

```c
typedef struct {
    uint8_t  schedule_enabled;

    uint32_t schedule_pause_until;
    uint8_t  pause_blocks_auto_trigger;

    uint32_t last_auto_hold_command_ts;

    auto_hold_state_t auto_hold;
} channel_scheduler_state_t;
```

---

### 6.2 session

```text
session = 0
→ 沒有 Auto Hold

session != 0
→ 有 Auto Hold
```

同一輪 Auto Hold 續約必須使用相同 session。

不同輪 Auto Hold 必須使用不同 session。

session 不一定要單調遞增，真正用來判斷封包新舊的是：

```text
command_ts
```

---

### 6.3 command_ts

Auto Hold command 必須帶 `command_ts`。

每個 channel 記錄：

```c
uint32_t last_auto_hold_command_ts;
```

規則：

```text
若 command_ts < last_auto_hold_command_ts：
    視為舊封包
    MCU 必須忽略該命令
    不得建立 Auto Hold
    不得續約 Auto Hold
    不得清除 Auto Hold
```

若：

```text
command_ts >= last_auto_hold_command_ts
```

則進入命令處理流程。

命令處理完成後，不論結果是：

```text
接受
拒絕
因 pause_blocks_auto_trigger 擋下
因 duration 無效擋下
因 target mismatch 擋下
```

都更新：

```c
last_auto_hold_command_ts = command_ts;
```

原因：

```text
避免較新的拒絕命令之後，又被較舊封包覆蓋。
```

第一版允許：

```text
command_ts == last_auto_hold_command_ts
```

視為重送封包。

但同 timestamp 重送不得改變 session / target / duration 語意。

---

### 6.4 start_state

`start_state` 只在第一次接受新 session 時記錄。

```text
同一個 session 續約時，不可更新 start_state。
```

用途：

```text
Auto Hold 到期且排程器不能接手時，
回到第一次接到該 session 當下的設備狀態。
```

---

### 6.5 target_state

同一個 session 只能代表同一個控制方向。

同 session 續約時：

```text
session 必須相同
target 必須與 auto_hold.target_state 相同
只更新 until_ms / reason
不可更新 start_state
不可更新 target_state
```

若同 session 收到不同 target：

```text
拒絕命令
記錄 SESSION_TARGET_MISMATCH
不更新 until_ms
不更新 target_state
不更新 start_state
```

若後端要從 ON hold 改成 OFF hold：

```text
必須建立新 session
```

---

### 6.6 until_ms

Auto Hold 是「持續多久」，不是「排程時間」。

因此使用 MCU monotonic timer，例如：

```text
system uptime ms
FreeRTOS tick
HAL_GetTick()
```

不要使用 RTC 絕對時間判斷 Auto Hold 到期。

原因：

```text
RTC 未同步時，排程器不能自動 ON
但後端感測器要求 ON 10 分鐘，仍然應該可以正常到期
```

---

### 6.7 duration_seconds 限制

```c
#define AUTO_HOLD_MIN_SECONDS 1u
#define AUTO_HOLD_MAX_SECONDS 1800u
```

規則：

```text
duration_seconds = 0
→ 結束同 session 的 Auto Hold

1 <= duration_seconds <= 1800
→ 合法

duration_seconds > 1800
→ 拒絕命令，不建立、不續約 Auto Hold
```

實作時必須先檢查再乘：

```c
if (cmd->duration_seconds > AUTO_HOLD_MAX_SECONDS) {
    reject_auto_hold(cmd);
    return;
}

uint32_t duration_ms = cmd->duration_seconds * 1000u;
hold->until_ms = monotonic_ms() + duration_ms;
```

---

### 6.8 reason

`reason` 取代原本命令中的 type / trigger。

MCU 不需要解析 reason。

reason 只用於：

```text
狀態顯示
log
後端追蹤
前端顯示目前為何被 Auto Hold
```

建議第一版 MCU 不依賴 reason 做邏輯判斷。

真正用來辨識同一輪控制的是：

```text
session
```

封包新舊由：

```text
command_ts
```

判斷。

reason 防呆：

```text
最多 63 bytes
不可包含 ASCII control characters
必須 NUL 結尾
UTF-8 合法性由後端保證
```

---

## 7. 排程資料結構

### 7.1 時間區間

```c
typedef struct {
    uint16_t start_minute;   // 0~1439
    uint16_t end_minute;     // 1~1440
} schedule_interval_t;
```

使用半開區間：

```text
[start_minute, end_minute)
```

時間換算：

```text
00:00 = 0
08:30 = 510
12:00 = 720
24:00 = 1440
```

---

### 7.2 每日排程

```c
#define MAX_INTERVALS_PER_DAY 8

typedef struct {
    uint8_t interval_count;
    schedule_interval_t intervals[MAX_INTERVALS_PER_DAY];
} daily_schedule_t;
```

規則：

```text
interval_count = 0 → 該天全天 OFF
interval_count > 0 → 根據 intervals 判斷是否 ON
```

---

### 7.3 每週排程

```c
typedef struct {
    uint8_t channel;
    daily_schedule_t days[7];   // 0=Mon, 1=Tue, ... 6=Sun
} weekly_schedule_t;
```

---

## 8. 排程輸入檢查

同一個 channel 同一天只能有一份 daily schedule。

同一天內可以有多個 ON 區間，但不可重疊。

重疊判斷：

```c
bool is_overlap(uint16_t a_start, uint16_t a_end,
                uint16_t b_start, uint16_t b_end)
{
    return (a_start < b_end) && (b_start < a_end);
}
```

檢查規則：

```text
1. interval_count <= MAX_INTERVALS_PER_DAY
2. start_minute < end_minute
3. 0 <= start_minute < 1440
4. 1 <= end_minute <= 1440
5. 區間依 start_minute 排序
6. 相鄰區間不可重疊
```

相鄰不算重疊：

```text
08:00 ~ 12:00
12:00 ~ 14:00
```

重疊範例：

```text
08:00 ~ 12:00
10:00 ~ 14:00
```

---

## 9. 跨午夜處理

使用者若設定：

```text
星期一 22:00 ~ 星期二 02:00
```

儲存前拆成：

```text
星期一 22:00 ~ 24:00
星期二 00:00 ~ 02:00
```

MCU 端只處理單日內區間。

---

## 10. control_task 決策流程

建議實作成一個 `control_task`，週期性掃描所有 channel。

`control_task` 負責：

```text
1. 檢查 Auto Hold 是否到期
2. 檢查目前是否有 Auto Hold
3. 若有 Auto Hold，使用 Auto Hold target 作為 desired_state
4. 若沒有 Auto Hold，才檢查排程器是否可接管
5. 若排程器可接管，使用 scheduler 計算出的 ON/OFF 作為 desired_state
6. 比對 relay_actual_state 與 desired_state
7. 不一致時，送控制請求到 relay protection check / control queue
```

偽程式：

```c
void control_channel_tick(uint8_t channel)
{
    channel_scheduler_state_t *state = &scheduler_state[channel];

    if (state->auto_hold.session != 0 &&
        time_after_or_equal(monotonic_ms(), state->auto_hold.until_ms)) {
        auto_hold_finish(channel, state->auto_hold.session);
    }

    if (state->auto_hold.session != 0) {
        uint8_t desired = state->auto_hold.target_state;

        if (relay_actual_state(channel) != desired) {
            request_channel_state_from_auto_hold(channel, desired);
        }

        return;
    }

    if (!state->schedule_enabled) {
        return;
    }

    if (now_time < state->schedule_pause_until) {
        return;
    }

    if (!time_valid) {
        return;
    }

    uint8_t desired = scheduler_get_desired_state(channel);

    if (relay_actual_state(channel) != desired) {
        request_channel_state_from_scheduler(channel, desired);
    }
}
```

注意：

```text
control_task 可以是一個 FreeRTOS task。
Scheduler / Auto Hold 不一定要各自獨立成 task。
Scheduler / Auto Hold 可以只是 control_task 內部呼叫的邏輯模組。
```

---

## 11. 排程器計算流程

排程器每次掃描 channel 時：

```text
1. 檢查 schedule_enabled
2. 檢查 schedule_pause_until
3. 檢查 Auto Hold 是否有效
4. 檢查 RTC / cluster time 是否有效
5. 取得 weekday 與 minute_of_day
6. 檢查今天 intervals 是否命中
7. 發送 ON / OFF 請求給控制層
```

偽程式：

```c
void scheduler_tick(void)
{
    for each channel {

        channel_scheduler_state_t *state = &scheduler_state[channel];

        if (!state->schedule_enabled) {
            continue;
        }

        if (now_time < state->schedule_pause_until) {
            continue;
        }

        if (state->auto_hold.session != 0) {
            continue;
        }

        if (!time_valid) {
            continue;
        }

        uint8_t weekday = get_weekday(now_time);
        uint16_t minute = get_minute_of_day(now_time);

        daily_schedule_t *day = &weekly_schedule[channel].days[weekday];

        bool should_on = false;

        for (int i = 0; i < day->interval_count; i++) {
            if (minute >= day->intervals[i].start_minute &&
                minute <  day->intervals[i].end_minute) {
                should_on = true;
                break;
            }
        }

        request_channel_state_from_scheduler(
            channel,
            should_on ? ON : OFF
        );
    }
}
```

pause 或 Auto Hold 有效時：

```text
排程器不做 ON
排程器不做 OFF
排程器不干預設備
```

---

## 12. Auto Hold tick 流程

Auto Hold 需要獨立 tick 檢查是否到期。

```c
void auto_hold_tick(void)
{
    for each channel {

        auto_hold_state_t *hold = &scheduler_state[channel].auto_hold;

        if (hold->session == 0) {
            continue;
        }

        if (time_after_or_equal(monotonic_ms(), hold->until_ms)) {
            auto_hold_finish(channel, hold->session);
        }
    }
}
```

`time_after_or_equal()` 必須能處理 `uint32_t` overflow。

```c
static bool time_after_or_equal(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}
```

---

## 13. Auto Hold 命令格式

### 12.1 感測器 trigger 到後端

感測器 trigger 可以只提供：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600
}
```

---

### 12.2 後端送 MCU

後端送 MCU 時必須補齊：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 1782196200,
  "reason": "溶氧過低，自動增氧中"
}
```

欄位說明：

```text
channel
→ relay channel

target
→ ON / OFF

duration_seconds
→ 持續多久
→ 0 代表結束同 session 的 Auto Hold

session
→ 本次後端控制識別碼
→ 同一輪續約必須使用相同 session

command_ts
→ 後端命令時間戳
→ MCU 用來判斷同 channel Auto Hold 命令新舊

reason
→ 顯示 / log 用
→ MCU 不依賴 reason 做核心判斷
```

---

### 12.3 不使用 type

不使用：

```json
{
  "type": "AUTO_HOLD"
}
```

命令類型由 API endpoint 或 MQTT topic 決定。

範例：

```text
relay/channel/3/auto-hold
```

或：

```http
POST /api/relay/channels/:channel/auto-hold
```

---

## 14. MCU 接收 Auto Hold 流程

### 13.1 基本檢查

```text
1. 檢查 session != 0
2. 檢查 command_ts 是否舊封包
3. 檢查 duration_seconds
4. 檢查 pause_blocks_auto_trigger
5. 檢查是否已有不同 session 的 Auto Hold
6. 檢查同 session target 是否一致
7. 建立 / 續約 / 清除 Auto Hold
```

第一版 MCU 不負責 weekly auto trigger policy 時間窗判斷。

weekly_allowed_windows 由後端依自己的時間判斷。

MCU 只做最後防呆：

```text
若 pause_blocks_auto_trigger = 1 且 pause 有效，拒絕 Auto Hold。
```

---

### 13.2 偽程式

```c
void handle_auto_hold_command(auto_hold_cmd_t *cmd)
{
    channel_scheduler_state_t *state = &scheduler_state[cmd->channel];
    auto_hold_state_t *hold = &state->auto_hold;

    if (cmd->session == 0) {
        return;
    }

    if (cmd->command_ts < state->last_auto_hold_command_ts) {
        return;
    }

    bool handled = true;

    if (cmd->duration_seconds == 0) {
        auto_hold_clear_if_same_session(cmd->channel, cmd->session);
        state->last_auto_hold_command_ts = cmd->command_ts;
        return;
    }

    if (cmd->duration_seconds > AUTO_HOLD_MAX_SECONDS) {
        reject_auto_hold(cmd, "DURATION_TOO_LONG");
        state->last_auto_hold_command_ts = cmd->command_ts;
        return;
    }

    if (now_time < state->schedule_pause_until &&
        state->pause_blocks_auto_trigger) {
        reject_auto_hold(cmd, "BLOCKED_BY_USER_PAUSE");
        state->last_auto_hold_command_ts = cmd->command_ts;
        return;
    }

    if (hold->session != 0 && hold->session != cmd->session) {
        reject_auto_hold(cmd, "CHANNEL_BUSY");
        state->last_auto_hold_command_ts = cmd->command_ts;
        return;
    }

    if (hold->session == cmd->session &&
        hold->target_state != cmd->target) {
        reject_auto_hold(cmd, "SESSION_TARGET_MISMATCH");
        state->last_auto_hold_command_ts = cmd->command_ts;
        return;
    }

    if (hold->session == 0) {
        hold->start_state = get_current_relay_state(cmd->channel);
        hold->session = cmd->session;
        hold->target_state = cmd->target;
    }

    uint32_t duration_ms = cmd->duration_seconds * 1000u;

    hold->until_ms = monotonic_ms() + duration_ms;
    copy_reason(hold->reason, cmd->reason);

    state->last_auto_hold_command_ts = cmd->command_ts;

    request_channel_state_from_auto_hold(cmd->channel, hold->target_state);
}
```

---

### 13.3 新 session

條件：

```text
auto_hold.session == 0
command.session != 0
duration_seconds > 0
duration_seconds <= 1800
command_ts >= last_auto_hold_command_ts
```

MCU 流程：

```text
1. 記錄目前設備狀態為 start_state
2. 寫入 session
3. 寫入 target_state
4. 寫入 reason
5. until_ms = monotonic_ms + duration_seconds
6. 發送 target_state 給relay protection check
```

---

### 13.4 相同 session 續約

條件：

```text
auto_hold.session == command.session
command.target == auto_hold.target_state
duration_seconds > 0
duration_seconds <= 1800
command_ts >= last_auto_hold_command_ts
```

MCU 只更新：

```text
until_ms
reason
last_auto_hold_command_ts
```

MCU 不可更新：

```text
start_state
target_state
```

---

### 13.5 不同 session

若 Auto Hold 已有效：

```text
auto_hold.session != 0
```

又收到不同 session：

```text
command.session != auto_hold.session
```

第一版拒絕：

```text
CHANNEL_BUSY
```

不支援新 session 覆蓋舊 session。

---

### 13.6 duration_seconds = 0

`duration_seconds = 0` 代表結束 Auto Hold。

但必須符合：

```text
command.session == auto_hold.session
command_ts >= last_auto_hold_command_ts
```

若 session 不同：

```text
忽略
```

原因：

```text
避免舊封包延遲到達，誤清掉新的 Auto Hold。
```

---

## 15. Auto Hold 有效期間行為

Auto Hold 有效期間，MCU 不需要高頻重送 target_state。

MCU 只在以下情況送 target_state：

```text
1. 新 Auto Hold 建立
2. 後端合法重送同 session target
3. 同 session 合法續約後需要確認 target
4. Auto Hold 結束，需要恢復 start_state
5. Auto Hold 結束，交回排程器重新判斷
```

若安全保護改變 relay 狀態：

```text
安全保護永遠優先
Auto Hold 不得強制覆蓋安全結果
Auto Hold 不因安全保護阻擋而自動失效
Auto Hold 可維持到 until_ms 到期
```

如果 Auto Hold target 被relay protection check阻擋：

```text
Auto Hold 仍可維持到時間結束
期間排程器仍不干預
但 Auto Hold 不得反覆高頻重送 target 覆蓋安全結果
```

---

## 16. Auto Hold 結束 / 到期收斂

Auto Hold 結束來源：

```text
1. duration_seconds = 0
2. until_ms 到期
3. 使用者手動控制
4. 安全保護強制中斷
```

### 15.1 正常到期或後端 clear

流程：

```text
1. 檢查 session 是否一致
2. 保存 start_state
3. 清除 auto_hold.session
4. 清除 reason
5. 若 schedule_enabled = 1 且 pause 無效且 time_valid：
   → 交還排程器重新判斷
6. 否則：
   → 發送 start_state 給relay protection check
```

偽程式：

```c
void auto_hold_finish(uint8_t channel, uint32_t session)
{
    channel_scheduler_state_t *state = &scheduler_state[channel];
    auto_hold_state_t *hold = &state->auto_hold;

    if (hold->session == 0) {
        return;
    }

    if (hold->session != session) {
        return;
    }

    uint8_t restore_state = hold->start_state;

    hold->session = 0;
    hold->target_state = OFF;
    hold->start_state = OFF;
    hold->until_ms = 0;
    hold->reason[0] = '\0';

    if (state->schedule_enabled &&
        !(now_time < state->schedule_pause_until) &&
        time_valid) {
        scheduler_evaluate_channel_once(channel);
    } else {
        request_channel_state_from_auto_hold_restore(channel, restore_state);
    }
}
```

---

### 15.2 使用者手動控制中斷 Auto Hold

使用者手動 ON / OFF 時：

```text
1. 清除 Auto Hold
2. 寫入 schedule_pause_until
3. 寫入 pause_blocks_auto_trigger
4. 發送 USER ON/OFF 請求
5. 命令進relay protection check
```

重點：

```text
使用者手動控制永遠高於 Auto Hold。
使用者手動控制時清 Auto Hold 不需要恢復 start_state。
因為使用者已經明確指定新的 ON / OFF。
```

本版不記錄 cancelled_session。

因此：

```text
使用者手動清除 Auto Hold 後，不代表永久禁止同 session 再次介入。
若後端之後再次送同 session 且 command_ts 較新，MCU 可重新處理。
若使用者希望禁止感測器再次介入，必須勾選同步禁止感測器自動觸發。
```

---

### 15.3 安全保護中斷 Auto Hold

若安全保護觸發，例如過流、ADC 飽和、強制停止：

```text
1. 安全保護立即生效
2. worker 依安全保護關閉 relay 或鎖定
3. Auto Hold 可繼續存在到 until_ms
4. Auto Hold 不恢復 start_state
5. Auto Hold 不得覆蓋安全保護結果
```

安全保護解除後，是否恢復排程器由安全策略決定。

---

## 17. 使用者手動控制流程

使用者手動 ON / OFF 時，前端顯示：

```text
1. 暫停排程器到此週期結束
2. 暫停排程器到 1 小時後
3. 暫停排程器到 1 天後
4. 暫停排程器到自訂時間
5. 暫停直到手動恢復
```

勾選項：

```text
同步禁止感測器自動觸發
```

預設不勾選。

固定流程：

```text
1. 使用者按 ON / OFF
2. 前端要求選擇排程器暫停時間
3. 前端取得 pause_blocks_auto_trigger 勾選值
4. 以原子流程執行：
   - 清除該 channel 目前的 Auto Hold
   - 寫入 schedule_pause_until
   - 寫入 pause_blocks_auto_trigger
   - 發送使用者 ON / OFF 控制命令
5. 命令進relay protection check
```

此流程不因 `schedule_enabled = 0` 省略 `pause_blocks_auto_trigger`。

原因：

```text
schedule_enabled = 0 時，schedule_pause_until + pause_blocks_auto_trigger
仍可作為 Auto Hold 禁止條件。
```

---

### 16.1 排程 ON 中，使用者手動 OFF

```text
排程：08:00 ~ 12:00 ON
現在：09:00
使用者手動 OFF
選擇：暫停到此週期結束
```

結果：

```text
清除 Auto Hold
設備 OFF
schedule_pause_until = 12:01
pause_blocks_auto_trigger = 使用者勾選值
09:00 ~ 12:01 排程器不再嘗試 ON
若 pause_blocks_auto_trigger = 1，Auto Hold 也不得建立
```

---

### 16.2 排程器 disabled 時，使用者手動控制

如果：

```text
schedule_enabled = 0
```

使用者手動 ON / OFF：

```text
1. 清除 Auto Hold
2. 寫入 schedule_pause_until
3. 寫入 pause_blocks_auto_trigger
4. 發送手動 ON / OFF 控制命令
```

說明：

```text
排程器 disabled 時，schedule_pause_until 不用來暫停排程器。
但如果 pause_blocks_auto_trigger = 1，
它仍用來表示 Auto Hold 禁止期限。
```

---

## 18. 後端自動觸發流程

### 17.1 核心原則

後端感測器只送 Auto Hold。

不要再分開送：

```text
SET_RELAY_STATE
SET_SCHEDULER_SUPPRESS
AUTO_TRIGGER_RENEW
AUTO_TRIGGER_CLEAR
```

改成統一送：

```text
target + duration_seconds + session + command_ts + reason
```

---

### 17.2 溶氧過低範例

```text
1. 後端偵測溶氧過低
2. 後端決定要讓增氧設備 ON 10 分鐘
3. 後端產生 session = 12345
4. 後端產生 command_ts
5. 後端送 Auto Hold：ON / 600 秒 / session 12345 / command_ts / reason
6. MCU 第一次接受 session 12345 時，記錄目前設備狀態 start_state
7. MCU 送 ON 請求進relay protection check
8. 後端每 5 分鐘如果溶氧仍低，重送同 session 的 600 秒續約
9. 若溶氧恢復，後端送同 session 且 duration_seconds = 0
10. 若後端斷線，MCU 到期後自動收斂
```

---

### 17.3 同 session 續約

後端每 5 分鐘續約一次：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 1782196500,
  "reason": "溶氧過低，自動增氧中"
}
```

MCU 行為：

```text
session 相同
target 相同
command_ts 不舊
duration 合法
→ 只更新 until_ms / reason / last_auto_hold_command_ts
→ 不更新 start_state
→ 不更新 target_state
```

---

### 17.4 感測器恢復

後端送：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 0,
  "session": 12345,
  "command_ts": 1782196800,
  "reason": "溶氧恢復，結束自動增氧"
}
```

MCU 行為：

```text
若 session 相同且 command_ts 不舊：
    結束 Auto Hold
    若排程器可接手 → 交回排程器
    否則 → 回到 start_state

若 session 不同：
    忽略

若 command_ts 舊：
    忽略
```

---

## 19. 外來觸發允許排程

外來觸發允許排程只決定：

```text
目前是否允許後端感測器建立 Auto Hold？
```

它不直接控制 relay。

後端可提供三種模式：

```text
always_allow
always_block
weekly_allowed_windows
```

說明：

```text
always_allow
→ 永遠允許感測器 Auto Hold

always_block
→ 永遠禁止感測器 Auto Hold，只告警

weekly_allowed_windows
→ 依照每週允許時段判斷是否允許 Auto Hold
```

第一版建議外來觸發允許排程由後端實作。

MCU 第一版不負責 weekly policy 時間窗判斷。

MCU 只做最後防呆：

```text
若 pause_blocks_auto_trigger = 1 且 pause 有效，MCU 必須拒絕 Auto Hold。
```

---

## 20. 前端排程設定 UI

前端可以使用時鐘或 24 小時 timeline 設定某一天 ON 區間。

操作：

```text
1. 選擇 channel
2. 選擇星期
3. 在時鐘 / timeline 上新增 ON 區間
4. 前端即時檢查同日區間是否重疊
5. 儲存後，後端再次檢查
```

複製到其他星期：

```text
將此日排程覆寫到：
[ ] 星期一
[ ] 星期二
[ ] 星期三
[ ] 星期四
[ ] 星期五
[ ] 星期六
[ ] 星期日
```

行為：

```text
覆寫，不合併
```

---

## 21. 優先權

整體優先權：

```text
1. 基本安全保護
2. 使用者手動控制
3. 使用者暫停且 pause_blocks_auto_trigger = true
4. 外來觸發允許排程
5. 後端 Auto Hold
6. 使用者暫停排程器
7. 一般設備排程器
```

白話：

```text
安全保護永遠最高。
使用者手動控制會清除 Auto Hold，避免舊後端控制到期後影響手動狀態。
使用者若明確禁止感測器自動觸發，後端只能告警，MCU 也拒絕 Auto Hold。
若使用者沒有禁止自動觸發，後端再檢查外來觸發允許排程。
Auto Hold 有效期間，排程器不干預設備。
沒有 pause / Auto Hold 時，才照設備排程控制。
```

---

## 22. API 設計

### 21.1 查詢一週設備排程

```http
GET /api/relay/channels/:channel/schedule-week
```

---

### 21.2 覆寫某一天設備排程

```http
PUT /api/relay/channels/:channel/schedule-week/:weekday
```

Body：

```json
{
  "intervals": [
    { "start_minute": 480, "end_minute": 720 },
    { "start_minute": 840, "end_minute": 1080 }
  ]
}
```

---

### 21.3 複製某一天排程到其他星期

```http
POST /api/relay/channels/:channel/schedule-week/:weekday/copy
```

Body：

```json
{
  "target_weekdays": [1, 2, 3, 4],
  "mode": "overwrite"
}
```

---

### 21.4 啟用 / 永久關閉設備排程器

```http
PATCH /api/relay/channels/:channel/scheduler/enabled
```

Body：

```json
{
  "enabled": false
}
```

---

### 21.5 暫停排程器 / 禁止 Auto Hold

```http
POST /api/relay/channels/:channel/scheduler/pause
```

Body：

```json
{
  "mode": "current_segment_end",
  "duration_seconds": 3600,
  "until": "2026-06-23T14:30:00+08:00",
  "pause_blocks_auto_trigger": false
}
```

`mode`：

```text
current_segment_end
duration
until
forever
```

---

### 21.6 恢復排程器暫停

```http
POST /api/relay/channels/:channel/scheduler/resume
```

效果：

```text
schedule_pause_until = 0
pause_blocks_auto_trigger = 0
```

---

### 21.7 使用者手動控制設備

```http
POST /api/relay/channels/:channel/control
```

Body：

```json
{
  "target": "OFF",
  "source": "USER",
  "pause_scheduler": {
    "mode": "CURRENT_SEGMENT_END",
    "pause_blocks_auto_trigger": false
  }
}
```

處理必須是原子流程：

```text
1. 清除 Auto Hold
2. 設定 schedule_pause_until
3. 設定 pause_blocks_auto_trigger
4. 發送 USER ON/OFF 請求
```

---

### 21.8 後端 Auto Hold

```http
POST /api/relay/channels/:channel/auto-hold
```

Body：

```json
{
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 1782196200,
  "reason": "溶氧過低，自動增氧中"
}
```

續約與結束都使用同一支 API。

結束：

```json
{
  "target": "ON",
  "duration_seconds": 0,
  "session": 12345,
  "command_ts": 1782196800,
  "reason": "溶氧恢復，結束自動增氧"
}
```

---

### 21.9 查詢排程器狀態

```http
GET /api/relay/channels/:channel/scheduler/status
```

回傳範例：

```json
{
  "channel": 3,
  "schedule_enabled": true,
  "pause_active": true,
  "schedule_pause_until": "2026-06-23T14:30:00+08:00",
  "pause_blocks_auto_trigger": false,
  "auto_hold_active": true,
  "auto_hold_target": "ON",
  "auto_hold_start_state": "OFF",
  "auto_hold_remaining_seconds": 420,
  "auto_hold_session": 12345,
  "auto_hold_last_command_ts": 1782196500,
  "auto_hold_reason": "溶氧過低，自動增氧中",
  "time_valid": true
}
```

---

## 23. 後端與 MCU 命令格式

### 22.1 設定 pause

```json
{
  "channel": 3,
  "until": 1782196200,
  "pause_blocks_auto_trigger": false
}
```

命令類型由 topic / API 決定，例如：

```text
relay/channel/3/scheduler/pause
```

---

### 22.2 清除 pause

```json
{
  "channel": 3
}
```

命令類型由 topic / API 決定，例如：

```text
relay/channel/3/scheduler/pause/clear
```

---

### 22.3 控制設備

```json
{
  "channel": 3,
  "target": "ON",
  "source": "USER"
}
```

命令類型由 topic / API 決定，例如：

```text
relay/channel/3/control
```

---

### 22.4 後端 Auto Hold

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 1782196200,
  "reason": "溶氧過低，自動增氧中"
}
```

命令類型由 topic / API 決定，例如：

```text
relay/channel/3/auto-hold
```

---

## 24. 時間有效性

排程器依賴 cluster time / RTC，因此需要 `time_valid`。

若時間無效：

```text
排程器不得自動 ON
UI 顯示：時間未同步，排程器暫停
```

Auto Hold 使用 monotonic duration，不依賴 RTC。

因此：

```text
RTC 無效時，Auto Hold 仍可接受、續約、到期
```

但仍須通過：

```text
安全保護
pause_blocks_auto_trigger
```

weekly_allowed_windows 由後端判斷，MCU 第一版不依賴本地 RTC 判斷 weekly policy。

---

## 25. 開機後行為

MCU 重啟後：

```text
1. 所有 relay 先保持 OFF
2. 檢查時間是否有效
3. 檢查排程資料 CRC 是否正確
4. 檢查 channel 是否故障鎖定
5. 檢查 schedule_pause_until 是否仍有效
6. 清除 Auto Hold
7. 清除 last_auto_hold_command_ts
8. 再決定是否依排程補開
```

Auto Hold 不建議跨重啟保存。

原因：

```text
Auto Hold 是短期租約
重啟後 monotonic time 已重置
若保存到 Flash，可能造成舊控制殘留
```

建議：

```text
重啟後 Auto Hold 一律清除
後端若感測器仍異常，會重新送新的 session
```

建議保留：

```text
boot_catch_up_enabled
```

表示：

```text
開機時如果目前剛好在 ON 區間，要不要自動補開
```

高風險設備預設不補開，一般循環設備可允許補開。

---

## 26. 儲存建議

MCU Flash 可保存：

```text
schedule_enabled
weekly_schedule
schedule_pause_until
pause_blocks_auto_trigger
```

Auto Hold 不建議保存到 Flash。

後端保存完整資料：

```text
設備排程規則
外來觸發允許排程
使用者操作紀錄
pause 紀錄
Auto Hold 紀錄
後端自動觸發紀錄
感測器判斷紀錄
設備狀態 history
```

建議加：

```text
magic number
version
CRC
sequence number
```

---

## 27. 測試案例

### 26.1 同一天排程重疊

```text
08:00 ~ 12:00
10:00 ~ 14:00
```

期待：

```text
拒絕儲存
回傳 SCHEDULE_OVERLAP
```

---

### 26.2 排程 ON 區間內使用者手動 OFF

```text
排程：08:00 ~ 12:00 ON
現在：09:00
使用者手動 OFF
選擇：暫停到此週期結束
```

期待：

```text
清除 Auto Hold
設備 OFF
schedule_pause_until = 12:01
09:00 ~ 12:01 排程器不再送 ON
```

---

### 26.3 排程器 disabled，使用者手動 OFF 並禁止 Auto Hold

```text
schedule_enabled = 0
使用者手動 OFF
勾選同步禁止感測器自動觸發
暫停 1 小時
溶氧仍過低
```

期待：

```text
清除 Auto Hold
schedule_pause_until = now + 1 hour
pause_blocks_auto_trigger = 1
設備 OFF
1 小時內 MCU 拒絕 Auto Hold
```

---

### 26.4 排程 OFF 區間內後端因溶氧過低 ON

```text
排程：08:00 ~ 12:00 ON
現在：13:00
設備 OFF
溶氧過低
後端送 ON 10 分鐘 session 12345
```

期待：

```text
MCU 記錄 start_state = OFF
MCU 建立 Auto Hold
MCU 發送 ON
10 分鐘內排程器不干預
後端每 5 分鐘可用同 session 續約
溶氧恢復後 duration_seconds = 0 結束
若排程器可接手 → 交回排程器
否則 → 回 start_state = OFF
```

---

### 26.5 設備原本 ON，後端送 ON Auto Hold

```text
設備目前 ON
後端因溶氧過低送 ON 10 分鐘
```

期待：

```text
MCU 記錄 start_state = ON
Auto Hold 有效期間排程器不干預
Auto Hold 結束後：
- 若排程器可接手 → 交回排程器
- 否則 → 回 start_state = ON
```

---

### 26.6 使用者禁止感測器自動觸發

```text
使用者暫停排程器 1 天
勾選同步禁止感測器自動觸發
2 小時後溶氧過低
```

期待：

```text
後端不得建立 Auto Hold
MCU 若收到 Auto Hold，也必須拒絕
後端只能告警
```

---

### 26.7 後端斷線

```text
溶氧過低
後端送 ON 10 分鐘 session 12345
後端突然斷線，不再續約
```

期待：

```text
10 分鐘內排程器不干預
10 分鐘後 Auto Hold 到期
若 schedule_enabled = 1 且 pause 無效且 time_valid → 交回排程器
否則 → 回 start_state
```

---

### 26.8 Auto Hold 存在時使用者手動 OFF

```text
後端因溶氧過低建立 Auto Hold
設備 ON
使用者手動 OFF
```

期待：

```text
手動流程先清除 Auto Hold
設定 pause / pause_blocks_auto_trigger
再發送 OFF
舊 Auto Hold 到期後不會再改變設備狀態
```

若使用者沒有勾選同步禁止：

```text
後端之後若再次送同 session 且 command_ts 較新，MCU 可重新處理。
```

若使用者有勾選同步禁止：

```text
MCU 會因 pause_blocks_auto_trigger 拒絕 Auto Hold。
```

---

### 26.9 舊 session clear 延遲到達

```text
session 100 建立 Auto Hold
session 100 已經結束
session 200 建立新的 Auto Hold
舊封包：duration_seconds = 0, session = 100 延遲到達
```

期待：

```text
若 command_ts 舊 → 忽略
若 command_ts 不舊但 session 不同 → 不清除 session 200
不得清除新的 Auto Hold
```

---

### 26.10 同 session 續約不可更新 start_state / target_state

```text
13:00 設備 OFF，收到 session 12345 ON 10 分鐘
13:05 設備已 ON，收到同 session 12345 續約 10 分鐘
```

期待：

```text
13:00 記錄 start_state = OFF、target_state = ON
13:05 不更新 start_state
13:05 不更新 target_state
只更新 until_ms / reason
Auto Hold 結束且無排程接手時，回 OFF
```

---

### 26.11 同 session 改 target

```text
session 12345 原本 target = ON
後端又送 session 12345 target = OFF
```

期待：

```text
MCU 拒絕
回報 SESSION_TARGET_MISMATCH
不更新 until_ms
不更新 start_state
不更新 target_state
```

---

### 26.12 duration 超過 30 分鐘

```text
後端送 duration_seconds = 3600
```

期待：

```text
MCU 拒絕
不建立 Auto Hold
不續約 Auto Hold
```

---

### 26.13 Auto Hold 被安全保護阻擋

```text
後端送 Auto Hold ON 10 分鐘
但 channel 正在過流鎖定或冷卻中
```

期待：

```text
安全保護優先
Auto Hold 不得覆蓋安全保護
Auto Hold 可維持到 until_ms
Auto Hold 有效期間排程器仍不干預
到期後依規則收斂
```

---

## 28. 最終定案摘要

### 27.1 Channel scheduler state

```c
typedef struct {
    uint32_t session;
    uint8_t  target_state;
    uint8_t  start_state;
    uint32_t until_ms;
    char     reason[64];
} auto_hold_state_t;

typedef struct {
    uint8_t  schedule_enabled;

    uint32_t schedule_pause_until;
    uint8_t  pause_blocks_auto_trigger;

    uint32_t last_auto_hold_command_ts;

    auto_hold_state_t auto_hold;
} channel_scheduler_state_t;
```

---

### 27.2 Weekly schedule

```c
#define MAX_INTERVALS_PER_DAY 8

typedef struct {
    uint16_t start_minute;
    uint16_t end_minute;
} schedule_interval_t;

typedef struct {
    uint8_t interval_count;
    schedule_interval_t intervals[MAX_INTERVALS_PER_DAY];
} daily_schedule_t;

typedef struct {
    uint8_t channel;
    daily_schedule_t days[7];
} weekly_schedule_t;
```

---

### 27.3 排程器檢查順序

```text
1. schedule_enabled
2. schedule_pause_until
3. auto_hold.session
4. time_valid
5. interval match
6. request ON/OFF
```

---

### 27.4 使用者手動控制順序

使用者手動控制必須是原子流程：

```text
1. 清除 Auto Hold
2. 寫入 schedule_pause_until
3. 寫入 pause_blocks_auto_trigger
4. 發送 USER ON/OFF
5. ON/OFF 進relay protection check
```

---

### 27.5 後端 Auto Hold 檢查順序

```text
1. 接收 target / duration_seconds / session / command_ts / reason
2. 若 session = 0，拒絕
3. 若 command_ts 舊，忽略
4. 若 duration_seconds = 0，且 session 相同，結束 Auto Hold
5. 若 pause_blocks_auto_trigger = 1 且 pause 有效，拒絕
6. 若 duration_seconds > 1800，拒絕
7. 若已有不同 session，拒絕
8. 若同 session 但 target 不同，拒絕
9. 若新 session，記錄 start_state 與 target_state
10. 寫入 until_ms / reason / last_auto_hold_command_ts
11. 發送 target_state 給relay protection check
12. 同 session 續約只更新 until_ms / reason，不更新 start_state / target_state
```

---

### 27.6 Auto Hold 到期收斂

```text
Auto Hold 到期時：
1. 清除 session
2. 清除 reason
3. 若 schedule_enabled = 1 且 pause 無效且 time_valid：
   → 交回排程器重新判斷
4. 否則：
   → 回到 start_state
```

---

### 27.7 最重要限制

```text
pause / Auto Hold 不能影響安全保護。
使用者手動控制必須清除 Auto Hold。
Auto Hold 使用 command_ts 防止舊封包覆蓋新狀態。
同 session 續約不可更新 start_state。
同 session 續約不可改 target_state。
duration_seconds 最大 1800 秒。
duration_seconds = 0 只有在 session 相同且 command_ts 不舊時才可清除 Auto Hold。
Auto Hold 有效期間排程器不干預設備。
Auto Hold 到期後，優先交還排程器；若排程器無法接手，才回 start_state。
schedule_enabled = 0 時，pause_blocks_auto_trigger 仍可作為 Auto Hold 禁止條件。
Auto Hold 有效期間不高頻重送 target_state。
安全保護永遠優先，Auto Hold 不得覆蓋安全保護。
同一 channel 的 Auto Hold、使用者手動控制、排程器控制，必須由控制層序列化處理。
任何來源的 ON/OFF 都不能繞過relay protection check。
GPIO 最終只能由 worker 寫入。
```

---

## 29. 與 v4 差異

本版補強：

```text
control_task 統一管理 Auto Hold / Scheduler / Pause
Auto Hold 與 Scheduler 不需要各自獨立成 task
command_ts
last_auto_hold_command_ts
同 session target 不可改
duration_seconds 最大 1800 秒
schedule_enabled = 0 時 pause_blocks_auto_trigger 仍可禁止 Auto Hold
time sync service / cluster time 用於 pause 判斷
weekly auto trigger policy 第一版由後端判斷
Auto Hold 不是高頻維持
Auto Hold 被安全保護阻擋時仍可維持到到期
感測器 trigger 可只送 channel / target / duration，後端補 session / command_ts / reason
```

本版仍不加入：

```text
cancelled_session
control_generation
```

語意：

```text
使用者手動清 Auto Hold 後，不代表永久禁止同 session 再介入。
若使用者要禁止感測器再次介入，必須勾選同步禁止感測器自動觸發。
```

---

## 30. 名詞修正說明

本版不使用「安全仲裁器」作為主要名詞，避免誤解成一個額外大型模組。

實作上可以拆成：

```text
control_task
→ 統一決定 desired_state 來源

relay protection check
→ 檢查安全保護、故障鎖定、冷卻、過流等條件

control queue
→ 序列化 relay 控制請求

relay worker
→ 唯一實際寫 GPIO 的地方
```

其中 `control_task` 可以同時處理：

```text
Auto Hold 到期
Auto Hold active 判斷
pause 判斷
scheduler desired_state 計算
relay_actual_state 比對
```

所以不需要兩個 task 分別管理 Auto Hold 與 Scheduler。
