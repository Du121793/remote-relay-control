# autoControl 系統架構文件

> 工業 IoT 繼電器控制 — STM32F767ZI + FreeRTOS + lwIP + MQTT TLS-PSK  
> Branch: `feature/scheduler-v4` | Updated: 2026-06-26

---

## 1. 系統架構總覽

```
┌──────────────────────────────────────────────────────────────┐
│                       Cloudflare Tunnel                       │
│  autocontrol.du1217.club :443 → VM:4300 (Backend HTTP)       │
│  mqtt.autocontrol.du1217.club :8883 → VM:8883 (MQTT Broker)  │
└──────────────────────────────────────────────────────────────┘
          │                                    │
          ▼                                    ▼
┌─────────────────────┐            ┌─────────────────────────┐
│    Backend Server    │            │    MQTT PSK Broker       │
│   (server.js:4300)   │──HTTP:4301→│  (mqtt-psk-broker.js)   │
│                      │            │      :8883 (TLS-PSK)     │
│  • REST API          │            │      :4301 (Control API) │
│  • Sensor Session    │            └─────────────────────────┘
│  • Key Management    │                      │
│  • Provisioning      │              MQTT TLS-PSK
│  • Dashboard         │                      │
└─────────────────────┘                      ▼
                                   ┌─────────────────────────┐
                                   │    MCU Cluster           │
                                   │  ┌───────┐ ┌───────┐   │
                                   │  │ 157   │ │ 245   │   │
                                   │  │Member │ │Master │   │
                                   │  └───────┘ └───────┘   │
                                   │    UDP Discovery:40000   │
                                   │    HTTP API:80           │
                                   └─────────────────────────┘
```

---

## 2. 後端-MCU 通訊架構

### 2.1 命令下發 (Backend → MCU)

```
┌──────────┐   HTTP POST    ┌──────────┐   MQTT Publish   ┌──────────┐
│ Grafana  │ ──────────────→│ Backend  │ ────────────────→│  MQTT    │
│ Webhook  │  /api/sensor/* │ Server   │  cluster/default  │  Broker  │
└──────────┘                └──────────┘  /command/xxx     └──────────┘
                                  │                              │
                                  │                    MQTT Subscribe
                                  │                              │
                                  ▼                              ▼
                          ┌──────────────┐            ┌──────────────────┐
                          │  MCU (Master) │            │   MCU (Master)    │
                          │  HTTP API:80  │            │  MQTT handler     │
                          │  (direct)     │            │  → scheduler      │
                          └──────────────┘            │  → control_queue   │
                                                      │  → worker → GPIO   │
                                                      └──────────────────┘
```

### 2.2 狀態上報 (MCU → Backend)

```
┌──────────┐   MQTT Publish   ┌──────────┐   HTTP Poll    ┌──────────┐
│   MCU    │ ────────────────→│  MQTT    │ ←──────────── │ Backend  │
│ (Master) │  cluster/default  │  Broker  │  /api/clients │ Server   │
│          │  /status (2s)     │          │               │          │
│          │  /events (5s)     │          │  sync         │          │
│          │  /ack/{req_id}    │          │  knownNodes   │          │
└──────────┘                  └──────────┘               └──────────┘
```

### 2.3 MQTT Topic 結構

