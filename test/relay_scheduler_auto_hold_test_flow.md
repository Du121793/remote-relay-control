# 繼電器排程器與 Auto Hold — 完整測試流程

> 適用規格：`relay_scheduler_auto_hold_spec_v5`
>
> 測試目標：驗證 `control_task` 統一管理 `Scheduler / Pause / Auto Hold / relay_actual_state` 的設計是否正確，並確認所有 ON/OFF 都經由 `relay protection check → control queue → worker → GPIO`。

---

## 1. 測試總目標

本測試要驗證：

```text
1. Scheduler 只負責計算 weekly schedule 的 desired_state。
2. Auto Hold 只負責保存後端短期控制租約。
3. control_task 是唯一 desired_state 決策點。
4. worker 是唯一實際寫 GPIO 的地方。
5. Auto Hold 有效期間，Scheduler 不得干預該 channel。
6. Pause 有效期間，Scheduler 不得干預該 channel。
7. pause_blocks_auto_trigger = 1 且 pause 有效時，MCU 必須拒絕 Auto Hold。
8. command_ts 必須阻擋舊 Auto Hold 封包。
9. 同 session 續約不可更新 start_state / target_state。
10. duration_seconds 最大 1800 秒。
11. 使用者手動控制必須清除 Auto Hold。
12. Auto Hold 到期後，優先交還 Scheduler；若 Scheduler 無法接手，回到 start_state。
13. 安全保護永遠最高優先，Auto Hold 不得覆蓋安全保護。
14. schedule_enabled = 0 時，pause_blocks_auto_trigger 仍可作為 Auto Hold 禁止條件。
```

---

## 2. 測試環境

### 2.1 建議測試層級

```text
第一層：PC 模擬測試
- 不接真 relay
- 使用 mock relay state
- 使用 mock now_time / monotonic_ms
- 使用 mock control queue
- 驗證狀態機與控制請求

第二層：MCU 實機測試
- 接 LED 或低風險 relay
- 驗證 FreeRTOS task / queue / worker
- 驗證 GPIO 最終狀態
```

### 2.2 必要 mock / hook

```text
mock_now_time
mock_monotonic_ms
mock_time_valid
mock_relay_actual_state
mock_control_queue
mock_relay_protection_result
mock_worker_output
```

### 2.3 每次測試必記錄欄位

```text
test_id
channel
now_time
monotonic_ms
time_valid
schedule_enabled
schedule_pause_until
pause_blocks_auto_trigger
auto_hold.session
auto_hold.target_state
auto_hold.start_state
auto_hold.until_ms
last_auto_hold_command_ts
relay_actual_state
desired_state
control_source
queue_event
worker_output
expected_result
actual_result
PASS / FAIL
```

---

## 3. 預設測試資料

### 3.1 預設 channel

```text
channel = 3
```

### 3.2 預設狀態

```text
relay_actual_state = OFF
schedule_enabled = 1
schedule_pause_until = 0
pause_blocks_auto_trigger = 0
auto_hold.session = 0
last_auto_hold_command_ts = 0
time_valid = 1
```

### 3.3 預設 weekly schedule

```text
星期一：
08:00 ~ 12:00 ON
14:00 ~ 18:00 ON

其他時間：
OFF
```

minute：

```text
08:00 = 480
12:00 = 720
14:00 = 840
18:00 = 1080
```

---

## 4. 排程資料測試

### SCH-001：同一天相鄰區間允許

```text
輸入：
08:00 ~ 12:00
12:00 ~ 14:00

步驟：
1. 寫入 intervals。
2. 執行 validate_daily_schedule()。

預期：
PASS
不回傳 overlap
允許儲存
```

---

### SCH-002：同一天重疊區間拒絕

```text
輸入：
08:00 ~ 12:00
10:00 ~ 14:00

步驟：
1. 寫入 intervals。
2. 執行 validate_daily_schedule()。

預期：
FAIL
回傳 SCHEDULE_OVERLAP
不得儲存
```

