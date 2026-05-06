# mem

## 2026-05-06 最新接手重點

- HTTP 現況已重新判斷：目前不是先前 `GET /` 首頁 body 截斷，而是 listener 沒穩定建立，症狀是瀏覽器連 port 80 被拒絕，且 UART 沒有 `[http] up 80`。
- 已檢查 [FINISH.md](/C:/Users/Public/Code/stm32/f767/autoControl/FINISH.md) 以前的 HTTP 問題：
  - 舊問題：首頁過大、close 太早、body 截斷
  - 目前問題：更前面，HTTP listener 建立路徑不穩
- 已將 [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c) 改成不再依賴獨立 `HttpService` task：
  - 移除 task create 路徑
  - `http_service_poll()` 改為直接 `tcpip_try_callback(http_service_tcpip_step, NULL)`
- 已在 [Core/Src/main.c](/C:/Users/Public/Code/stm32/f767/autoControl/Core/Src/main.c) 的 `StartDefaultTask()` 主迴圈中加入 `http_service_poll();`
- 最新 build：
  - `Program Size: Code=107366 RO-data=13426 RW-data=15940 ZI-data=50852`
  - `0 Error(s), 0 Warning(s)`

## 2026-05-06 Web relay 延遲新結論

- 使用者最新回報：首頁已經能打開，但 Web UI 控制 relay 回應極慢，久了網頁又會變不穩。
- 重新審查後確認：HTTP 與 TCP control 都是在 lwIP `tcpip` thread 裡處理請求，原本又同步呼叫 `control_queue_submit()` 等 worker 完成，這會阻塞整個 lwIP thread。
- 已改成：
  - [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c)：HTTP relay API 直接 `relay_domain_execute(...)`
  - [App/Service/control_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/control_service.c)：TCP control 也直接 `relay_domain_execute(...)`
- 另外已放寬 HTTP 連線資源：
  - `HTTP_CONN_POOL_SIZE: 4 -> 8`
  - `tcp_listen_with_backlog(...): 2 -> 4`
- 最新 build：
  - `Program Size: Code=107038 RO-data=13426 RW-data=15940 ZI-data=50932`
  - `0 Error(s), 0 Warning(s)`

## 2026-05-06 全面審查補充

- 沒有看到 thread / queue 被無限重建的直接問題；主要 task/queue 都有單次建立守門。
- 但發現一個更可能讓「久了整體變慢」的點：
  - [App/Service/control_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/control_service.c) 原本送完 response 就立即 `tcp_close()`
  - 這對短連線高頻控制很危險，因為 send queue 未清空時會產生未乾淨回收的 PCB 風險
- 已修正 `control_service` 關閉時序：
  - 改為 `tcp_sent` / `tcp_poll` 等待 queue 清空後才 close
  - 拿掉共享 `static client_ctx`

## 專案現況

- 專案：`STM32F767ZI + FreeRTOS + lwIP + Keil/ARMCLANG`
- 主要功能：
  - 乙太網路 DHCP
  - TCP control service
  - UDP discovery service
  - HTTP service / Web UI
  - 16 路 relay 控制

## 已完成的重要基線

- 大量 lwIP `UNALIGNED HardFault` 已處理
  - `ip4.c`
  - `dhcp.c`
  - `etharp.c`
  - `icmp.c`
  - `igmp.c`
  - `pbuf.c`
  - `tcp_in.c`
  - `cc.h`
- ETH RX custom pbuf 初始化已改成安全手動路徑
- `HAL_ETH_ReleaseTxPacket()` 已改成只在有 pending TX 時才呼叫
- PHY fallback 已改成：
  - `100HD`
  - `10HD`
- `control_service` / `discovery_service` / `http_service` 都已 RTOS 化

## 目前正在追的問題

- 目前不是 HardFault
- 不是 scheduler 卡死
- 不是 `osDelay(100)` 卡死
- 不是 `HAL_ETH_Start()` 前的 PHY/MAC bring-up
- 最新卡點收斂在：
  - `ethlink start mac ok`
  - `ethlink netif link up`
  - 之後沒有更多 log

## 最新技術判讀

### 第一階段判讀