| Topic | 方向 | QoS | 用途 |
|------|------|:---:|------|
| `cluster/{id}/status` | MCU→Broker | 0 | 設備狀態 (retained, 2s) |
| `cluster/{id}/events` | MCU→Broker | 0 | Hello/Heartbeat (5s) |
| `cluster/{id}/command/relay` | Broker→MCU | 1 | Relay 控制 |
| `cluster/{id}/command/mask` | Broker→MCU | 1 | Relay mask |
| `cluster/{id}/command/config/relay` | Broker→MCU | 1 | Relay 設定 |
| `cluster/{id}/command/time` | Broker→MCU | 1 | 時間同步 |
| `cluster/{id}/command/auto-hold` | Broker→MCU | 1 | Auto Hold |
| `cluster/{id}/command/control` | Broker→MCU | 1 | 手動控制 |
| `cluster/{id}/command/scheduler/set` | Broker→MCU | 1 | 排程設定 |
| `cluster/{id}/command/scheduler/enabled` | Broker→MCU | 1 | 排程啟用 |
| `cluster/{id}/command/scheduler/pause` | Broker→MCU | 1 | 暫停排程 |
| `cluster/{id}/command/scheduler/resume` | Broker→MCU | 1 | 恢復排程 |
| `cluster/{id}/command/config/protection` | Broker→MCU | 1 | 全域保護參數 |
| `cluster/{id}/command/protection/strategy` | Broker→MCU | 1 | 單路保護策略 |
| `cluster/{id}/command/protection/force-stop` | Broker→MCU | 1 | 強制停止 |
| `cluster/{id}/command/protection/unlock` | Broker→MCU | 1 | 解鎖保護 |
| `cluster/{id}/command/protection/clear-learning` | Broker→MCU | 1 | 清除學習 |
| `cluster/{id}/ack/{request_id}` | MCU→Broker | 0 | 命令確認 |
| `cluster/{id}/query/request` | Broker→MCU | 0 | 後端查詢 MCU/Member 詳細狀態 |
| `cluster/{id}/query/response/{request_id}` | MCU→Broker | 0 | 後端查詢回應 |

### 2.4 後端詳細查詢

後端不再預設直接連線 `192.168.x.x` MCU HTTP API。原因是雲端後端通常不在同一個 LAN，直接 HTTP 也沒有端到端加密與身分保證。

目前詳細查詢流程：

```
Backend
  → Broker control API /api/request-response
  → MQTT cluster/{id}/query/request
  → Master MCU
      ├─ target 是自己：直接讀本機狀態後回覆
      └─ target 是 Member：由 Master 透過本地 cluster proxy 查詢 Member，再回覆
  → MQTT cluster/{id}/query/response/{request_id}
  → Backend
```

已支援查詢：

- `/api/config/relays`
- `/api/protection`
- `/api/config/protection`
- `/api/relay/channels/{ch}/scheduler/status`
- `/api/relay/channels/{ch}/schedule-week`

### 2.5 後端 Live State Cache 與查詢快取

後端顯示資料分成兩層：

1. **Live State Cache（主要顯示來源）**
   - 來源：MCU 透過 `cluster/{id}/status` 主動上報的 retained snapshot。
   - 後端 broker 解析 status 後，`server.js` 會同步到 `knownNodes`。
   - 用途：儀表板、節點清單、繼電器總覽、保護總覽的即時顯示。
   - 目前包含：`relay_mask`、`scheduler_enabled_mask`、`scheduler_pause_mask`、`scheduler_pause_blocks_mask`、`protection_enabled_mask`、`protection_tripped_mask`、`protection_fault_lock_mask`、`protection_ct_valid_mask`、`protection_learning_mask`、保護策略 bit mask、ADC/CT 摘要、節點 online/role/ip。
   - Member 的保護與排程「總覽狀態」由 discovery v8 與 Master 的 MQTT status `nodes[]` 轉送，因此後端不需要為每一路 Member 狀態反覆發 query fallback。
   - 資料新鮮度：
     - `LIVE_STATE_FRESH_MS` 預設 5000 ms。
     - `LIVE_STATE_STALE_MS` 預設 15000 ms。
     - 超過 stale window 後，前端不可把該資料當作已確認狀態。

2. **Detail Query Cache（詳細頁與補償用）**
   - 來源：後端透過 MQTT request-response 查詢 MCU 或 Member。
   - 用途：Relay 設定、Protection 設定、Scheduler week、排程實際時間表等詳細資料。
   - 同一個 `node/path` 同時只送一筆查詢；短時間內重複讀取會共用快取。
   - 寫入命令成功 publish 後，後端會清除該 node 的 detail query cache，避免長時間顯示舊設定。