---

### SCH-003：interval_count = 0 代表全天 OFF

```text
初始：
星期三 interval_count = 0
now_time = 星期三 10:00
relay_actual_state = ON
schedule_enabled = 1
time_valid = 1
auto_hold.session = 0
pause 無效

步驟：
1. 執行 control_task。

預期：
scheduler desired_state = OFF
relay_actual_state != OFF
送 OFF
```

---

### SCH-004：跨午夜拆分

```text
使用者輸入：
星期一 22:00 ~ 星期二 02:00

預期儲存：
星期一 22:00 ~ 24:00
星期二 00:00 ~ 02:00

預期：
MCU 端不得出現 start_minute > end_minute。
```

---

## 5. control_task 基本測試

### CTL-001：排程 ON 區間內自動 ON

```text
初始：
now_time = 星期一 09:00
relay_actual_state = OFF
schedule_enabled = 1
time_valid = 1
auto_hold.session = 0
pause 無效

步驟：
1. 執行 control_task。

預期：
scheduler desired_state = ON
送 ON 到 control queue
worker 最終輸出 ON
```

---

### CTL-002：排程 OFF 區間內自動 OFF

```text
初始：
now_time = 星期一 13:00
relay_actual_state = ON
schedule_enabled = 1
time_valid = 1
auto_hold.session = 0
pause 無效

步驟：
1. 執行 control_task。

預期：
scheduler desired_state = OFF
送 OFF 到 control queue
worker 最終輸出 OFF
```

---

### CTL-003：實際 relay 已一致時不重送

```text
初始：
now_time = 星期一 09:00
relay_actual_state = ON
scheduler desired_state = ON

步驟：
1. 執行 control_task。

預期：
relay_actual_state == desired_state
不送重複 ON
```

---

### CTL-004：schedule_enabled = 0 時 Scheduler 不接管

```text
初始：
schedule_enabled = 0
relay_actual_state = OFF
now_time = 星期一 09:00
time_valid = 1
auto_hold.session = 0

步驟：
1. 執行 control_task。

預期：
不計算 scheduler desired_state
不送 ON
relay 維持 OFF
```

---

### CTL-005：time_valid = 0 時 Scheduler 不自動 ON

```text
初始：
time_valid = 0
relay_actual_state = OFF
schedule_enabled = 1
auto_hold.session = 0
pause 無效

步驟：
1. 執行 control_task。

預期：
Scheduler 不自動 ON
不送排程 ON
```

---

## 6. Pause 測試

### PAUSE-001：pause 有效時 Scheduler 不干預

```text
初始：
now_time = 星期一 09:00
schedule_pause_until = 星期一 10:00
pause_blocks_auto_trigger = 0
relay_actual_state = OFF
auto_hold.session = 0

步驟：
1. 執行 control_task。

預期：
now_time < schedule_pause_until
Scheduler 不送 ON
relay 維持 OFF
```

---

### PAUSE-002：pause 到期後 Scheduler 恢復

```text
初始：
now_time = 星期一 10:01
schedule_pause_until = 星期一 10:00
schedule_enabled = 1
relay_actual_state = OFF
auto_hold.session = 0
time_valid = 1

步驟：
1. 執行 control_task。

預期：
pause 已到期
Scheduler 恢復
若目前在 ON 區間，送 ON
```

---

### PAUSE-003：pause_blocks_auto_trigger = 1 時拒絕 Auto Hold

```text
初始：
now_time = 星期一 09:00
schedule_pause_until = 星期一 10:00
pause_blocks_auto_trigger = 1
auto_hold.session = 0

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 100,
  "command_ts": 1000,
  "reason": "LOW_DO"
}

步驟：
1. 呼叫 handle_auto_hold_command()。

預期：
拒絕 Auto Hold
原因 BLOCKED_BY_USER_PAUSE
auto_hold.session = 0
last_auto_hold_command_ts = 1000
不得送 ON
```

---

