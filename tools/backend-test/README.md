# backend-test

`backend-test` 目前同時扮演三個角色：

1. 現有韌體的 backend 長連線對接端
2. 未來 cloud / registry / control plane 的本地模擬器
3. 給瀏覽器操作的管理與控制 UI

目前通訊分成兩層：

- `browser -> backend-test`
  - `HTTP + SSE`
- `master device -> backend-test`
  - 目前是 `plain TCP long link`
  - 未來預計升級成 `MQTT over TLS`

## 啟動

```powershell
cd tools/backend-test
npm install
npm start
```

預設：

- UI / HTTP API: `http://127.0.0.1:4300/`
- TCP backend link: `42001`

安全預設：

- 管理 HTTP 只監聽 `127.0.0.1`
- provisioning API 不回傳 device private key
- 僅限本機隔離測試時，才能用 `ALLOW_PRIVATE_KEY_EXPORT=1` 明確開啟 private key 匯出

相關環境變數：

- `HTTP_HOST`
- `HTTP_PORT`
- `TCP_PORT`
- `BACKEND_KEY`
- `ALLOW_PRIVATE_KEY_EXPORT`

## 已有功能

### 1. TCP 長連線接入

接受來自韌體 master 的：

- `hello`
- `heartbeat`
- `status`
- `fault_report`

並可下發：

- `set_relay`
- `set_mask`
- `set_all`

### 2. 金鑰管理

金鑰資料持久化於：

- `tools/backend-test/data/keys.json`

`keys.json` 是本機 runtime 資料，不納入 Git。版本庫只保留：

- `tools/backend-test/data/keys.example.json`

請使用 `BACKEND_KEY` 環境變數提供本機 bootstrap key，不要把真實 key 寫入範例檔或提交。

UI 與 API 支援：

- 新增金鑰
- 啟用 / 停用金鑰
- 刪除金鑰

若資料檔不存在，會自動建立一把 bootstrap key：

- `label = bootstrap`
- `key = BACKEND_KEY` 環境變數
- 若未設定 `BACKEND_KEY`，預設為 `dev-key`

### 3. 已知裝置與 relay 控制

backend 會維護：

- 目前 TCP 鏈路
- 已知裝置
- relay 設定快取

UI 可直接操作：

- master 本機 relay
- backend -> master -> member relay
- relay 名稱
- relay enabled
- relay remember_state
- relay boot_default_on
- 裝置名稱

### 4. cloud identity registry

backend 會從 master 上報的 payload 裡整理出兩份 registry：

- `cloud cluster identity`
- `cloud device identity`

資料持久化於：

- `tools/backend-test/data/cloud-clusters.json`
- `tools/backend-test/data/cloud-devices.json`

這兩份檔案是本機 runtime registry，可能包含 CA private key、device private key 與 binding secret，因此不納入 Git，也不可作為正式環境的安全儲存方案。

目前 registry 會記錄：

- `cloud_cluster_id`
- `cloud_device_id`
- `cloud_key_id`
- `cloud_transport`
- 對應 `nodeId`
- 對應裝置名稱 / 叢集名稱
- `online`
- `role`
- `relayMask`
- `lastSeenAt`

並支援：

- 預建 cluster identity
- 預建 device identity
- 啟用 / 停用 cluster identity
- 啟用 / 停用 device identity

當 cluster/device 被停用時：

- 現有 TCP client 會立即斷線
- 後續 `hello` 也會被拒絕

### 5. fault / crash 接收

韌體發生 fault 後會：

1. 將 fault 詳細資訊存到 RTC backup registers
2. 自動 reset
3. 開機重連 backend 後送出 `fault_report`

backend 目前會：

- 在 UI `事件紀錄` 顯示
- 在 console 直接 `console.error(...)`

## MQTT over TLS-PSK 測試服務

- MQTT broker：`0.0.0.0:8883`
- 本機控制 API：`http://127.0.0.1:4301`
- TLS：固定使用 TLS 1.2 PSK。
- PSK identity：device 的 `cloud_device_id`。
- PSK：cluster 的 `cloud_cluster_binding_secret` 經 base64url 解碼後的 32 bytes。
- MQTT username/password 仍會比對 `cloud_key_id` 與原始 binding secret。

啟動與桌面 smoke test：

```powershell
npm run mqtt
npm run mqtt:smoke
```