狀態同步原則：

```
MCU 狀態變更
  → mark status dirty
  → MQTT status snapshot 立即 publish
  → broker 更新 client state
  → backend syncKnownNodesFromBroker()
  → knownNodes live cache 更新
  → SSE 推送 dashboard
```

ADC/current 類高頻資料不逐筆推送，仍跟隨 status snapshot 週期；Relay 開關、Scheduler 啟用/暫停、Protection lock/unlock/trip、學習完成等狀態變更會立即 mark dirty，觸發下一輪 MQTT poll 儘快 publish status。

為避免既有 `control_queue` 的遠端 HTTP 回應 buffer 被多筆查詢互相覆蓋，MCU 只小幅保留 MQTT pending query slot（目前 4 筆）作為排隊緩衝；底層 remote HTTP 仍避免真正多工共用同一份 response buffer。Local query 會立即處理，不佔用 remote HTTP response buffer。若未來要支援 10 台以上設備的高頻詳細查詢，應擴充「Master 狀態摘要 retained publish + 後端快取 + 需要時單筆 query」，而不是繼續加大 HTTP response buffer。

MCU 的 lwIP MQTT output ring buffer 目前是 4096 bytes，因此大型查詢不可直接回完整 verbose JSON。`config/relays` 與 `protection` 會使用 compact JSON array 回傳，後端收到後再展開成既有 API 欄位格式。這樣不增加常駐 RAM，也避免大型 response publish 失敗造成後端 timeout。

當查詢目標是 Master 自己時，Master 直接產生 compact response。當查詢目標是 Member 時，Master 不應該向 Member 讀取 verbose `/api/config/relays` 或 `/api/protection`，而是改讀 Member 的 compact HTTP endpoint：

- `/api/config/relays-compact`
- `/api/protection-compact`

這兩個 endpoint 是 MCU-to-MCU / cloud-query 轉發用的低 RAM 格式；一般本機 Web 仍可使用 verbose endpoint。

### 2.6 MQTT TLS 記憶體配置

MQTT 雲端連線使用 TLS-PSK。TLS config 物件生命週期比一般 HTTP chunk 與 MQTT pbuf 長，若直接走 lwIP heap，重連時容易受碎片化影響。

目前 `altcp_tls_mbedtls_mem.c` 保留 2 個固定 TLS config slot，每個 slot 8192 bytes：

- slot 1：主要供 MQTT cloud link 使用。
- slot 2：保留給 HTTPS provisioning 或短時間重連交錯。

若 UART 出現 `mqtt tls cfg err=1202`，代表 TLS config allocation 在建立階段失敗；優先檢查 slot size 是否小於實際 `struct altcp_tls_config`，或是否有 config 未釋放。

---

## 3. MCU-MCU 叢集通訊架構

### 3.1 發現與選舉流程

```
┌──────────┐                          ┌──────────┐
│ MCU 157  │                          │ MCU 245  │
│ Member   │                          │ Master   │
└────┬─────┘                          └────┬─────┘
     │                                     │
     │  UDP Multicast 239.255.42.99:40000  │
     │◄──────────announce (2s)───────────►│
     │                                     │
     │  Announce Payload (v7, ~643 bytes): │
     │  • service="autoControl"            │
     │  • cluster_name, device_name        │
     │  • node_id, role, term              │
     │  • auth_tag (HMAC-SHA256)           │
     │  • load_score, relay_mask           │
     │  • ADC data, time info              │
     │  • cloud_cluster/device/key_id      │
     │  • MAC address, IP, port            │
     │                                     │
     ▼                                     ▼
┌──────────────┐                    ┌──────────────┐
│ Discovery    │                    │ Discovery    │
│ Service      │                    │ Service      │
│              │                    │              │
│ parse → save │                    │ parse → save │
│ → election   │                    │ → election   │
└──────────────┘                    └──────────────┘
     │                                     │
     ▼                                     ▼
  Role: Member                        Role: Master
  master_node_id=1165657530           master_node_id=1165657530
  term=2                              term=2
```

