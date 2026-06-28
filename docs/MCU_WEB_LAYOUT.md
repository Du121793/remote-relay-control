# MCU Web UI 設計

## 1. 目標

MCU Web 是本地操作介面，必須在桌機與手機都能完成基本控制、排程、保護與設定。

## 2. 頁面分工

- 儀表板：relay 狀態、控制、強停、設定入口、排程狀態提示。
- 排程器：設定每路一週 ON/OFF 時段、暫停/恢復排程、複製排程。
- 叢集：顯示本機 Node ID、角色、term、叢集節點、節點狀態與操作。
- 保護：顯示全域保護設定、每路保護狀態、學習資訊、策略與操作。
- 設定：裝置名稱、叢集名稱、後端連線、Cloud Device ID、本機登入密碼、本地叢集密碼。

## 3. 設定頁規則

- 不顯示 Cloud Key ID、Binding Secret、CA cert、Backend Key。
- Cloud Device ID 只讀。
- 後端連線提示使用小字，不得用過大字級。
- 「目前使用燒錄預設叢集密碼」只在變更本地叢集密碼區顯示一次。
- 本機登入密碼與本地叢集密碼必須分開。
- ROM/API 回空時，UI 應顯示合理預設值：
  - 裝置名稱：`autoControl`
  - 叢集名稱：`default`
  - Broker Host：`mqtt.autocontrol.du1217.club`
  - Broker Port：`8883`

## 4. 保護頁規則

- 策略說明應使用藍底小字提示，不應用大段正文。
- 桌機可用表格顯示。
- 手機必須用 CSS 將表格轉為卡片式顯示，避免欄位過窄。
- CT 未就緒時必須顯示「CT 未就緒，電流保護不會啟動」，不可顯示成 0mA。

## 5. Relay 控制規則

- 一般開關按下後，按鈕要進入 pending 灰色狀態。
- 收到結果或狀態更新後才解除 pending。
- 強制停止不應被一般 pending 凍結。
- 排程器正在管理時，手動控制前應詢問暫停到何時或停用排程。

## 6. 叢集頁規則

- Node ID 不得空白。
- Node ID fallback 順序：
  1. `/api/config/cluster.self_node_id`
  2. `/api/config/cluster.node_id`
  3. `/api/status.cluster.self_node_id`
  4. 目前 dashboard 已知狀態
- 手機版叢集節點表格應轉為卡片式顯示。

## 7. Web asset

修改 `Web/index.html` 後必須執行：

```powershell
python tools\regen_web.py
```

否則燒錄後仍會看到舊版 UI，因為韌體實際嵌入的是 `Web/web_assets.h`。

## 8. 輪詢與手機版規則

- 儀表板即時刷新應優先使用合併後的 `/api/dashboard/live` 與節點摘要，避免對每一路 relay 重複呼叫 scheduler/protection API。
- 本機節點不得走 cluster proxy；比較 Node ID 時必須先轉成數字，避免字串與數字比較造成自我代理。
- 手機版表格使用卡片式顯示；操作欄按鈕在窄螢幕下採單欄滿寬排列，避免「設定／強制停止／清除學習／切換策略」錯位。