### PAUSE-004：schedule_enabled = 0 時仍可禁止 Auto Hold

```text
初始：
schedule_enabled = 0
now_time = 星期一 09:00
schedule_pause_until = 星期一 10:00
pause_blocks_auto_trigger = 1
auto_hold.session = 0

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 101,
  "command_ts": 1010,
  "reason": "LOW_DO"
}

步驟：
1. 呼叫 handle_auto_hold_command()。

預期：
即使 schedule_enabled = 0，仍因 pause_blocks_auto_trigger 拒絕 Auto Hold
auto_hold.session = 0
last_auto_hold_command_ts = 1010
```

---

## 7. Auto Hold 建立測試

### AH-001：合法新 Auto Hold 建立

```text
初始：
relay_actual_state = OFF
auto_hold.session = 0
last_auto_hold_command_ts = 0
pause 無效

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 2000,
  "reason": "LOW_DO"
}

步驟：
1. 呼叫 handle_auto_hold_command()。
2. 執行 control_task。

預期：
auto_hold.session = 12345
auto_hold.target_state = ON
auto_hold.start_state = OFF
auto_hold.until_ms = monotonic_ms + 600000
last_auto_hold_command_ts = 2000
control_task 使用 Auto Hold target = ON
送 ON
```

---

### AH-002：設備原本 ON 時建立 ON Auto Hold

```text
初始：
relay_actual_state = ON
auto_hold.session = 0

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12346,
  "command_ts": 2010,
  "reason": "LOW_DO"
}

預期：
auto_hold.start_state = ON
auto_hold.target_state = ON
Auto Hold 結束且排程器無法接手時，恢復 ON
```

---

### AH-003：session = 0 拒絕

```text
命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 0,
  "command_ts": 2020,
  "reason": "LOW_DO"
}

預期：
拒絕
不建立 Auto Hold
不送 ON
```

---

### AH-004：duration_seconds > 1800 拒絕

```text
命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 3600,
  "session": 12347,
  "command_ts": 2030,
  "reason": "LOW_DO"
}

預期：
拒絕 DURATION_TOO_LONG
不建立 Auto Hold
last_auto_hold_command_ts = 2030
```

---

### AH-005：duration_seconds = 0 且無 active session

```text
初始：
auto_hold.session = 0
last_auto_hold_command_ts = 2030

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 0,
  "session": 12347,
  "command_ts": 2040,
  "reason": "CLEAR"
}

預期：
不建立 Auto Hold
不送 ON/OFF
last_auto_hold_command_ts = 2040
```

---

## 8. Auto Hold 續約測試

### AH-RENEW-001：同 session 合法續約

```text
初始：
auto_hold.session = 12345
auto_hold.target_state = ON
auto_hold.start_state = OFF
last_auto_hold_command_ts = 2000

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 2050,
  "reason": "LOW_DO still active"
}

預期：
更新 until_ms
更新 reason
last_auto_hold_command_ts = 2050
不更新 start_state
不更新 target_state
```

---

### AH-RENEW-002：同 session 改 target 拒絕

```text
初始：
auto_hold.session = 12345
auto_hold.target_state = ON
auto_hold.start_state = OFF
last_auto_hold_command_ts = 2050

命令：
{
  "channel": 3,
  "target": "OFF",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 2060,
  "reason": "wrong target"
}

預期：
拒絕 SESSION_TARGET_MISMATCH
不更新 until_ms
不更新 target_state
不更新 start_state
last_auto_hold_command_ts = 2060
```

---

### AH-RENEW-003：Auto Hold active 時不同 session 拒絕

```text
初始：
auto_hold.session = 12345
last_auto_hold_command_ts = 2060

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 99999,
  "command_ts": 2070,
  "reason": "new session while busy"
}

預期：
拒絕 CHANNEL_BUSY
不覆蓋現有 Auto Hold
last_auto_hold_command_ts = 2070
```

---

### AH-RENEW-004：command_ts 舊封包忽略

