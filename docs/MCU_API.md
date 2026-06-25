# autoControl MCU HTTP API 完整參考

> 板子 IP: DHCP 動態分配（目前 master 約在 192.168.0.157 / .245）  
> Port: 80 (HTTP)  
> Content-Type: `application/x-www-form-urlencoded`（**不是 JSON**）  
> 認證: Session Token（`X-Auth-Token` header）

---

## 認證模型

| Scope | 權限 | 用途 |
|-------|------|------|
| `control` | relay 控制 + 保護設定 + 叢集管理 | 一般操作 |
| `service` | 上述全部 + config sync | 後端 / 自動化 |

### POST /api/login

```
body: password=admin
body: password=admin&scope=service&service_key=<backend_key>
→ {"ok":true,"token":<session_token>}
```

### POST /api/logout

```
→ {"ok":true,"logout":true}
```

### GET /api/session

```
→ {"ok":true,"authenticated":true,"scope":"control"}
```

---

## Relay 讀取

### GET /api/relay

回傳目前 16 路 GPIO 狀態快照（不需認證）。

```
→ {"ok":true,"relay_mask":0,"ch1":0,"ch2":0,...,"ch16":0}
```

---

## Relay 控制（需 control/service scope）

### POST /api/relay-channel

單路 ON/OFF。

```
body: channel=3&value=1
→ {"ok":true}
```

### POST /api/relay-mask

一次設全部 16 路。

```
body: mask=65535
→ {"ok":true}
```

### POST /api/cluster/node-relay

透過叢集對遠端 node 發 relay 命令（master 代理轉發）。

```
body: node_id=<target_node_id>&channel=3&value=1
→ {"ok":true}
```

---

## 叢集

### GET /api/config/cluster

回傳完整叢集 + cloud 設定（需認證）。

```
→ {
  "ok":true,
  "device_name":"...",
  "cluster_name":"default",
  "cluster_enabled":1,
  "auto_join_cluster":1,
  "cluster_password_set":true,
  "backend_host":"...",
  "backend_port":42001,
  "backend_key_set":true,
  "cloud_enabled":1,
  "cloud_transport":1,
  "cloud_broker_host":"...",
  "cloud_broker_port":8883,
  "cloud_cluster_id":"...",
  "cloud_device_id":"...",
  "cloud_key_id":"...",
  "cloud_cluster_ca_fingerprint":"...",
  "cloud_cluster_binding_secret_set":true,
  "cloud_cluster_ca_cert_set":true,
  "provisioning_revision":0,
  "provisioning_ts":0
}
```

### POST /api/config/cluster

更新叢集/cloud 設定（需認證）。

```
body: device_name=<name>
body: cluster_name=<name>
body: cluster_enabled=1
body: auto_join_cluster=1
body: cluster_password=<new_password>
body: backend_host=<host>
body: backend_port=42001
body: backend_key=<key>
body: cloud_enabled=1
body: cloud_transport=1           # 1=MQTT, 0=raw TCP fallback
body: cloud_broker_host=<host>
body: cloud_broker_port=8883
body: cloud_cluster_ca_fingerprint=<hex>
body: cloud_cluster_binding_secret=<secret>
body: cloud_cluster_ca_cert=<pem>
body: provisioning_revision=<rev>

註：cloud_cluster_id / cloud_device_id / cloud_key_id 唯讀（由 MCU UID 產生）
註：config sync 支援加密模式：_enc=1&_data=<base64url>（AES-128-GCM）
```

### GET /api/cluster/nodes

回傳叢集節點清單。

### POST /api/cluster/kick

踢除線上節點（master only）。

```
body: node_id=<node_id>
```

### POST /api/cluster/forget

移除離線節點紀錄（master only）。

```
body: node_id=<node_id>
```

---

## Relay 設定

### GET /api/config/relays

回傳全部 16 路 relay 設定（需認證）。

### POST /api/config/relay

更新單路 relay 設定（需認證）。

```
body: channel=3
body: name=增氧泵
body: enabled=1
body: remember_state=1
body: boot_default_on=0
body: adc_channel=0              # 綁定 CT channel（255=未綁定）
body: protection_enabled=1
body: overcurrent_milliamps=30000
body: stall_timeout_ms=3000
body: cooldown_seconds=0         # 0=使用全域設定
body: retry_delay_seconds=0      # 0=使用全域設定
body: max_retry_count=0          # 0=使用全域設定
body: boot_delay_seconds=0       # 0=使用全域設定
body: strategy=0                 # 0=default, 1=溫和
```

---

## Relay 安全保護

### GET /api/protection

回傳全部 16 路保護狀態（故障鎖定、冷卻、過流、relay 黏住、ADC 飽和）。

### GET /api/config/protection

回傳全域保護參數。

### POST /api/config/protection

更新全域保護參數（需認證）。

```
body: cooldown_seconds=60
body: retry_delay_seconds=5
body: max_retry_count=3
body: boot_delay_seconds=30
body: learning_samples=100
body: aging_update_hours=24
body: aging_cap_percent=20
body: stall_multiplier_x10=15
```

### POST /api/protection/strategy

設定單路保護策略（需認證）。

```
body: channel=3&strategy=0       # 0=default, 1=溫和
```

### POST /api/protection/force-stop

緊急停止單路（需認證）。

```
body: channel=3
```

### POST /api/protection/clear-learning

清除單路 CT 學習資料（需認證）。

```
body: channel=3
```

### POST /api/protection/unlock

解鎖故障鎖定的 channel（需認證）。