### 3.2 選舉規則

```
cluster_service_run_election():
  1. 收到 announce → cluster_service_note_remote_node() → 立即 run_election()
  2. 比較所有 node (含自己):
     a. 找 role=CLUSTER_ROLE_MASTER 的遠端 node
     b. 最高 term 者為 master candidate
     c. 同 term 時，最低 node_id 為 master candidate
  3. 若自己為 candidate: term = max_seen_term + 1, role = MASTER
  4. 若遠端為 candidate: term = candidate.term, role = MEMBER
  5. Master switch delay: CLUSTER_SERVICE_MASTER_SWITCH_DELAY_MS = 2.5s
  6. Self-promotion delay: STARTUP=4s, FAILOVER=3s
```

選主資格只看叢集名稱、叢集密碼 auth_tag、node_id、role、term、online/stale 狀態與健康狀態；不因本機與遠端 `unix_time` 差超過 30 秒而排除節點。時間差只影響是否採信對方時間，不能讓合法節點失去選主資格，避免 RTC/NTP/MQTT 短暫不同步造成 split-brain。

### 3.3 時間同步

```
時間優先序:
  1. MQTT Broker time (server-time command)
  2. NTP (pool.ntp.org)
  3. Backend time sync
  4. Cluster Master announce

Master 取得時間 → announce 含 unix_time + time_valid=1
  → Member: cluster_service_set_unix_time(announce.unix_time,
                                          CLUSTER_TIME_SOURCE_MASTER)
  → RTC signature 寫入 DR7 供斷電後恢復
```

---

## 4. 安全保護策略

### 4.1 控制優先權

```
1. 安全保護 (最高)
   ├─ 30A 硬體上限
   ├─ ADC 飽和
   ├─ Relay 黏住
   ├─ 強制停止 (API)
   └─ 故障鎖定

2. 使用者手動控制
   └─ 清除 Auto Hold + 設定 pause

3. Pause + pause_blocks_auto_trigger
   └─ 使用者禁止 Backend Auto Hold

4. Backend Auto Hold
   └─ 感測器短期控制租約 (≤1800s)

5. Scheduler
   └─ 每週排程 ON/OFF (最低優先)
```

### 4.2 保護觸發條件

| 保護 | 條件 | 行為 | 恢復 |
|------|------|------|------|
| **30A Hard Limit** | mA ≥ 30000 | 立即鎖定 | 手動解鎖 |
| **ADC Saturation** | CT saturated | 立即鎖定 | 手動解鎖 |
| **Relay Stuck** | OFF 後 current > noise×3 (5s) | 鎖定 | 手動解鎖 |
| **Fast OC** | mA > baseline × 2.0 | 300ms 跳脫+鎖定 | 手動解鎖 |
| **Slow OC** | mA > baseline × 1.5 (3s) | 跳脫+鎖定 | 手動解鎖 |
| **OC Warning** | mA > baseline × 1.25 (4s) | 僅告警 | 自動 |
| **Under Current** | mA < ref/2 (5s) | 跳脫 | 視策略 |
| **Startup Fail** | startup 5s 內 mA≈0 | 跳脫 | 可重試 |
| **Startup Peak** | startup peak > history × 1.5 | 立即跳脫 | 可重試 |
| **Force Stop** | API 命令 | 立即 OFF+鎖定 | 手動解鎖 |

### 4.3 保護策略 (per-channel)

| Strategy | Mode | OC | UC | Stuck |
|------|------|:---:|:---:|:---:|
| **Default (0)** | Safety | Trip+Lock | Trip | Lock |
| **Continuity (1)** | Gentle | Trip | Warn Only | Lock |

### 4.4 控制鏈