```text
初始：
auto_hold.session = 12345
last_auto_hold_command_ts = 2070

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 2065,
  "reason": "old packet"
}

預期：
直接忽略
不更新 last_auto_hold_command_ts
不更新 until_ms
不更新 reason
```

---

### AH-RENEW-005：command_ts 相同但內容不同

```text
初始：
auto_hold.session = 12345
auto_hold.target_state = ON
last_auto_hold_command_ts = 2070

命令：
{
  "channel": 3,
  "target": "OFF",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 2070,
  "reason": "same timestamp different target"
}

預期：
不得接受 target 變更
拒絕 SESSION_TARGET_MISMATCH
last_auto_hold_command_ts 維持 2070 或重新寫入 2070
```

---

## 9. Auto Hold Clear 測試

### AH-CLEAR-001：同 session clear 成功，交回 Scheduler

```text
初始：
auto_hold.session = 12345
auto_hold.target_state = ON
auto_hold.start_state = OFF
last_auto_hold_command_ts = 2070
schedule_enabled = 1
pause 無效
time_valid = 1
now_time = 星期一 13:00

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 0,
  "session": 12345,
  "command_ts": 2080,
  "reason": "clear"
}

預期：
清除 Auto Hold
last_auto_hold_command_ts = 2080
因排程器可接手，交回 Scheduler
星期一 13:00 為 OFF 區間
最終送 OFF
```

---

### AH-CLEAR-002：不同 session clear 不得清除現有 Auto Hold

```text
初始：
auto_hold.session = 200
last_auto_hold_command_ts = 3000

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 0,
  "session": 100,
  "command_ts": 3010,
  "reason": "old clear"
}

預期：
session 不同
不得清除 session 200
last_auto_hold_command_ts = 3010
```

---

### AH-CLEAR-003：舊 clear 封包忽略

```text
初始：
auto_hold.session = 200
last_auto_hold_command_ts = 3010

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 0,
  "session": 200,
  "command_ts": 3005,
  "reason": "old clear"
}

預期：
command_ts 舊
直接忽略
不得清除 Auto Hold
```

---

## 10. Auto Hold 到期收斂測試

### AH-EXP-001：到期且 Scheduler 可接手

```text
初始：
auto_hold.session = 12345
auto_hold.target_state = ON
auto_hold.start_state = OFF
monotonic_ms >= auto_hold.until_ms
schedule_enabled = 1
pause 無效
time_valid = 1
now_time = 星期一 09:00

步驟：
1. 執行 control_task 或 auto_hold_finish()。

預期：
Auto Hold 清除
Scheduler 接手
星期一 09:00 為 ON 區間
最終 desired_state = ON
不恢復 start_state OFF
```

---

### AH-EXP-002：到期且 Scheduler 無法接手，恢復 start_state

```text
初始：
auto_hold.session = 12345
auto_hold.target_state = ON
auto_hold.start_state = OFF
monotonic_ms >= auto_hold.until_ms
schedule_enabled = 0

預期：
Auto Hold 清除
Scheduler 無法接手
恢復 start_state = OFF
```

---

### AH-EXP-003：到期時 pause 仍有效，恢復 start_state

```text
初始：
auto_hold.session = 12345
auto_hold.start_state = OFF
schedule_enabled = 1
schedule_pause_until > now_time

預期：
Auto Hold 清除
因 pause 有效，Scheduler 不接手
恢復 start_state = OFF
```

---

### AH-EXP-004：到期時 time_valid = 0，恢復 start_state

```text
初始：
auto_hold.session = 12345
auto_hold.start_state = OFF
schedule_enabled = 1
time_valid = 0

預期：
Auto Hold 清除
因 time_valid = 0，Scheduler 不接手
恢復 start_state = OFF
```

---

## 11. 使用者手動控制測試

### USER-001：手動 OFF 清除 Auto Hold

