# backend-test

這是一個給 `autoControl` 韌體測試 TCP 長連線用的簡單後端。

它提供兩個面向：

- TCP 連線端
  - 讓叢集 `master` 主動連入
  - 接收 `hello / heartbeat / status`
  - 下發 `set_relay / set_mask / set_all`
- HTTP 管理端
  - 顯示已連線節點
  - 顯示事件紀錄
  - 管理真正的後端金鑰
  - 手動送控制命令

## 啟動

```powershell
cd tools/backend-test
npm install
npm start
```

啟動後：

- HTTP UI: `http://127.0.0.1:4300/`
- TCP listener: `42001`

## 金鑰機制

這版已經不是單一硬編碼金鑰。

- 金鑰會保存在：
  - `tools/backend-test/data/keys.json`
- 可在後端 UI 直接：
  - 產生新金鑰
  - 啟用 / 停用金鑰
  - 刪除金鑰
- 韌體端 `hello.key` 必須對應到這裡某一把已啟用金鑰，否則會收到：
  - `hello_ack { ok:false, error:"bad_key" }`

首次啟動時，如果 `keys.json` 不存在，會自動建立一把：

- label: `bootstrap`
- key: `BACKEND_KEY` 環境變數
- 若未提供 `BACKEND_KEY`，則預設為：
  - `dev-key`

## 環境變數

- `HTTP_PORT`
- `TCP_PORT`
- `BACKEND_KEY`
- `DUPLICATE_MASTER_GRACE_MS`
- `DUPLICATE_MASTER_SCAN_MS`
- `CLIENT_STALE_MS`

預設值：

- `HTTP_PORT=4300`
- `TCP_PORT=42001`
- `BACKEND_KEY=dev-key`
- `DUPLICATE_MASTER_GRACE_MS=15000`
- `DUPLICATE_MASTER_SCAN_MS=2000`
- `CLIENT_STALE_MS=12000`

## HTTP API

- `GET /`
  - 後端管理 UI
- `GET /api/summary`
- `GET /api/keys`
- `POST /api/keys`
- `PATCH /api/keys/:id`
- `DELETE /api/keys/:id`
- `GET /clients`
- `GET /events`
- `POST /broadcast/ping`
- `POST /send/:nodeId/relay`
- `POST /send/:nodeId/mask`
- `POST /send/:nodeId/all`

## 範例

控制 master 本機 relay：

```powershell
curl.exe -X POST http://127.0.0.1:4300/send/579948958/relay ^
  -H "Content-Type: application/json" ^
  --data "{\"channel\":1,\"value\":1}"
```

透過 master 轉送控制 member：

```powershell
curl.exe -X POST http://127.0.0.1:4300/send/579948958/relay ^
  -H "Content-Type: application/json" ^
  --data "{\"targetNodeId\":1165657530,\"channel\":1,\"value\":1}"
```

## 容錯邏輯

- 同一組 `key + cluster` 若出現多個 master：
  - 先進入 grace period
  - 超時後只記：
    - `duplicate-master-warning`
  - 不主動斷線
- stale client：
  - 只記：
    - `stale-client-warning`
  - 不主動斷線
- 同一個 `nodeId` 若有新連線進來：
  - 新連線取代舊連線