```
任何來源 (User / Scheduler / Auto Hold)
  │
  ▼
request_relay(channel, target)
  │
  ▼
control_queue_submit_async()
  │
  ▼
relay_protection_check(channel, target)
  ├─ fault_locked?       → REJECT (最高優先)
  ├─ cooldown_active?    → REJECT
  ├─ force_stop?         → REJECT
  ├─ stuck_detected?     → REJECT
  └─ overcurrent?        → auto-OFF + lock
  │
  ▼ (OK)
worker → GPIO write
```

### 4.5 全域保護參數

| 參數 | 預設 | 說明 |
|------|------|------|
| `cooldown_seconds` | 3 | 故障後冷卻時間 |
| `retry_delay_seconds` | 30 | 重試延遲 |
| `max_retry_count` | 1 | 最大重試次數 |
| `boot_delay_seconds` | 30 | 開機延遲 |
| `learning_samples` | 100 | CT 學習樣本數 |
| `aging_update_hours` | 24 | Aging 更新間隔 |
| `aging_cap_percent` | 20 | Aging 上限 |
| `stall_multiplier_x10` | 15 | Stall 判斷倍率 (×1.5) |

---

## 5. Backend API 完整參考

**Base:** `https://autocontrol.du1217.club`

### 5.1 核心狀態

| Method | Path | Auth | 說明 |
|------|------|:---:|------|
| GET | `/` | - | Dashboard HTML |
| GET | `/api/summary` | - | 系統摘要 |
| GET | `/events` | - | 事件串流 (SSE) |

### 5.2 Key 管理

| Method | Path | Body | 說明 |
|------|------|------|------|
| GET | `/api/keys` | - | 列出全部 keys |
| POST | `/api/keys` | `{label, key}` | 新增 key |
| PATCH | `/api/keys/:id` | `{label?, enabled?}` | 更新 key |
| DELETE | `/api/keys/:id` | - | 刪除 key |

### 5.3 叢集與設備

| Method | Path | 說明 |
|------|------|------|
| GET | `/api/known-nodes` | 已知節點清單 |
| GET | `/api/clients` | MQTT 客戶端清單 |
| GET | `/api/cloud/devices` | 雲端設備清單 |
| POST | `/api/cloud/devices` | 註冊新設備 |
| PATCH | `/api/cloud/devices/:cid/:did` | 更新設備 |
| POST | `/api/cloud/devices/from-node` | 從 MCU node 同步設備 |
| GET | `/api/cloud/clusters` | 叢集清單 |
| POST | `/api/cloud/clusters` | 建立叢集 |
| PATCH | `/api/cloud/clusters/:id` | 更新叢集 |

### 5.4 Provisioning 與憑證

| Method | Path | 說明 |
|------|------|------|
| GET | `/api/cloud/clusters/:id/provision` | 取得 provisioning data |
| POST | `/api/cloud/clusters/:id/rotate-ca` | 輪換 CA 憑證 |
| POST | `/api/cloud/clusters/:id/rotate-psk` | 輪換 PSK 密鑰 |
| POST | `/api/cloud/clusters/:id/apply-online` | 套用線上設定 |
| POST | `/api/cloud/apply/:nodeId` | 對指定 node 套用設定 |
| POST | `/api/provision` | MCU HTTPS provisioning |
| POST | `/api/devices/:deviceId/claim` | 宣告設備所有權 |
| GET | `/api/cloud/devices/:cid/:did/provision` | 設備 provisioning 狀態 |
| POST | `/api/cloud/devices/:cid/:did/rotate-credential` | 輪換設備憑證 |

### 5.5 MCU 命令轉發 (MQTT)