```text
初始：
auto_hold.session = 12345
relay_actual_state = ON
schedule_enabled = 1

使用者命令：
{
  "channel": 3,
  "target": "OFF",
  "source": "USER",
  "pause_scheduler": {
    "mode": "current_segment_end",
    "pause_blocks_auto_trigger": false
  }
}

預期：
Auto Hold 清除
schedule_pause_until = current_segment_end + 1 min
pause_blocks_auto_trigger = 0
發送 USER OFF
不恢復 Auto Hold start_state
```

---

### USER-002：手動 OFF 並禁止 Auto Hold

```text
初始：
auto_hold.session = 12345
relay_actual_state = ON
schedule_enabled = 1

使用者命令：
{
  "channel": 3,
  "target": "OFF",
  "source": "USER",
  "pause_scheduler": {
    "mode": "duration",
    "duration_seconds": 3600,
    "pause_blocks_auto_trigger": true
  }
}

預期：
Auto Hold 清除
schedule_pause_until = now_time + 3600
pause_blocks_auto_trigger = 1
發送 USER OFF
1 小時內 Auto Hold 命令被 MCU 拒絕
```

---

### USER-003：手動清 Auto Hold 後，同 session 新 command_ts 可再次介入

```text
初始：
auto_hold.session = 12345
last_auto_hold_command_ts = 5000
relay_actual_state = ON

步驟：
1. 使用者手動 OFF。
2. pause_blocks_auto_trigger = 0。
3. 後端再次送同 session，command_ts = 5010。

預期：
因本版不記錄 cancelled_session
且 pause_blocks_auto_trigger = 0
MCU 可重新處理同 session 新命令
```

---

### USER-004：手動清 Auto Hold 後，同 session 但被 pause_blocks_auto_trigger 擋下

```text
初始：
auto_hold.session = 12345
last_auto_hold_command_ts = 5000
relay_actual_state = ON

步驟：
1. 使用者手動 OFF。
2. pause_blocks_auto_trigger = 1。
3. schedule_pause_until = now_time + 3600。
4. 後端再次送同 session，command_ts = 5010。

預期：
MCU 拒絕 Auto Hold
原因 BLOCKED_BY_USER_PAUSE
last_auto_hold_command_ts = 5010
relay 維持使用者 OFF
```

---

## 12. 安全保護測試

### SAFE-001：Auto Hold target 被安全保護阻擋

```text
初始：
auto_hold.session = 0
relay_actual_state = OFF
channel = 過流鎖定中

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 6000,
  "command_ts": 6000,
  "reason": "LOW_DO"
}

預期：
Auto Hold 可建立
control_task 嘗試 desired_state = ON
relay protection check 阻擋 ON
worker 不得寫 ON
Auto Hold 維持到 until_ms
期間 Scheduler 不干預
```

---

### SAFE-002：安全保護期間 Auto Hold 不高頻覆蓋

```text
初始：
Auto Hold active
target_state = ON
channel = 過流鎖定中
relay_actual_state = OFF

步驟：
1. 連續執行 control_task 100 次。

預期：
不得高頻無限制重送 ON
不得繞過安全保護
可記錄 blocked 狀態或節流重試
```

---

### SAFE-003：安全保護解除後是否恢復，由安全策略決定

```text
初始：
Auto Hold active
target_state = ON
channel 原本過流鎖定
後來解除

預期：
若安全策略允許重試，control_task 可再次送 ON。
若安全策略要求人工恢復，不得自動 ON。
此行為需由安全保護規格決定。
```

---

## 13. 時間與重啟測試

### TIME-001：RTC 無效但 Auto Hold 可用

```text
初始：
time_valid = 0
auto_hold.session = 0

命令：
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 7000,
  "command_ts": 7000,
  "reason": "LOW_DO"
}

預期：
Auto Hold 可接受
使用 monotonic_ms 到期
Scheduler 不自動 ON
```

---

### TIME-002：RTC 無效時 Auto Hold 到期恢復 start_state