```
body: channel=3
```

---

## 排程器

全部路由都在 `/api/relay/channels/:channel/...` 之下。

### GET /api/relay/channels/:ch/schedule-week

```
→ {"ok":true,"channel":3,"schedule_enabled":1,
    "days":[
      {"weekday":0,"intervals":[]},
      {"weekday":1,"intervals":[{"start_minute":480,"end_minute":720}]},
      ...]}
```

### PUT /api/relay/channels/:ch/schedule-week/:weekday

覆寫某一天的 ON 區間。`:weekday` = 0(Mon)~6(Sun)。

```
body: count=2&s0=480&e0=720&s1=840&e1=1080
→ {"ok":true}
```

欄位：
- `count`: 區間數（0~8，0=該天全天 OFF）
- `sN`: 第 N 個區間的 start_minute（0~1439）
- `eN`: 第 N 個區間的 end_minute（1~1440）

### POST /api/relay/channels/:ch/schedule-week/:weekday/copy

複製某一天排程到其他星期。

```
body: target_weekdays=1,2,3&mode=overwrite
```

### PATCH /api/relay/channels/:ch/scheduler/enabled

```
body: enabled=1   (or enabled=0)
→ {"ok":true}
```

### POST /api/relay/channels/:ch/scheduler/pause

```
body: mode=1                      # 0=current_segment_end, 1=duration, 2=until, 3=forever
body: duration_seconds=3600       # mode=1 時使用
body: until=<unix_ts>             # mode=2 時使用
body: pause_blocks_auto_trigger=1 # 是否同時阻擋 Auto Hold
→ {"ok":true}
```

### POST /api/relay/channels/:ch/scheduler/resume

清除 pause 狀態。

```
→ {"ok":true}
```

---

## Auto Hold（後端感測器控制）

### POST /api/relay/channels/:ch/auto-hold

```
body: target=1                    # 1=ON, 0=OFF
body: duration_seconds=600        # 1~1800，0=清除
body: session=12345               # 後端控制 session ID（不可為 0）
body: command_ts=<unix_ts>        # 時間戳，用於舊封包拒絕
body: reason=溶氧過低，自動增氧中   # 顯示用，最多 63 bytes
→ {"ok":true}
```

續約：同 session + 同 target + 新 duration + 較新的 command_ts → 只更新 until_ms。
清除：同 session + duration_seconds=0。

---

## 手動控制（原子操作）

### POST /api/relay/channels/:ch/control

原子流程：清 Auto Hold → 設 pause → 發 relay 命令。

```
body: target=0                    # 0=OFF, 1=ON
body: pause_mode=1                # 0=current_segment_end, 1=duration, 2=until, 3=forever
body: pause_duration_seconds=3600
body: pause_until=<unix_ts>
body: pause_blocks_auto_trigger=1
→ {"ok":true}
```

---

## Scheduler 狀態查詢

### GET /api/relay/channels/:ch/scheduler/status

```
→ {
  "ok":true,
  "channel":3,
  "schedule_enabled":1,
  "pause_active":1,
  "schedule_pause_until":1782240059,
  "pause_blocks_auto_trigger":0,
  "auto_hold_active":1,
  "auto_hold_target":1,
  "auto_hold_start_state":0,
  "auto_hold_remaining_seconds":420,
  "auto_hold_session":12345,
  "auto_hold_last_command_ts":1782240000,
  "auto_hold_reason":"溶氧過低，自動增氧中",
  "time_valid":1
}
```

---

## 全域狀態

### GET /api/status

完整設備狀態 JSON（不需認證）。包含：
- `uptime_ms`, `ip`, `link_up`, `dhcp_bound`
- `relay_mask`: 目前 16 路狀態
- `adc`: ADC 取樣摘要（16 通道 + VREF + CT）
- `adc_channels[]`: 每路 ADC 詳細值
- `cluster`: 叢集狀態（role, master, term, time...）
- `time_sync`: NTP/backend 時間同步狀態
- `discovery`: UDP discovery 統計
- `http`: HTTP 連線統計
- `backend`: raw TCP backend 統計
- `cloud`: MQTT 連線統計（connected, connect_failures, last_error...）
- `control`: TCP port 41000 診斷統計
- `control_queue`: 控制佇列深度
- `relay_protection`: 安全保護狀態
- `fault`: 最近一次故障紀錄
- `rtos`: heap_free, heap_min
- `lwip`: LwIP 內部記憶體/連線統計

---

## Web UI

### GET / 或 GET /index.html

回傳內建 SPA 控制面板（HTML/CSS/JS inline，從 Web/web_assets.h 內嵌）。

---

## 控制流程

```
HTTP API (form-encoded)
  │
  ├─ scheduler_handle_auto_hold()    ← POST .../auto-hold
  ├─ scheduler_user_control()        ← POST .../control
  ├─ scheduler 狀態直接寫入           ← PATCH/POST pause, resume, enabled
  │
  ▼
request_relay(channel, target)
  │ control_queue_submit_async()
  ▼
control_queue（序列化）
  │
  ▼
relay_protection_check（安全保護：過流/黏住/冷卻/故障鎖定）
  │
  ▼
worker → GPIO（唯一能寫 GPIO 的地方）
```

---

## 優先權

1. 安全保護（最高，不可被覆蓋）
2. 使用者手動控制（清 Auto Hold + 設 pause）
3. pause_blocks_auto_trigger = 1 且 pause 有效
4. 後端 Auto Hold
5. 使用者暫停排程器
6. 一般設備排程器