| Method | Path | Body | 說明 |
|------|------|------|------|
| POST | `/send/:nodeId/relay` | `{channel, value}` | 單路 Relay ON/OFF |
| POST | `/send/:nodeId/mask` | `{mask}` | 全部 Relay mask |
| POST | `/send/:nodeId/all` | `{value}` | 全部 Relay 同值 |
| POST | `/send/:nodeId/auto-hold` | `{channel, target, duration_seconds, session, command_ts, reason}` | Auto Hold |
| POST | `/send/:nodeId/control` | `{channel, target, pause_mode, pause_duration_seconds?, pause_until?, pause_blocks_auto_trigger?}` | 手動控制 |
| POST | `/send/:nodeId/scheduler/set` | `{channel, weekday, count, s0..s7, e0..e7}` | 排程設定 |
| POST | `/send/:nodeId/scheduler/enabled` | `{channel, enabled}` | 排程啟用/關閉 |
| POST | `/send/:nodeId/scheduler/pause` | `{channel, mode, duration_seconds?, until?, pause_blocks_auto_trigger?}` | 暫停排程 |
| POST | `/send/:nodeId/scheduler/resume` | `{channel}` | 恢復排程 |
| POST | `/send/:nodeId/config/protection` | 全域保護欄位 | 全域保護設定 |
| POST | `/send/:nodeId/protection/strategy` | `{channel, strategy}` | 單路保護策略 |
| POST | `/send/:nodeId/protection/force-stop` | `{channel}` | 強制停止 |
| POST | `/send/:nodeId/protection/unlock` | `{channel}` | 解鎖保護 |
| POST | `/send/:nodeId/protection/clear-learning` | `{channel}` | 清除 CT 學習 |

### 5.6 MCU 設定讀取 (HTTP Proxy)

| Method | Path | 說明 |
|------|------|------|
| GET | `/api/config/:nodeId/relays` | 讀取 relay 組態 |
| POST | `/api/config/:nodeId/device-name` | 設定裝置名稱（透過 MQTT `config/cluster` 轉發，等 MCU ack 後回傳 completed） |
| POST | `/api/config/:nodeId/relay` | 設定單路 relay |

### 5.7 感測器 Auto Hold API ★

| Method | Path | Body | 說明 |
|------|------|------|------|
| POST | `/api/sensor/alert` | `{channel, target, duration_seconds, reason, cluster_id?, device_id?, node_id?}` | 感測器告警 → 建立/續約 Auto Hold |
| POST | `/api/sensor/recover` | `{channel, cluster_id?, device_id?}` | 感測器恢復 → 結束 Auto Hold |
| GET | `/api/sensor/status` | - | 查詢所有 active 感測器 session |

**Alert 流程:**
```
POST /api/sensor/alert { channel:3, target:1, duration_seconds:600, reason:"..." }
  → 後端: 產生 session, command_ts, 解析 target node
  → MQTT → MCU: scheduler_handle_auto_hold()
  → MCU: auto_hold_active=1, Scheduler 暫停干預
  → 續約節流: 新告警 AND ≥5min 才重送 MQTT
  ← { ok:true, session:4210395328, mqtt:"sent", is_new_session:true }
```

**Recover 流程:**
```
POST /api/sensor/recover { channel:3 }
  → 後端: MQTT duration_seconds=0 (同 session)
  → MCU: auto_hold_clear() → 交回 Scheduler
  ← { ok:true, cleared:true, session:4210395328 }
```

---

## 6. MCU HTTP API 完整參考

**Base:** `http://192.168.0.{157,245}:80`  
**Auth:** `X-Auth-Token` header (from `POST /api/login`)

### 6.1 認證

| Method | Path | Body | 說明 |
|------|------|------|------|
| POST | `/api/login` | `password=admin` | 取得 token |
| POST | `/api/login` | `password=admin&scope=service&service_key=K` | Service scope |
| POST | `/api/logout` | - | 登出 |
| GET | `/api/session` | - | 檢查 session |

### 6.2 Relay 控制

| Method | Path | Body | Auth | 說明 |
|------|------|------|:---:|------|
| GET | `/api/relay` | - | - | 讀取 16 路狀態 |
| POST | `/api/relay-channel` | `channel=N&value=0\|1` | ✅ | 單路 ON/OFF |
| POST | `/api/relay-mask` | `mask=65535` | ✅ | 全部設定 |

