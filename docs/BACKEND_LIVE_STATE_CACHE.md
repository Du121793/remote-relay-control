# 後端即時狀態快取

這份文件說明後端管理台目前採用的即時狀態與細節快取策略。

## 1. 目的

後端管理台同時要服務：

- 總覽頁
- Relay / 保護 / 排程器頁
- 雲端與節點管理頁

如果每次畫面 render 都重新查一次所有節點，會造成：

- request 過多
- 連線延遲增加
- 畫面與實際狀態不同步

因此要分成兩層：

1. 即時狀態快取
2. 細節查詢快取

## 2. 即時狀態快取

即時狀態來源以 broker / backend 的同步資料為主，通常包含：

- `summary`
- `clients`
- `knownNodes`
- `cloudDevices`
- `cloudClusters`
- `events`

這些資料可透過：

- `GET /api/dashboard/live`
- `GET /api/stream`

取得。

特性：

- 用來畫總覽頁與節點清單。
- 更新頻率可以較高，但不應在每次畫面渲染都重新抓。
- 若 SSE 失效，可退回到低頻輪詢。

## 3. 細節查詢快取

細節查詢是指比較重的資料，例如：

- `GET /api/config/:nodeId/relays`
- `GET /api/cluster/node/:nodeId/config/protection`
- `GET /api/cluster/node/:nodeId/relay/channels/:channel/scheduler/status`
- `GET /api/cluster/node/:nodeId/relay/channels/:channel/schedule-week`

這些資料不應該一直重複查。

建議原則：

- 同一節點、同一通道的資料先快取。
- TTL 不要太長，避免畫面長時間顯示舊資料。
- 寫入成功後，先清除對應快取，再主動重抓一次。

## 4. 建議 TTL

可用的基本參考：

- Relay 設定：30 秒
- 保護設定：30 秒
- 排程狀態：15 秒
- 排程週表：30 秒

若使用者剛剛手動操作成功，應立即失效快取並重抓。

## 5. 寫入後同步

任何會改變 MCU 狀態的操作，例如：

- 開 / 關 Relay
- 更新保護設定
- 更新排程
- 暫停 / 恢復排程

在 ack 回來後都應該做：

1. 失效對應快取
2. 重新抓取最新資料
3. 再把畫面渲染回來

這樣才不會出現「按鈕顏色已變，但實際狀態還沒同步」的問題。

## 6. ADC / CT 的顯示原則

單路電流或 CT 狀態必須以 MCU 上報的保護快照為主。

不要再用整台 ADC 數值代替單路數值。

如果 CT 未就緒：

- 顯示 `CT 未就緒`
- 不要顯示成 `0 mA`
- 保護邏輯應避免被誤判成正常讀值

## 7. 快取不是唯一真相

快取的作用是降低壓力，不是取代真實狀態。

因此應遵守：

- UI 優先看快取
- 寫入後立即同步
- 若發現快取過舊，就重新抓
- 若查詢失敗，要有 fallback，但要標示為 fallback