- 原先懷疑：
  - `tcpip_mbox` 被 service tasks 的 `tcpip_callback()` 塞滿
- 已做修正：
  - `control_service_task()` 改成 `tcpip_try_callback()`
  - `discovery_service_task()` 改成 `tcpip_try_callback()`
  - `http_service_task()` 改成 `tcpip_try_callback()`
  - `ethernetif_schedule_netif_callback()` 改成 `tcpip_try_callback()`
  - `TCPIP_MBOX_SIZE` 從 `6` 拉到 `16`
- 但使用者最新 log 顯示，問題仍停在：
  - `ethlink netif link up`

### 第二階段判讀

- `netif_set_link_up()` 內部會先跑：
  - `dhcp_network_changed(netif)`
- 專案原本在 [lwip.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/App/lwip.c) 的 `MX_LWIP_Init()` 裡就先 `dhcp_start(&gnetif)`
- 這可能讓 `netif_set_link_up()` 在 link up 路徑裡背到 DHCP 狀態轉換

## 最新修正

### 1. 延後 DHCP 啟動時機

- 已從 [lwip.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/App/lwip.c) 移除：
  - `dhcp_start(&gnetif);`
- 改成在 [ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c) 的 `ethernetif_netif_up_callback()` 裡：
  1. `netif_set_up()`
  2. `netif_set_link_up()`
  3. `dhcp_start(netif)`

### 2. 加入 callback 內部最小定位 log

- 新 log：
  - `netif cb up a`
  - `netif cb up b`
  - `netif cb up c`
  - `netif cb dhcp start`
  - `netif cb up d`

## 下一輪要怎麼判讀

- 如果只看到：
  - `netif cb up a`
  - `netif cb up b`
  - 然後停住
  - 代表卡在 `netif_set_link_up()`

- 如果看到：
  - `netif cb up c`
  - 但沒有 `netif cb dhcp start`
  - 代表卡在 `dhcp_start()`

- 如果全部都有：
  - 下一步就看：
    - `[net] tx ok ...`
    - DHCP Discover 抓包
    - DHCP state 是否從 `selecting` 前進

## 使用者最新實測結論

- 抓包目前看不到任何 DHCP request
- 最新幾輪 log 都停在：
  - `ethlink start mac ok`
  - `ethlink netif link up`

## 最新 build

- `Program Size: Code=108350 RO-data=14082 RW-data=15940 ZI-data=33508`
- `0 Error(s), 0 Warning(s)`

## 2026-05-06 重新審查後的接手摘要

### 這一輪真正確認的問題點

- 先前的診斷程式碼已經過度入侵送包路徑，不能再拿那些 log 當根因
- 兩個最明顯的回歸：
  - `HardFault_Handler()` 被改成空死循環，真 fault 會完全無聲
  - DHCP / lwIP / HAL ETH TX 內被插入大量同步 `HAL_UART_Transmit()`，這會直接污染第一個封包的發送路徑

### 這一輪已做的事

- 恢復 [Core/Src/stm32f7xx_it.c](/C:/Users/Public/Code/stm32/f767/autoControl/Core/Src/stm32f7xx_it.c) 的 `HardFault_Handler()`：
  - 改回進入既有 `hardfault_dump()` 的 naked wrapper
- 移除深層同步診斷：
  - [Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c](/C:/Users/Public/Code/stm32/f767/autoControl/Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c)
  - [LWIP/Target/ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c)
  - [Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_eth.c](/C:/Users/Public/Code/stm32/f767/autoControl/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_eth.c)
- 保留高層狀態：
  - link up/down
  - DHCP 狀態摘要
  - 必要的 PHY fallback 訊息

### 為什麼這樣改

- 使用者最後看到停在：
  - `dhcp_discover()` 之後
  - 或 `ethlink netif link up` 一帶
- 但那時 code 已經在：
  - lwIP core
  - ethernetif TX path
  - HAL ETH driver
  內部都插了 blocking UART
- 這會讓「沒看到封包」和「真的沒送到 MAC/DMA」兩件事失去可分辨性

### 最新 build

- `Program Size: Code=107366 RO-data=14066 RW-data=15940 ZI-data=33500`
- `0 Error(s), 0 Warning(s)`