### 6.3 叢集

| Method | Path | Body | Auth | 說明 |
|------|------|------|:---:|------|
| GET | `/api/cluster/nodes` | - | ✅ | 節點清單 |
| POST | `/api/cluster/node-relay` | `node_id=X&channel=N&value=0\|1` | ✅ | 遠端控制 |
| POST | `/api/cluster/kick` | `node_id=X` | ✅ | 踢除節點 |
| POST | `/api/cluster/forget` | `node_id=X` | ✅ | 移除節點 |

### 6.4 設定

| Method | Path | Body | Auth | 說明 |
|------|------|------|:---:|------|
| GET | `/api/config/cluster` | - | ✅ | 叢集+cloud 設定 |
| POST | `/api/config/cluster` | 多欄位 | ✅ | 更新設定 |
| GET | `/api/config/relays` | - | ✅ | 全部 relay 設定 |
| GET | `/api/config/relays-compact` | - | ✅ | compact relay 設定，供 Master 轉發 Member 查詢 |
| POST | `/api/config/relay` | `channel=N&name=X...` | ✅ | 單路 relay |

### 6.5 安全保護

| Method | Path | Body | Auth | 說明 |
|------|------|------|:---:|------|
| GET | `/api/protection` | - | ✅ | 16 路保護狀態 |
| GET | `/api/protection-compact` | - | ✅ | compact 保護狀態，供 Master 轉發 Member 查詢 |
| GET | `/api/config/protection` | - | ✅ | 全域保護參數 |
| POST | `/api/config/protection` | 多欄位 | ✅ | 更新全域保護 |
| POST | `/api/protection/strategy` | `channel=N&strategy=0\|1` | ✅ | 設定策略 |
| POST | `/api/protection/force-stop` | `channel=N` | ✅ | 強制停止 |
| POST | `/api/protection/clear-learning` | `channel=N` | ✅ | 清除學習 |
| POST | `/api/protection/unlock` | `channel=N` | ✅ | 解鎖故障 |

### 6.6 排程器

| Method | Path | Body | Auth | 說明 |
|------|------|------|:---:|------|
| GET | `/api/relay/channels/:ch/schedule-week` | - | ✅ | 一週排程 |
| PUT | `/api/relay/channels/:ch/schedule-week/:wd` | `count=N&s0=M&e0=N...` | ✅ | 覆寫某天 |
| POST | `/api/relay/channels/:ch/schedule-week/:wd/copy` | `target_weekdays=1,2&mode=overwrite` | ✅ | 複製排程 |
| PATCH | `/api/relay/channels/:ch/scheduler/enabled` | `enabled=0\|1` | ✅ | 啟用/關閉 |
| POST | `/api/relay/channels/:ch/scheduler/pause` | `mode=0-3&duration_seconds=N&pause_blocks_auto_trigger=0\|1` | ✅ | 暫停；`pause_blocks` 為相容 alias |
| POST | `/api/relay/channels/:ch/scheduler/resume` | - | ✅ | 恢復 |

### 6.7 Auto Hold / 手動控制

| Method | Path | Body | Auth | 說明 |
|------|------|------|:---:|------|
| POST | `/api/relay/channels/:ch/auto-hold` | `target=0\|1&duration_seconds=N&session=S&command_ts=T&reason=R` | ✅ | Auto Hold |
| POST | `/api/relay/channels/:ch/control` | `target=0\|1&pause_mode=0-3&pause_duration_seconds=N&pause_blocks_auto_trigger=0\|1` | ✅ | 手動控制；`pause_blocks` 為相容 alias |
| GET | `/api/relay/channels/:ch/scheduler/status` | - | ✅ | 完整狀態 |

### 6.8 全域狀態

| Method | Path | Auth | 說明 |
|------|------|:---:|------|
| GET | `/api/status` | - | 完整 JSON (cluster, adc, cloud, http, rtos, lwip...) |
| GET | `/` | - | Web UI (68KB SPA) |

