# autoControl 後端管理台

`tools/backend-test` 是目前的後端參考實作。它不是最終 React 版本，但必須具備足夠完整且可維護的功能，讓之後重構時能直接照這份行為重做。

## 1. 執行方式

```powershell
cd C:\Users\Public\Code\stm32\f767\autoControl
npm install --prefix tools\backend-test
npm start --prefix tools\backend-test
```

啟動後會提供：

- 後端管理台：`http://127.0.0.1:4300/`
- MQTT TLS-PSK Broker：`:8883`
- Broker 控制 API：`http://127.0.0.1:4301`

## 2. 登入

預設登入帳密：

- 帳號：`admin123`
- 密碼：`admin123`

支援的登入 API：

- `POST /api/auth/login`
- `POST /api/auth/logout`
- `GET /api/auth/session`

## 3. 後端管理台的角色

這個頁面是給管理者用的，不是 MCU 本機 Web。

它主要負責：

- 多群組 / 多節點管理
- Relay 與保護設定
- 排程器管理
- 雲端認領與群組註冊
- API 金鑰管理
- 事件紀錄檢視

設計原則：

- 頁面資訊要可讀，不能把原始 JSON 直接丟給使用者看。
- 寫入操作要等 MCU 或 Broker ack 完成後，才算完成。
- 若資料可由 MCU 或 broker 提供，優先顯示已同步的狀態。
- 不要用 prompt 代替正式表單。

## 4. 核心流程

### 4.1 認領節點

管理台可透過：

- `POST /api/cloud/claim-node`

把 `Node ID` 與雲端設備、群組綁在一起。

### 4.2 MCU Provisioning

MCU 會透過：

- `POST /api/provision`

向後端取得憑證、Broker 資訊與群組綁定資料。

### 4.3 Relay 與保護寫入

寫入 API 會經過 MQTT Broker 轉發，並等待 ack。

常用 API：

- `POST /api/config/:nodeId/relay`
- `POST /send/:nodeId/relay`
- `POST /send/:nodeId/all`
- `POST /send/:nodeId/protection/force-stop`
- `POST /send/:nodeId/protection/unlock`
- `POST /send/:nodeId/protection/clear-learning`
- `POST /send/:nodeId/config/protection`

### 4.4 排程器

排程器頁面提供：

- 啟用 / 停用排程
- 暫停 / 恢復
- 單日排程編輯
- 單日複製
- 整週複製到其他通道

相關 API：

- `GET /api/cluster/node/:nodeId/relay/channels/:channel/scheduler/status`
- `GET /api/cluster/node/:nodeId/relay/channels/:channel/schedule-week`
- `PUT /api/cluster/node/:nodeId/relay/channels/:channel/schedule-week/:weekday`
- `POST /api/cluster/node/:nodeId/relay/channels/:channel/schedule-week/:weekday/copy`
- `POST /api/cluster/node/:nodeId/relay/channels/:channel/scheduler/enabled`
- `POST /api/cluster/node/:nodeId/relay/channels/:channel/scheduler/pause`
- `POST /api/cluster/node/:nodeId/relay/channels/:channel/scheduler/resume`

## 5. 目前實作重點

- `device-name` 與設定寫入已改成等待 MQTT ack 後回傳完成。
- 後端管理台的摘要區改成可讀資訊，不再直接顯示 JSON。
- Relay / 保護 / 排程器頁面改成和 MCU Web 一致的功能分區。
- 行動版版面需要維持可用，不能讓按鈕與欄位掉出可視範圍。

## 6. 注意事項

- `GET /api/clients` 只是 broker client 狀態，不等於 MCU 每個功能都已完成。
- 若某些功能缺少 ack，前端應顯示「進行中」或「等待回應」，不要假裝已完成。
- 若要追查異常，請先看 `server.js` 的實際 API 行為，再看 `dashboard.html`。