### 下一步應該怎麼測

1. 燒最新韌體
2. 看是否：
   - 若再次 fault，現在會重新印出 `[fault] HardFault ... PC=...`
   - 若不 fault，是否開始真的出現 DHCP Discover
3. 同步抓包確認：
   - `DHCP Discover` 是否上線
   - 若還是沒有，下一步直接回頭看 `udp_sendto_if_src -> ip4_output_if_src -> ethernet_output -> low_level_output`

### 接手重點

- 不要再把同步 UART log 插進：
  - `dhcp.c`
  - `udp.c`
  - `ip4.c`
  - `ethernet_output()`
  - `HAL_ETH_Transmit()`
- 如果下一輪還要定位，優先用：
  - 恢復後的 HardFault dump
  - 高層摘要 log
  - 抓包

## 2026-05-06 新確認的真 fault 點

- 使用者重燒這版後終於拿到真正 fault：
  - `PC=0x0800EBE8`
  - `LR=0x0800EB2F`
  - `CFSR=0x01000000`
- 反組譯對回 [LWIP/Target/ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c) 的 `low_level_output()`
- 不是 DHCP state machine 本身炸掉，而是：
  - `low_level_output()` 在真正送包前先解析 `tx_dma_buffer`
  - 讀了像 `tx_dma_buffer + 2`、`+26`、`+30` 這種非 4-byte 對齊位置的 32-bit 值
  - `armclang` 產生的 `LDR` 直接觸發 `UNALIGNED UsageFault`

### 這次已修正

- 移除 `low_level_output()` 內部那段非必要的 TX frame 解析/統計
- 保留真正必要的：
  - TX pbuf 複製到 DMA buffer
  - `SCB_CleanDCache_by_Addr(...)`
  - `HAL_ETH_Transmit(...)`

### 這代表什麼

- 之前「DHCP 根本沒送出去」至少有一部分是因為第一個 TX 路徑在送出前就先 fault
- 下一輪如果這個 fault 消失，才有資格繼續判斷：
  - DHCP 是否真的送出
  - 若有送出但沒回包，再回頭看 PHY/duplex/收包

### 最新 build

- `Program Size: Code=107142 RO-data=14066 RW-data=15940 ZI-data=33452`
- `0 Error(s), 0 Warning(s)`

## 下一步

1. 燒錄最新韌體
2. 觀察 `netif cb up a/b/c/d`
3. 若看到 `dhcp start`，再看是否出現：
   - `[net] tx ok ...`
4. 同時用抓包確認 DHCP Discover 是否真的上線
## 2026-05-06 接手摘要：DHCP packed header fault

### 這次已確認的真根因

- 目前這條線不是單一 DHCP 邏輯錯誤，而是多個 lwIP packed header 在 `armclang + Cortex-M7` 下的未對齊 32-bit 存取
- 已確認並修掉的 fault 類型：
  - `low_level_output()` 內對 TX frame 緩衝區做非必要解析時的未對齊讀取
  - `igmp_input()` 對 packed `igmp_group_address` 的直接讀取
  - `dhcp_recv()` 對 packed `reply_msg->xid` 的直接讀取

### 這一輪實際修改

- [dhcp.c](/C:/Users/Public/Code/stm32/f767/autoControl/Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c)
  - 新增 bytewise safe helper
  - 改掉：
    - `reply_msg->xid`
    - `msg_in->yiaddr`
    - `msg_in->siaddr`
    - `msg_out->xid`
    - `msg_out->ciaddr`
    - `msg_out->cookie`
- 最新 build：
  - `Program Size: Code=107614 RO-data=14046 RW-data=15940 ZI-data=33452`
  - `0 Error(s), 0 Warning(s)`

### 使用者下一步要測什麼

1. 燒錄最新韌體
2. 確認 `PC=0x08007974` 的 HardFault 不再出現
3. 觀察 DHCP 是否能從 `selecting` 前進到 `checking` 或 `bound`
4. 若還有新的 `CFSR=0x01000000`，保留完整：
   - `PC`
   - `LR`
   - `CFSR`

### 下一步工作建議

