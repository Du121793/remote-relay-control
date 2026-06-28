# 後端管理頁設計

## 1. 目標

`tools/backend-test/dashboard.html` 是之後搬到 React 的功能基準，不追求最終 UI，但必須具備可用的核心流程。

## 2. 必要功能

- 後端登入：測試版固定 `admin123 / admin123`。
- 多叢集選擇：可以篩選 cloud cluster。
- 裝置認領：輸入 Cloud Device ID / Node ID 建立或認領 device。
- 叢集總覽：顯示每個 cluster 的線上狀態與裝置數。
- 節點平鋪：進入 cluster 後應平鋪每台 MCU，不使用單一下拉選單隱藏 Member。
- Relay 控制：每台 MCU 顯示 16 路 relay card。
- 設定能力：能設定 relay、排程、保護；MCU 本機登入密碼不應由後端修改。

## 3. 後端 API 原則

- 管理頁 API 必須經過後端 session。
- `/api/provision` 必須保持公開給 MCU 使用，但 device 必須已認領。
- dashboard 使用的 API 必須全部存在；目前已補 `/api/events` 作為 `/events` 的相容路由。
- 後端不直接連 Member MCU；應透過 Master 或 broker 轉發。

## 4. Relay UI 原則

- relay card 按下控制後立即進入 pending 灰色狀態。
- 等待 ack / 狀態刷新後再解除 pending。
- 強制停止不應被一般控制 pending 凍結。
- Member 不應在 UI 中消失；即使只有 Master 連後端，也必須顯示 Master 上報的 known nodes。

## 5. API 與資料來源

主要資料來源：

- `/api/summary`
- `/api/known-nodes`
- `/api/clients`
- `/api/cloud/devices`
- `/api/cloud/clusters`
- `/api/events`
- `/api/stream`

裝置認領：

- `POST /api/cloud/claim-node`

Provisioning：

- `POST /api/provision`
- `GET /api/cloud/devices/:clusterId/:deviceId/provision`

## 6. 目前已知限制

- 測試版 claim 流程仍是輸入式信任，不是正式安全認領。
- 正式版應改成後端 challenge + MCU signature。
- 後端頁面仍是單檔 HTML/JS，之後應搬到 React。

## 7. 本輪同步規則

- 後端測試版登入預設為 `admin123 / admin123`；若部署端登入失敗，優先檢查部署環境變數 `DASHBOARD_USER`、`DASHBOARD_PASSWORD` 是否覆蓋預設值。
- `/api/cloud/devices` 與 `/api/cloud/clusters` 回傳前會先從 MQTT broker 的 `/api/clients` 同步線上狀態。
- 這可避免出現「broker 已連線，但後端裝置列表仍顯示離線」。
- Broker 只負責轉發命令與回報連線狀態；Dashboard 仍必須平鋪顯示 Master 與 Member，不能只顯示直接連線到後端的 Master。
- 節點清單只顯示節點、狀態、角色、叢集、IP、Relay、最後上報與操作；ADC/CT 不放在總欄，避免混淆成全域資訊。
- Member 由 Master 的 MQTT status `nodes` 陣列同步到 broker，再由後端 `/api/known-nodes` 展開。
