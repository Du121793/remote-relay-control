# autoControl 後端測試服務

`tools/backend-test` 是目前的後端參考實作。它不是最終 React 版本，但必須提供足夠完整的功能，讓未來搬遷時可以照這份行為重做。

## 1. 啟動

```powershell
cd C:\Users\Public\Code\stm32\f767\autoControl
npm install --prefix tools\backend-test
npm start --prefix tools\backend-test
```

預設服務：

- 後端網頁與 REST API：`http://127.0.0.1:4300/`
- MQTT TLS-PSK broker：`:8883`
- Broker HTTP 控制 API：`http://127.0.0.1:4301`

## 2. 後端登入

目前是測試用固定帳號：

- 帳號：`admin123`
- 密碼：`admin123`

登入 API：

- `POST /api/auth/login`
- `POST /api/auth/logout`
- `GET /api/auth/session`

除了下列 API，其餘 `/api/*`、`/send/*` 都需要登入 session：

- `/api/auth/login`
- `/api/auth/logout`
- `/api/auth/session`
- `/api/provision`

`/api/provision` 是 MCU 開機向後端取得 MQTT PSK 與 broker 設定用，不能要求使用者登入 cookie。

## 3. 多叢集與裝置認領

後端支援多個雲端叢集。Dashboard 可用「叢集」下拉選單篩選節點。

### 3.1 認領節點

Dashboard 的「雲端/叢集」頁面提供認領表單：

- 叢集 ID
- Node ID
- Device ID
- 顯示名稱

對應 API：

```http
POST /api/cloud/claim-node
Content-Type: application/json

{
  "clusterId": "default",
  "nodeId": 579948958,
  "deviceId": "node-579948958",
  "deviceName": "測試節點"
}
```

行為：

- 如果叢集不存在，後端會建立該叢集。
- 如果裝置不存在，後端會建立 cloud device 紀錄。
- 如果裝置已存在，會更新 `nodeId`、`deviceName`、`owner`。
- 認領完成後，MCU 仍需要透過 `/api/provision` 取得雲端連線設定。

目前認領是測試版信任後端輸入的 `nodeId`。正式版建議改為簽章認領流程：

1. 使用者在後端輸入 `node_id`。
2. 後端產生一次性 challenge。
3. 使用者把 challenge 放回 MCU。
4. MCU 用本機裝置私鑰或叢集金鑰簽章回傳。
5. 後端驗證簽章後才把該 device 綁定到使用者帳號。

這樣可避免任意使用者只知道 `node_id` 就搶先認領別人的設備。

## 4. MCU provisioning 流程

目前 MCU provisioning 使用：

```http
POST /api/provision
Content-Type: application/json

{
  "cluster_id": "default",
  "device_id": "node-579948958",
  "device_fingerprint": "..."
}
```

成功回應包含：

- `cloud_key_id`
- `cloud_transport`
- `cloud_broker_host`
- `cloud_broker_port`
- `cloud_cluster_ca_fingerprint`
- `cloud_cluster_ca_cert_pem`
- `cloud_cluster_binding_secret`
- `provisioning_revision`
- `ca_revocation_tag`

MCU 取得後寫入本機 ROM config，之後由 Master 連線 MQTT broker。

## 5. Dashboard 功能基準

目前後端 Dashboard 必須至少提供：

1. 簡單登入。
2. 多叢集選擇。
3. 以 `node_id` 認領設備。
4. 進入叢集後平鋪顯示所有節點。
5. 每個節點平鋪 16 路 relay。
6. 可對每台節點控制：
   - 單路 relay 開關
   - 全開 / 全關
   - 強制停止
   - 解除鎖定
   - 清除學習
   - scheduler enable / pause / resume
7. 可檢視：
   - 節點在線狀態
   - role：Master / Member
   - relay mask
   - CT 是否就緒
   - 目前電流
   - cloud device / cloud cluster registry

注意：後端只轉發命令給 broker 或 MCU HTTP proxy，實際 GPIO、排程器、保護判斷都在 MCU。

## 6. 重要 API