---

## 7. Flash 佈局

| 區域 | Single-bank | Dual-bank | 用途 |
|------|------|------|------|
| Firmware IROM | 0x08000000–0x080BFFFF | 0x08000000–0x080BFFFF | Keil scatter/Target Options 必須限制為 768KB |
| Sector 7 erase base | 0x080C0000 | - | Scheduler Flash sector，整個 sector 保留 |
| Scheduler data address | 0x080E0000 | 0x081A0000 | Scheduler payload 寫入位址 |
| Sector 10 | 0x08180000 | - | Config Slot A |
| Sector 22 | - | 0x081C0000 | Config Slot A |
| Sector 11 | 0x081C0000 | - | Config Slot B |
| Sector 23 | - | 0x081E0000 | Config Slot B |

Flash 寫入規則：

- 所有 `HAL_FLASHEx_Erase` / `HAL_FLASH_Program` 必須經過 `flash_storage` mutex。
- single-bank Flash erase/program 期間必須保存 PRIMASK、disable IRQ，完成後恢復原 PRIMASK。
- 不可從 ISR 或未受控的 lwIP callback 直接寫 Flash。
- Scheduler 對外只允許 copy getter + setter；外部不得持有 writable raw pointer 修改排程狀態。
- Keil 必須同時設定 Target IROM1=`0x08000000/0x000C0000`，且 Linker 使用 `MDK-ARM/autoControl/autoControl.sct`；不能只手改 `.sct`。
- Rebuild 後 map file 必須出現 `RW_DTCM`、`RW_SRAM1`、`RW_SRAM2`，且 `ucHeap=0x20000000`、ETH DMA/RX pool 位於 `0x2007C000–0x2007FFFF`。

### A/B Slot 格式

```
Slot Header (16 bytes):
  [0]  magic    = 0x43464732 ("CFG2")
  [4]  sequence = 單調遞增
  [8]  crc      = CRC32 of config data
  [12] commit   = 0x43474744 ("CGD") when valid

Config Data: device_config_t (~3.6KB)

Save: 寫入 alternate slot → verify CRC → write commit marker
Load: 選 magic + commit + CRC 都有效且 sequence 最高的 slot
      若 primary slot 損壞 → fallback 到另一個 slot
```

---

## 8. RTC Backup Register 佈局

| Register | 用途 |
|------|------|
| DR0 | Fault Signature |
| DR1 | Fault Type |
| DR2 | Fault PC |
| DR3 | Fault LR |
| DR4 | Fault CFSR |
| DR5 | Fault HFSR |
| DR6 | Scheduler Magic (low16) + Enabled Mask (hi16) |
| DR7 | RTC Signature (Cluster Time restore) |
| DR8 | Boot Counter |
| DR9 | Relay Protection Restart Detection |
| DR10–DR25 | Scheduler Pause Until (16 channels) |
| DR26–DR31 | Free |

---

## 9. SRAM 佈局

| 區域 | 位址 | 大小 | 用途 |
|------|------|------|------|
| **DTCM** | 0x20000000 | 128KB | ucHeap (60KB FreeRTOS heap) — CPU only, DMA 不觸及 |
| **SRAM1** | 0x20020000 | 368KB | RW/ZI data + lwIP heap (48KB linker-managed) — DMA accessible |
| **SRAM2** | 0x2007C000 | 16KB | ETH DMA descriptors + TX buffers + RX pool |

### Task Stack 配置

| Task | Stack | 說明 |
|------|------|------|
| TCPIP | 24KB | TLS/MQTT/HTTPS callbacks |
| DefaultTask | 16KB | Main loop, scheduler tick |
| CfgSave | 16KB | Flash write operations |
| ControlQueue | 8KB | Command serialization |
| Discovery | 4KB | UDP announce |
| ClusterHealth | 4KB | Health check |