```text
初始：
time_valid = 0
auto_hold.session = 7000
auto_hold.start_state = OFF
monotonic_ms >= until_ms

預期：
Auto Hold 清除
Scheduler 因 time_valid = 0 不接手
恢復 start_state = OFF
```

---

### BOOT-001：MCU 重啟後清除 Auto Hold

```text
初始：
重啟前 Auto Hold active

步驟：
1. MCU reboot。
2. 執行初始化流程。

預期：
所有 relay 先保持 OFF
auto_hold.session = 0
last_auto_hold_command_ts = 0
Auto Hold 不從 Flash 恢復
若後端仍需要控制，必須重新送新命令
```

---

### BOOT-002：boot_catch_up_enabled = 0 不補開

```text
初始：
開機時 now_time = 星期一 09:00
目前在排程 ON 區間
boot_catch_up_enabled = 0

預期：
relay 保持 OFF
不因目前在 ON 區間立即補開
```

---

### BOOT-003：boot_catch_up_enabled = 1 可補開

```text
初始：
開機時 now_time = 星期一 09:00
目前在排程 ON 區間
boot_catch_up_enabled = 1
time_valid = 1
無故障鎖定

預期：
boot delay 後 Scheduler 可送 ON
仍需通過 relay protection check
```

---

## 14. Control Queue / Worker 測試

### QUEUE-001：所有來源都不得直接寫 GPIO

```text
測試來源：
USER ON/OFF
Scheduler ON/OFF
Auto Hold ON/OFF
Auto Hold restore start_state
安全保護 OFF

預期：
所有來源都只能送 control request
GPIO 只由 worker 寫入
```

---

### QUEUE-002：同一 channel 控制請求序列化

```text
步驟：
1. 幾乎同時送 USER OFF、Auto Hold ON、Scheduler ON。
2. 觀察 control queue 順序。

預期：
同一 channel 不得並行寫 GPIO
最終狀態符合優先權：
安全保護 > 使用者手動 > Auto Hold > Scheduler
```

---

### QUEUE-003：relay_actual_state 與 desired_state 一致時不重送

```text
初始：
desired_state = ON
relay_actual_state = ON

預期：
不送重複 ON
避免 queue 被塞爆
```

---

## 15. API / MQTT 整合測試

### API-001：Auto Hold API 建立

```http
POST /api/relay/channels/3/auto-hold
```

```json
{
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 10000,
  "reason": "LOW_DO"
}
```

預期：

```text
回傳 accepted
MCU 建立 Auto Hold
status 顯示 auto_hold_active = true
```

---

### API-002：Auto Hold status 查詢

```http
GET /api/relay/channels/3/scheduler/status
```

預期欄位：

```json
{
  "auto_hold_active": true,
  "auto_hold_target": "ON",
  "auto_hold_start_state": "OFF",
  "auto_hold_remaining_seconds": 420,
  "auto_hold_session": 12345,
  "auto_hold_last_command_ts": 10000
}
```

---

### API-003：Pause API 禁止 Auto Hold

```http
POST /api/relay/channels/3/scheduler/pause
```

```json
{
  "mode": "duration",
  "duration_seconds": 3600,
  "pause_blocks_auto_trigger": true
}
```

預期：

```text
schedule_pause_until = now + 3600
pause_blocks_auto_trigger = 1
後續 Auto Hold 被拒絕
```

---

### MQTT-001：topic 決定命令類型

Topic：

```text
relay/channel/3/auto-hold
```

Payload：

```json
{
  "channel": 3,
  "target": "ON",
  "duration_seconds": 600,
  "session": 12345,
  "command_ts": 10000,
  "reason": "LOW_DO"
}
```

預期：

```text
不需要 payload type
依 topic 分派到 Auto Hold handler
```

---

## 16. 壓力與長時間測試

### STRESS-001：連續續約 24 小時