### 6.1 基本資料

- `GET /api/summary`
- `GET /api/clients`
- `GET /api/known-nodes`
- `GET /api/events`
- `GET /api/stream`

### 6.2 雲端叢集與裝置

- `GET /api/cloud/clusters`
- `POST /api/cloud/clusters`
- `PATCH /api/cloud/clusters/:clusterId`
- `POST /api/cloud/clusters/:clusterId/rotate-ca`
- `POST /api/cloud/clusters/:clusterId/rotate-psk`
- `GET /api/cloud/devices`
- `POST /api/cloud/devices`
- `PATCH /api/cloud/devices/:clusterId/:deviceId`
- `POST /api/cloud/claim-node`
- `POST /api/cloud/devices/:clusterId/:deviceId/reset-fingerprint`
- `POST /api/cloud/devices/:clusterId/:deviceId/rotate-credential`
- `POST /api/provision`

### 6.3 Relay / 設定

- `POST /send/:nodeId/relay`
- `POST /send/:nodeId/mask`
- `POST /send/:nodeId/all`
- `GET /api/config/:nodeId/relays`
- `POST /api/config/:nodeId/relay`

### 6.4 排程與保護

- `POST /send/:nodeId/scheduler/enabled`
- `POST /send/:nodeId/scheduler/pause`
- `POST /send/:nodeId/scheduler/resume`
- `POST /send/:nodeId/protection/force-stop`
- `POST /send/:nodeId/protection/unlock`
- `POST /send/:nodeId/protection/clear-learning`

## 7. 目前測試結論

燒錄後測試結果：

- 兩台 MCU HTTP API 都可回應。
- 破壞性 ROM v1 更新後，雲端設定被清空是預期行為。
- 本地叢集正常選主：目前 `192.168.0.57` 為 Master，`192.168.0.209` 為 Member。
- proxy pending queue 可連續收 4 筆遠端 proxy request。
- `control_queue.busy_rejected=0`。
- heap low watermark 維持約 `25504` bytes。

限制：

- MCU HTTP pool 目前只有 2 條連線。若瀏覽器同時開 4 條 TCP 連線打 MCU，仍可能有 connection reset。這不是 proxy queue 的問題。
- 不建議直接把 MCU HTTP pool 提高到 4，因為每條 connection 有約 `12KB response + 1.5KB request` buffer，RAM 會明顯增加。

## 8. Member 是否應該控制整個叢集

目前建議仍維持「Master 對外連後端、後端命令先到 Master，再由 Master 代理 Member」。

不建議現在解除成「任意 Member 都可以控制任意叢集成員」，原因：

- 權限面：每個 Member 都變成控制入口，攻擊面變大。
- 一致性面：多個節點同時代理控制，容易出現狀態競爭。
- 佇列面：relay control、scheduler、protection 都要保證單一排序來源，否則 UI 看到的 pending/confirmed 順序會亂。
- 叢集面：Master 已經是選主結果，也是雲端唯一連線點；讓 Member 全都具備控制代理會破壞目前簡單模型。

可以考慮的折衷：

- Member 可以控制自己本機 relay。
- Member 的 UI 若要控制其他節點，仍轉交目前 Master。
- 如果 Master 不可用，等叢集重新選主後再控制。

這樣比「所有 Member 都可互控」穩定，也比較容易驗證。

## 9. 線上狀態同步

- `GET /api/cloud/devices` 與 `GET /api/cloud/clusters` 會先查詢 broker control API `/api/clients`，再回傳雲端裝置與叢集摘要。
- 這是為了避免 MQTT broker 已看到 MCU 連線，但後端 registry 尚未收到 MCU 狀態訊息時，管理頁仍顯示離線。
- 若部署端 API 全部回 `401`，先確認後端登入帳密或環境變數是否與測試版預設 `admin123 / admin123` 一致。
- Broker `/api/clients` 會回傳每個 MQTT client 最近一次 status 中的 `nodes` 陣列；後端會用它建立 Member known-node，讓管理頁可以控制不直接連後端的 Member。