- 如果這版後 DHCP 能前進：
  - 開始收斂多餘網路 log
  - 再回頭做 HTTP / discovery / control 回歸
- 如果又跳新的 unaligned fault：
  - 直接用 `PC/LR` 對回下一個 lwIP packed header 路徑
  - 不再回到碎片化的逐行 log 追查
## 2026-05-06 接手補充：ETHARP packed header fault

- DHCP 目前已經能走到 `bound`
- 新的真 fault：
  - `PC=0x0800992E`
  - `CFSR=0x01000000`
- 已對回 [etharp.c](/C:/Users/Public/Code/stm32/f767/autoControl/Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c) 的 `etharp_output()`
- 問題點是 `LWIP_AUTOIP` 分支直接讀 packed `ip_hdr.src`
- 已修正成：
  - bytewise 複製 packed `src`
  - 再用 aligned `ip4_addr_t` 做 `ip4_addr_islinklocal()`
- 最新 build：
  - `0 Error(s), 0 Warning(s)`
# mem

## 2026-05-06 接手摘要

- 當前實機狀態：
  - DHCP 已可 `bound`
  - `ping` 正常
  - TCP control `PING` 正常
  - Web/HTTP 仍未確認啟動成功
- 本輪已將大量暫時 debug log 收斂，只保留：
  - `boot`
  - `link up/down`
  - `dhcp ...`
  - `http up/err`
  - `fault`
- HTTP 啟動路徑已改：
  - [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c)
  - `tcpip_try_callback()` -> `tcpip_callback()`
  - 新增最小狀態：
    - `[http] up 80`
    - `[http] pcb err`
    - `[http] bind err`
    - `[http] listen err`
    - `[http] cb err=...`
- 最新 build：
  - `Program Size: Code=107502 RO-data=13482 RW-data=15940 ZI-data=50852`
  - `0 Error(s), 0 Warning(s)`

- 另已修正：
  - [Core/Inc/FreeRTOSConfig.h](/C:/Users/Public/Code/stm32/f767/autoControl/Core/Inc/FreeRTOSConfig.h)
    - `configTOTAL_HEAP_SIZE` 回升到 `32768`
  - [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c)
    - HTTP task stack `384 -> 256`
  - [App/Service/discovery_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/discovery_service.c)
    - `tcpip_try_callback()` -> `tcpip_callback()`
# 2026-05-06 Handoff
- Latest stability fix unifies service maintenance into `StartDefaultTask()`:
  - `control_service_poll();`
  - `http_service_poll();`
- `control_service` no longer spawns its own periodic polling thread.
- Both poll functions now use `tcpip_callback()` instead of `tcpip_try_callback()`.
- Purpose: stop silent listener starvation that can kill ping/TCP/UDP/HTTP after sustained use.
- Added `/api/status` runtime diagnostics:
  - `rtos.heap_free`, `rtos.heap_min`
  - `http.active`, `http.close_pending`, `http.listener_up`
  - `control.listener_up`
  - `lwip.mem_*`, `lwip.tcp_pcb_*`, `lwip.tcp_seg_*`, `lwip.pbuf_*`
- Latest direct fixes:
  - lwIP pool sizes increased to address repeated `tcp=5/5` saturation and `mem.err` growth
  - semihosting disabled and lwIP assert output rerouted to UART to stop `_sys_write/BKPT` HardFaults

# 2026-05-06 Latest Handoff
- New field evidence:
  - repeated `[lwip] assert "pbuf_free: p->ref > 0"`
  - then `mem_free: illegal memory: non-linked: double free`
- This is now the clearest root cause for the current "web won't load" stage.
- Fixed in [LWIP/Target/ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c):
  - `low_level_output()` now calls `pbuf_ref(p)` before handing the TX packet to HAL
  - on `HAL_ETH_Transmit()` failure it immediately calls `pbuf_free(p)` to undo the extra ref
- Rationale:
  - HAL frees the TX pbuf later through `HAL_ETH_ReleaseTxPacket()` -> `HAL_ETH_TxFreeCallback()`
  - without the extra ref, that later free races with lwIP's own ownership and causes double-free asserts
- Latest build:
  - `0 Error(s), 0 Warning(s)`