```text
步驟：
1. 建立 Auto Hold，duration = 600 秒。
2. 每 5 分鐘用同 session 續約。
3. 持續 24 小時。

預期：
start_state 不變
target_state 不變
until_ms 正常延長
無 memory leak
無 queue 爆量
無 uint32_t 時間比較錯誤
```

---

### STRESS-002：大量舊封包

```text
步驟：
1. last_auto_hold_command_ts = 10000。
2. 連續送 1000 筆 command_ts < 10000 的封包。

預期：
全部忽略
不改 Auto Hold
不更新 last_auto_hold_command_ts
不送控制請求
```

---

### STRESS-003：多 channel 同時控制

```text
步驟：
1. CH1 Auto Hold ON。
2. CH2 Scheduler ON。
3. CH3 User OFF。
4. CH4 Safety OFF。
5. 同時執行 control_task。

預期：
各 channel 狀態互不干擾
control queue 正常序列化
worker 正確輸出
```

---

## 17. 驗收標準

功能通過驗收需滿足：

```text
1. 所有測試案例 PASS。
2. 任一來源不得直接寫 GPIO。
3. Auto Hold 有效時 Scheduler 不得干預。
4. Pause 有效時 Scheduler 不得干預。
5. pause_blocks_auto_trigger = 1 時 Auto Hold 必須被拒絕。
6. command_ts 舊封包不得影響狀態。
7. 同 session 續約不得修改 start_state / target_state。
8. duration_seconds > 1800 必須拒絕。
9. Auto Hold 到期後能正確交回 Scheduler 或恢復 start_state。
10. 使用者手動控制必須清除 Auto Hold。
11. schedule_enabled = 0 時 pause_blocks_auto_trigger 仍能禁止 Auto Hold。
12. RTC 無效時 Scheduler 不自動 ON，但 Auto Hold 仍能用 monotonic time 到期。
13. MCU reboot 後 Auto Hold 不恢復。
14. 安全保護永遠最高優先。
15. control_task 是唯一 desired_state 決策點。
16. worker 是唯一 GPIO 寫入點。
```

---

## 18. 測試輸出格式

成功：

```text
[PASS] SCH-001 adjacent intervals allowed
[PASS] CTL-001 scheduler on interval sends ON
[PASS] PAUSE-004 schedule disabled still blocks Auto Hold
[PASS] AH-RENEW-002 same session target mismatch rejected
```

失敗：

```text
[FAIL] TEST_ID
reason:
expected:
actual:
state dump:
```

state dump 範例：

```text
channel=3
relay_actual_state=OFF
schedule_enabled=1
schedule_pause_until=1782196200
pause_blocks_auto_trigger=1
auto_hold.session=12345
auto_hold.target_state=ON
auto_hold.start_state=OFF
auto_hold.until_ms=12345678
last_auto_hold_command_ts=10000
time_valid=1
```

---

## 19. 建議測試執行順序

```text
1. SCH-*        排程資料檢查
2. CTL-*        control_task 基本決策
3. PAUSE-*      pause 與 Auto Hold block
4. AH-*         Auto Hold 建立
5. AH-RENEW-*   Auto Hold 續約
6. AH-CLEAR-*   Auto Hold clear
7. AH-EXP-*     Auto Hold 到期收斂
8. USER-*       使用者手動控制
9. SAFE-*       安全保護
10. TIME-*      時間有效性
11. BOOT-*      開機重啟
12. QUEUE-*     control queue / worker
13. API-*       API 整合
14. MQTT-*      MQTT 整合
15. STRESS-*    壓力測試
```

---

## 20. 最小必跑測試清單

時間不足時至少必跑：

```text
SCH-002
CTL-001
CTL-002
PAUSE-003
PAUSE-004
AH-001
AH-004
AH-RENEW-001
AH-RENEW-002
AH-RENEW-004
AH-CLEAR-001
AH-CLEAR-002
AH-EXP-001
AH-EXP-002
USER-001
USER-002
SAFE-001
TIME-001
BOOT-001
QUEUE-001
```

以上測試通過後，才建議進入實機 relay 測試。