控制 API：

- `GET /api/summary`
- `GET /api/clients`
- `GET /api/events`
- `POST /api/publish`

裝置訂閱 `cluster/<clusterId>/command/time` 後，測試 broker 會自動下發：

```json
{
  "request_id": "broker-time-...",
  "unix_time": 1781367600
}
```

master 寫入 LSE RTC，並透過 discovery announce 將時間同步給 member。

`POST /api/publish` 範例：

```json
{
  "topic": "cluster/default/command/relay",
  "payload": {
    "request_id": "test-1",
    "node_id": 1165657530,
    "channel": 3,
    "value": 1
  }
}
```

relay queue 滿載時，裝置會在 ack 回覆：

```json
{
  "ok": false,
  "queued": 0,
  "completed": 0,
  "error": "queue_full"
}
```

繼電器設定使用獨立 topic：

```json
{
  "topic": "cluster/default/command/config/relay",
  "payload": {
    "request_id": "config-test-1",
    "node_id": 1165657530,
    "channel": 16,
    "name": "MqttMember16",
    "enabled": 1,
    "remember_state": 0,
    "boot_default_on": 0,
    "adc_channel": 255,
    "protection_enabled": 0,
    "overcurrent_milliamps": 4500,
    "trip_delay_ms": 1000,
    "reset_hysteresis_milliamps": 500
  }
}
```

`node_id=0` 或省略時設定目前 master；指定 member node ID 時，由 master 透過
member HTTP 設定介面保存。設定命令只有在 Flash 保存完成後才回覆完成 ACK：

```json
{
  "ok": true,
  "queued": 1,
  "completed": 1,
  "request_id": "config-test-1",
  "target_node_id": 1165657530,
  "error": ""
}
```

一般 relay/mask 命令的 ACK 表示已接受進入 control queue，會是
`queued=1, completed=0`；設定與時間命令則以 `completed=1` 表示持久化或時間套用已完成。

## HTTP API

### UI 與狀態

- `GET /`
- `GET /api/stream`
- `GET /api/summary`
- `GET /clients`
- `GET /api/clients`
- `GET /api/known-nodes`
- `GET /events`

### 金鑰

- `GET /api/keys`
- `POST /api/keys`
- `PATCH /api/keys/:id`
- `DELETE /api/keys/:id`

### cloud registry

- `GET /api/cloud/devices`
- `GET /api/cloud/clusters`
- `POST /api/cloud/clusters`
- `POST /api/cloud/devices`
- `PATCH /api/cloud/devices/:clusterId/:deviceId`
- `PATCH /api/cloud/clusters/:clusterId`

### 裝置設定 / relay 設定

- `POST /api/config/:nodeId/device-name`
- `GET /api/config/:nodeId/relays`
- `POST /api/config/:nodeId/relay`

### 控制命令

- `POST /send/:nodeId/relay`
- `POST /send/:nodeId/mask`
- `POST /send/:nodeId/all`
- `POST /broadcast/ping`

## relay 控制策略

目前 backend UI 為了減少 request 洪水，策略是：

1. 按下按鈕先進入 pending
2. 同一台節點 `500ms` 內的多次切換合併成一個 `mask`
3. 命令送出後，不再先假改 server 狀態
4. 由 backend 背景做狀態確認
5. 真實狀態一致後才解凍

另外：

- 同一台節點只保留一個 in-flight confirm
- remote login token 會快取 `60s`
- stale client 不再假裝 `200 OK`

## stale / offline 規則

預設：

- `CLIENT_STALE_MS = 12000`
- `DUPLICATE_MASTER_SCAN_MS = 2000`

backend 會在下列情況把裝置標成離線：

- TCP socket `close`
- TCP socket `error`
- 超過 stale 時間沒有新訊息

## 未來方向

這個 backend-test 已開始承擔未來 cloud 的資料模型，但目前真正的 cloud transport 還沒切到 MQTT/TLS。

後續規劃：

1. `master -> cloud` 從 raw TCP 過渡到 `MQTT over TLS`
2. 保留：
   - 每台裝置獨立 `device identity`
   - 每個叢集獨立 `cluster identity`
3. backend-test 從目前 TCP emulator 逐步演進成：
   - MQTT broker bridge
   - cloud registry UI
   - control plane simulator
