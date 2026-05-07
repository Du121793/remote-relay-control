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

# 2026-05-06 Cluster Settings UI/API
- User explicitly wants a web page for cluster settings; status JSON alone is not enough.
- User also explicitly wants `mem.md` updated from this conversation onward on every meaningful step.
- Implemented:
  - `GET /api/config/cluster`
  - `POST /api/config/cluster`
  - `GET /api/cluster/nodes`
- `Web/index.html` now includes a cluster settings panel for:
  - device name
  - cluster name
  - cluster password
  - cluster enabled
  - auto join
- Added `cluster_service_reload_from_config()` and made it clear the node table on config reload so stale nodes from a previous cluster do not linger.
- `cluster_password_hash` is still only a placeholder CRC32-repeat hash; this is config scaffolding, not final security.
- Could not run Keil rebuild locally in the current environment because `UV4.exe` was not available on PATH / common install path; next validation point is the user's actual rebuild result.

# 2026-05-06 web_assets Root Cause
- User reported reflashed firmware still showed the old web UI with no cluster settings panel.
- Verified source files were correct:
  - `Web/index.html` already contained the cluster settings panel and JS
  - `http_service.c` already exposed `/api/config/cluster` and `/api/cluster/nodes`
- Root cause: `Web/web_assets.h` had not actually been regenerated and still embedded the old HTML.
- Confirmed by decoding `web_assets.h`:
  - old asset did not contain `saveClusterConfig`, `clusterStatus`, or `/api/config/cluster`
- Regenerated `Web/web_assets.h` directly from the current `Web/index.html`.
- Next required step is user-side rebuild + reflash with the regenerated asset.

# 2026-05-06 Cluster Save / Relay Visibility Follow-up
- User reported:
  - cluster config page can read current config
  - saving cluster config returned `{"ok":false,"error":"config_save_failed"}`
  - after entering a password and saving, two boards could both become unresponsive
  - other cluster relay states were still not visible
- Likely root cause for save failure / hang:
  - config had been temporarily moved to `Bank1/Sector7`
  - erasing/programming the executing bank is unsafe and can stall/crash the MCU
- Applied follow-up changes:
  - moved config storage back to `Bank2` config sector
  - HTTP save failure now returns `flash_error`
  - discovery announce now includes `relay_mask` and `auth_tag`
  - discovery receive path filters by `auth_tag`
  - `/api/cluster/nodes` now returns `ip_text` + `relay_mask`
  - web node panel now formats each node as `name | role | ip | relay=0x...`
- Next validation point:
  - user-side rebuild/reflash
  - confirm `config_save_failed` disappears
  - confirm cluster node panel shows remote relay masks

# 2026-05-07 CLI Build Validation
- User provided direct Keil CLI path:
  - `C:\Users\a5522\AppData\Local\Keil_v5\UV4\UV4.exe`
- Build failure root cause this round:
  - `discovery_service.c` used `board_relay_snapshot()` without including `board_relay.h`
- Fixed:
  - added `#include "board_relay.h"` to `App/Service/discovery_service.c`
- Validation:
  - ran Keil CLI build with `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - command returned exit code `0`

# 2026-05-07 Config Save Failure Follow-up
- User reported cluster config still could not be saved even after moving config storage back to `0x081E0000`.
- Root cause is likely flash bank mode mismatch:
  - `0x081E0000` is valid in both layouts
  - but erase parameters differ:
    - single-bank mode: `Bank1 / Sector11`
    - dual-bank mode: `Bank2 / Sector23`
- Updated `device_config.c`:
  - runtime-detect `FLASH_OPTCR_nDBANK`
  - choose erase bank/sector dynamically
- Revalidated with Keil CLI build:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 flash_error=4 + cluster web rework
- User provided exact save failure:
  - `{"ok":false,"error":"config_save_failed","flash_error":4}`
- On STM32F7 HAL, `4` corresponds to `HAL_FLASH_ERROR_PGP`; this points to the program mode itself, not generic erase failure.
- Fixed in `App/Config/device_config.c`:
  - changed config writes from `FLASH_TYPEPROGRAM_DOUBLEWORD` to `FLASH_TYPEPROGRAM_WORD`
- Widened cluster node model:
  - `cluster_name`
  - `auth_tag`
  so the system can show foreign clusters while still limiting election/membership to same-cluster + same-auth nodes.
- Added remote relay control API:
  - `POST /api/cluster/node-relay`
  - implemented through `control_queue` worker using outbound TCP control sockets
  - avoids running blocking remote control traffic directly inside the HTTP/lwIP callback path
- Reworked `Web/index.html`:
  - login-first view
  - top navigation tabs
  - cluster node cards
  - same-cluster nodes show remote relay toggles
  - foreign-cluster nodes are visible but explicitly non-controllable
- Regenerated `Web/web_assets.h` from the updated HTML.
- Revalidated by Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 save request hang follow-up
- User reported two remaining symptoms:
  - page still looked old after flashing
  - cluster save request was sent but took a very long time / did not return promptly
- Follow-up fixes:
  - moved config persistence out of the HTTP/lwIP callback path into a dedicated FreeRTOS worker queue in `device_config.c`
  - `device_config_update_and_save()` now submits to the config worker and waits for completion
  - direct flash programming is still used internally during early init / fallback paths
- This specifically targets the case where flash erase/program inside the HTTP request path stalls response handling.
- Keil CLI revalidation after the worker change:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 stale embedded web asset root cause
- User screenshot still showed the old single-page UI after rebuild/flash.
- Verified by decoding `Web/web_assets.h`:
  - the embedded HTML was still an older version
  - it did not match the latest `Web/index.html`
- Regenerated `Web/web_assets.h` again from the current `Web/index.html`.
- Re-decoded the generated asset and confirmed it now contains:
  - `loginView`
  - `tab-relay`
  - `clusterNodeCards`
- Revalidated by Keil CLI after regenerating the embedded asset:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 cluster follow-up after new UI was confirmed live
- User confirmed the page is now the new UI and asked to ignore the previously deleted source issue; focus shifted to functional gaps:
  - remote relay control returned `remote_command_failed` even when the side effect succeeded
  - different passwords still looked "joined" from the UI point of view
  - members should redirect to the master and only the master should accept control/config writes
  - local and remote same-cluster relays should both be on the main page
  - relay names should be editable
  - multiple browser sessions should explicitly show a single-controller warning
- Work completed in this step:
  - rebuilt `Web/index.html` from scratch as the source-of-truth page
  - regenerated `Web/web_assets.h` from that rebuilt HTML
  - added master-awareness to `http_service.c`
    - cluster config now exposes `master_ip` and `is_local_master`
    - member nodes now receive `not_master` on write APIs
  - added relay config APIs:
    - `GET /api/config/relays`
    - `POST /api/config/relay`
  - added `POST /api/cluster/kick`
  - added `cluster_service_remove_node()` for local master-side member removal from the visible cluster view
  - relaxed `control_queue` remote command handling so a successful remote `SET` is not falsely reported as a failure just because the immediate response path is incomplete
  - added a follow-up `GET RELAY` refresh attempt after remote `SET`
- Keil CLI validation used for this step:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 follow-up for empty tab content on the new UI
- User reported:
  - the shell/tabs were visible
  - but each tab still looked empty
  - member nodes should not control even local relays and should have a redirect button to master
- Response in this step:
  - rebuilt `Web/index.html` again as an ASCII-safe source page to eliminate lingering encoding/rendering instability
  - added explicit `Go To Master` button
  - kept home page responsibility as:
    - local relay controls
    - same-cluster remote relay controls
  - preserved member-side write rejection through `not_master`
  - regenerated `Web/web_assets.h`
- Revalidated again by Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 pending HTTP requests root cause for relay/node JSON
- User reported the same symptom again:
  - tabs were present
  - but content still looked empty
  - browser requests remained pending
- Root cause identified in `http_service.c`:
  - `GET /api/config/relays` and `GET /api/cluster/nodes` had been switched to the streaming send path
  - but both responses were built in the local stack buffer `response_body` inside `http_service_handle_request()`
  - the streaming path stored a pointer to that local buffer and returned
  - after the function returned, the send pointer referenced invalid memory
  - this explains pending requests / empty sections / unstable response bodies
- Fix:
  - switched both dynamic JSON endpoints back to `http_service_send_payload()`
  - this path uses `TCP_WRITE_FLAG_COPY`, so the payload is copied into lwIP buffers before the local stack buffer goes out of scope
  - kept `HTTP_BODY_BUFFER_SIZE = 4096` so the relay config JSON still fits
- Revalidated by Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 empty tab content root cause narrowed to relay config response size
- I directly queried the live board at `192.168.0.245`.
- Findings:
  - `/api/config/cluster` returned valid JSON and showed the board is a member with master `192.168.0.157`
  - `/api/cluster/nodes` returned a valid same-cluster master node
  - `/api/config/relays` returned HTTP 200 headers with `Content-Length: 1000` but effectively no usable body in the client path
- This explains why the shell/tabs were visible but the content areas looked empty:
  - the relay config request was failing to deliver a complete body
  - the front-end then rendered no local relay cards and no relay settings table
- Fixes applied:
  - increased `HTTP_BODY_BUFFER_SIZE` from `1024` to `4096`
  - changed `GET /api/config/relays` and `GET /api/cluster/nodes` to use streaming HTTP payload sending instead of the short-payload helper
  - rebuilt `Web/index.html` again and restored Chinese UI text
  - retained explicit member-side `Go To Master` button behavior
- Revalidated again by Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 前端 pending 疑似 session/輪詢重疊
- 使用者指出：新版頁面殼存在，但每個分頁內容仍空白，且瀏覽器 network 長時間 pending，懷疑和 session 機制有關。
- 檢查 `Web/index.html` 後確認：
  - `refreshAll()` 每 3 秒由 `setInterval` 無條件觸發
  - 多個按鈕操作也會 `await refreshAll()`
  - 沒有任何 in-flight guard，請求很容易重疊堆積
- 已修改前端：
  - `state.refreshInFlight`
  - `state.refreshPending`
  - `apiFetch()` 加入 4 秒 timeout / abort
  - 自動輪詢改為 5 秒，且 `document.hidden` 時不主動刷
- 已重新生成 `Web/web_assets.h`
- 待驗證：新版韌體燒入後，`/api/config/relays`、`/api/cluster/nodes` 是否不再 pending，主畫面與設定頁內容是否正常顯示。

# 2026-05-07 後端 tcpip callback 維護節流
- 直接本機 curl 測試 `192.168.0.245` 與 `192.168.0.157` 的 `/`、`/api/session`、`/api/config/relays`、`/api/cluster/nodes`，全部 6 秒 timeout。
- 這表示問題已不是單純前端畫面，而是 HTTP listener / tcpip thread 在實機上又進入不回應狀態。
- 已先修前端：
  - refresh 鎖
  - pending 合併
  - 4 秒 fetch timeout
  - 5 秒輪詢
- 進一步懷疑 `defaultTask` 每 100ms 呼叫 `http_service_poll()` / `control_service_poll()`，持續用 `tcpip_callback()` 向 lwIP thread 塞維護工作，負載下會擠壓真正 TCP 處理。
- 已先在 `http_service.c` 加入 `g_http_poll_pending` 節流，避免重複排入 callback。
- 下一步正在對 `control_service.c` 做同樣節流。

# 2026-05-07 HTTP 全面 timeout 與 poll 節流
- 直接本機 `curl` 測 `192.168.0.245`、`192.168.0.157` 的 `/`、`/api/session`、`/api/config/relays`、`/api/cluster/nodes`，全部在 6 秒內 connection timeout。
- 這表示當前問題已不是單純前端 DOM，而是板子上的 HTTP listener / tcpip thread 實際上進入不回應狀態。
- 進一步檢查發現：
  - `Web/index.html` 原本雖已加 `refreshInFlight`，但仍用固定輪詢；已改成 `setTimeout` 串行輪詢與 4 秒 fetch timeout。
  - `Core/Src/main.c` 每 100ms 呼叫 `http_service_poll()` / `control_service_poll()`。
  - 兩者都直接 `tcpip_callback(...)`，沒有任何 pending 節流，負載下可能持續把 maintenance callback 塞進 lwIP thread，擠壓真正的 TCP 處理。
- 已修改：
  - `Web/index.html`：串行 refresh、pending 合併、5 秒輪詢、頁面 hidden 不輪詢。
  - `App/Service/http_service.c`：新增 `g_http_poll_pending`。
  - `App/Service/control_service.c`：新增 `g_control_poll_pending`。
- 已重新生成 `Web/web_assets.h` 並用 Keil CLI build 驗證成功。

# 2026-05-07 HTTP 診斷 log 強化
- 使用者要求直接加 log 測試，不再猜測。
- 已在 `App/Service/http_service.c` 加入最小必要 log：
  - `[http] listener down`
  - `[http] accept active=...`
  - `[http] accept full`
  - `[http] close ok active=...`
  - `[http] close defer err=...`
  - `[http] err=... active=...`
  - `[http] poll skip=...`
  - `[http] poll err=...`
- 目的：判斷 HTTP 整體 timeout 時，是 listener 被關掉、連線數吃滿不回收、還是 poll callback 堆積。

# 2026-05-07 HTTP request/send 路徑直接加 log
- 根據目前 log：
  - 有 `accept`
  - 多數也有 `close ok`
  - 沒有 `accept full` / `close defer` / `poll skip`
- 這表示前一輪懷疑的 connection pool 卡死與 callback 堆積，不是目前最直接的主因。
- 比對 `81c776a` 後，最大的風險點是：
  - 新頁面 HTML 約 25KB
  - 新增 `/api/config/cluster`、`/api/config/relays`、`/api/cluster/nodes` 等較大 JSON
  - 但 `http_service_recv()` 仍直接忽略 `http_service_handle_request()` 的回傳值
- 已修改 `http_service.c`：
  - `recv()` 現在會印 request first line：`[http] req ...`
  - 若 `http_service_handle_request()` 回傳非 `ERR_OK`，會印：`[http] send err=...`
  - 並立即關閉該連線，避免 request 靜默卡住
- 這一版能直接分辨：
  - request 根本沒進來
  - 或 request 進來了，但 send/tcp_write/tcp_output 失敗

## 2026-05-07 / api-config-relays pending
- UART log confirmed GET /api/config/relays enters http_service_recv but never closes.
- Tightened JSON formatting for relay names, cluster names, and node names to bounded-length + escaped output.
- Added [http] relays bytes=... log to measure actual payload size before send.
- Keil CLI rebuild passed after patch.


## 2026-05-07 / HTTP pending root cause narrowed to lwIP thread stack
- User UART showed GET /, session, login, cluster all close cleanly, but GET /api/config/relays logs relays bytes and then stalls with no close.
- Checked lwipopts.h: TCPIP_THREAD_STACKSIZE is only 1024 bytes.
- http_service_recv + http_service_handle_request had request[768] + response_body[4096] on stack in lwIP tcpip thread, which is enough to corrupt/stall large API paths without immediate fault.
- Moved HTTP request/response buffers to static storage in http_service.c and rebuilt with Keil CLI successfully.


- Follow-up fix: after moving response_body to a static buffer, handle_request still used sizeof(response_body), which became pointer-size (4) and truncated many JSON/error paths. Replaced those calls with HTTP_BODY_BUFFER_SIZE-backed response_body_size.


- Follow-up fix: request buffer was also moved to a pointer-backed static buffer, but http_service_recv still used sizeof(request), so only 'GET' was copied and all requests became bad_request. Switched copy_len to HTTP_REQUEST_BUFFER_SIZE.


- Live curl to 192.168.0.245 proved /api/config/relays returns HTTP 200 with Content-Length 1332 but zero body bytes. Root cause likely send-buffer sizing: current TCP_SND_BUF was only 1072 (2*MSS), smaller than relay JSON payload. Raised TCP_SND_BUF to 4*MSS and updated TCP_SND_QUEUELEN/SNDLOWAT/SNDQUEUELOWAT accordingly.

# 2026-05-07 cluster / UI live test and follow-up fixes
- Ran live curl end-to-end against both boards:
  - `192.168.0.245`
  - `192.168.0.157`
- Verified on live firmware:
  - `GET /` works
  - `POST /api/login` issues a new token
  - old token is rejected by `/api/session` after a new login, so the single-controller token model already works at backend level
  - `GET /api/config/relays` now returns full JSON body
  - local relay rename and local relay toggle both work on the master
- Live issues confirmed before this round of fixes:
  - relay default names were wrong for channels 10..16 (`Relay19`..`Relay25`)
  - saving the same password to both nodes left them as two standalone masters with empty node tables
  - changing password to empty string did not clear `cluster_password_hash`
  - `kick` only removed a node from local view; it reappeared on the next announce
  - remote relay control had been failing with short `CONTROL_QUEUE_WAIT_TIMEOUT_MS=100`
  - frontend refresh path was still too heavy because it fetched `cluster + relays + relay state + nodes + status` every cycle
- Fixes applied in this round:
  - `App/Config/device_config.c`
    - fixed default relay naming using `snprintf("Relay%u", channel)`
  - `App/Service/control_queue.c`
    - raised `CONTROL_QUEUE_WAIT_TIMEOUT_MS` from `100` to `1000`
  - `App/Service/discovery_service.h/.c`
    - added `discovery_service_force_announce()`
    - lowered announce interval from `5000ms` to `2000ms`
    - update `cluster_service` with local load score before announce
  - `App/Service/cluster_service.c/.h`
    - added runtime banned node list
    - added `cluster_service_kick_node()`
    - `cluster_service_note_remote_node()` now ignores banned nodes
    - election logic changed to deterministic master selection using:
      - master term
      - load score
      - node_id tie-break
    - added tracked `local_load_score`
    - lowered stale timeout from `15000ms` to `6000ms`
  - `App/Service/http_service.c`
    - empty `cluster_password` now clears the stored hash
    - successful cluster config save now calls:
      - `cluster_service_reload_from_config()`
      - `discovery_service_force_announce()`
    - `kick` now uses `cluster_service_kick_node()`
  - `Web/index.html`
    - reduced periodic refresh weight:
      - `relayConfig` only on first load / settings / forced refresh
      - `status` only on status tab / forced refresh
      - home/cluster only fetch what they need
    - relay controls changed to switch-style toggles
    - settings/relay saves reset `relayConfigLoaded` so the next forced refresh reloads configuration
  - regenerated `Web/web_assets.h` from the updated HTML
- Revalidated current source with Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 quick compile cleanup after cluster/ui round
- User rebuild reported two pure compile-breaks unrelated to runtime logic:
  - stray `1` left in `Core/Src/main.c` under `USER CODE BEGIN SysInit`
  - `App/Config/device_config.c` used `snprintf` without including `<stdio.h>`
- Fixed both with minimal edits only:
  - removed the stray `1`
  - added `#include <stdio.h>`
- Revalidated again with Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 live cluster retest blocker found
- Ran live curl after the previous cluster/password fixes and found both boards still ended up as separate masters with `nodes=[]`.
- Checked the current source path in `StartDefaultTask()` and found a critical omission:
  - `control_service_poll();`
  - `cluster_service_poll();`
  - `http_service_poll();`
  - but **`discovery_service_poll();` was missing**
- This omission explains why the flashed firmware could not meaningfully reform clusters after password changes:
  - discovery announce task/UDP path may never initialize
  - same-password boards stay split
  - `kick` and auth-mismatch behavior cannot be validated because there is no same-cluster node view
- Fixed by adding `discovery_service_poll();` back into `StartDefaultTask()`.
- Revalidated again with Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 live retest after reflash: discovery asymmetry isolated
- User reflashed both boards and requested full live retest.
- Live curl results after reflash:
  - `192.168.0.157`
    - `/api/config/cluster`: `role=MASTER`, `master_ip=192.168.0.157`
    - `/api/status`: `discovery.rx=0`, `discovery.announce=141`
    - `/api/cluster/nodes`: empty
  - `192.168.0.245`
    - `/api/config/cluster`: `role=MEMBER`, `master_ip=192.168.0.157`
    - `/api/status`: `discovery.rx=141`, `discovery.announce=0`
    - `/api/cluster/nodes`: contains node `157` with `same_cluster=1`, `controllable=1`, `auth_match=1`
- Confirmed requirements/behavior:
  - member cannot control even its own local relay:
    - `POST 245 /api/relay-channel` returns `{"ok":false,"error":"not_master","master_ip":"192.168.0.157"}`
  - old token invalidation had already been verified earlier and still stands.
- Main remaining blocker is no longer generic cluster logic:
  - discovery behavior is asymmetric
  - `157` only transmits announce and never receives
  - `245` only receives announce and never records successful transmit
- Added diagnostic fields to `discovery_service_stats_t` and `/api/status`:
  - `tx_attempts`
  - `tx_failures`
  - `last_tx_err`
  - `last_payload_len`
- Purpose of this patch:
  - after next flash, determine whether `245` fails before send (payload/build path) or at `udp_sendto()` itself
- Revalidated current source with Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 live discovery root cause confirmed
- After adding discovery TX diagnostics and retesting live:
  - `157 /api/status`
    - `discovery.rx=0`
    - `tx_attempts=32`
    - `announce=31`
    - `tx_failures=1`
    - `last_tx_err=0`
    - `last_payload_len=191`
  - `245 /api/status`
    - `discovery.rx=32`
    - `tx_attempts=0`
    - `announce=0`
    - `tx_failures=34`
    - `last_tx_err=-2`
    - `last_payload_len=192`
- This proved the asymmetry was not generic UDP breakage:
  - `245` was failing locally before actual send
  - reason: `discovery_service_udp_send_announce()` used `char payload[192]`
  - `discovery_proto_build_announce()` on `245` produced exactly `192` bytes
  - local guard `payload_len >= sizeof(payload)` rejected it
  - result: member never announced successfully, master never learned about member
- Fix applied:
  - introduced `DISCOVERY_ANNOUNCE_BUFFER_SIZE = 256`
  - changed local announce buffer to `payload[DISCOVERY_ANNOUNCE_BUFFER_SIZE]`
- Revalidated current source with Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 full live cluster workflow retest
- After reflashing the discovery-buffer fix, live retest against `157` and `245` confirmed:
  - same-password regroup works
  - different-password split works
  - after password split:
    - `157` becomes local master with `term=2`
    - `245` becomes local master with `term=2`
    - `245 /api/cluster/nodes` still lists `157` but with:
      - `same_cluster=0`
      - `auth_match=0`
  - changing `245` to the same password again makes it rejoin `157` within a few seconds
  - member cannot control even its own relay:
    - `POST 245 /api/relay-channel` returns `not_master`
  - runtime kick works one-way:
    - `157` removes and bans `245`
    - `245` still thinks `157` is master until config/election changes
- Remaining core failure after the above retest:
  - `157 -> POST /api/cluster/node-relay` still returned `remote_command_failed`
  - direct raw TCP test from PC to `245:41000` using `SET RELAY 1 1` returned:
    - `OK type=relay relay=0x0001 ...`
  - therefore the control port on the member is healthy
  - the failure is in the master-side remote execution path
- Root cause identified:
  - HTTP request handling runs inside lwIP tcpip thread
  - `http_service_apply_remote_command()` called synchronous `control_queue_submit_remote()`
  - `control_queue_submit_remote()` blocks waiting for a worker result
  - worker then uses lwIP socket/connect/send/recv to reach the remote node
  - this creates a structural deadlock/fake-failure risk because the tcpip thread is blocked waiting while the worker depends on lwIP networking progress
- Fix applied:
  - added `control_queue_submit_remote_async(uint32_t ip, const control_proto_command_t *cmd)`
  - worker now supports remote requests with `result == NULL`
  - `http_service_apply_remote_command()` now enqueues remote control asynchronously and returns `{ \"ok\": true, \"queued\": 1 }`
  - front-end delayed refresh was attempted but not yet patched because of the current source file's encoding/noisy text; backend is fixed first
- Revalidated current source with Keil CLI:
  - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

# 2026-05-07 live retest after async remote-control change
- Reflashed firmware was retested live on both boards.
- Fresh login tokens used:
  - `157`: `2999035032`
  - `245`: `2897566515`
- Verified current cluster status:
  - `157` reports `role=MASTER`, `is_local_master=1`
  - `245` reports `role=MEMBER`
- Verified local control behavior:
  - `157 /api/relay-channel` succeeds (master can control local relay)
  - `245 /api/relay-channel` still returns `not_master` (member cannot control even its own relay)
- Verified cluster membership:
  - `157 /api/cluster/nodes` lists `245`
  - `245 /api/cluster/nodes` lists `157`
- Verified asynchronous remote relay after backend change:
  - `157 /api/cluster/node-relay` now returns `{"ok":true,"queued":1}`
  - after ~2s, `245 /api/relay` reflects the new relay state
- Conclusion:
  - remote relay control is now functionally working
  - remaining issues are no longer cluster/network-core failures
  - next work should focus on:
    - unifying all write operations into one queue
    - improving frontend session/master-state synchronization
    - upgrading `kick` from one-way runtime ban to protocol-level eviction

# 2026-05-07 control-queue unification and Chinese UI rewrite
- User requested:
  - keep the "single control queue" direction
  - do not use the earlier short-delay retry idea for remote control yet
  - rewrite the web UI in Chinese
  - reduce polling weight and add pending/disable behavior on control buttons
- Backend change:
  - `http_service_apply_command()` now uses `control_queue_submit(...)`
  - `control_service_execute()` now routes:
    - `SET_RELAY`
    - `SET_RELAY_MASK`
    - `SET_RELAY_ALL`
    through the same control queue
  - this keeps local and remote write operations on the same serialized path
- Frontend change:
  - replaced the previous corrupted/mojibake `Web/index.html`
  - new page is Chinese and split into:
    - 主畫面
    - 叢集與節點
    - 設定
    - 系統狀態
  - 本機 relay and same-cluster remote relay are both on 主畫面
  - member nodes see state but cannot control, and get a "前往 Master" button
  - relay controls now use switch-style buttons
  - local/remote control buttons enter pending/disabled state until refreshed state matches
  - polling is reduced to a single 4s loop with per-tab selective fetches
- Asset packaging:
  - `Web/web_assets.h` regenerated from the rewritten `Web/index.html`
- Validation:
  - Keil CLI build re-run after UI rewrite:
    - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
    - exit code `0`
- Next step after user reflashes:
  - verify the new UI is actually embedded and visible
  - live-test all tabs, master-only control behavior, local queue-backed control, and remote queue-backed control

# 2026-05-07 live latency measurement and polling reduction
- User feedback after the rewritten Chinese UI:
  - web controls still felt very laggy
  - repeated clicking could still trigger errors
  - the UI did not visibly switch into a pressed/pending state as desired
- Live HTTP timing checks against both boards showed:
  - `157 /api/config/cluster` about `79ms`
  - `157 /api/config/relays` about `355ms`
  - `157 /api/cluster/nodes` about `1067ms`
  - `157 /api/relay` about `121ms`
  - `245 /api/config/cluster` about `81ms`
  - `245 /api/config/relays` about `127ms`
  - `245 /api/cluster/nodes` about `127ms`
  - `245 /api/relay` about `57ms`
- Conclusion:
  - the major user-perceived lag is not a broken API anymore
  - it is primarily caused by:
    - over-frequent full refresh behaviour
    - `/api/cluster/nodes` being significantly slower than the simple relay APIs, especially on the member
    - UI waiting for the next poll to reflect control changes
- Additional live verification:
  - both boards are serving the new HTML asset
  - HTML markers verified from `/`:
    - `主畫面`
    - `同叢集其他節點`
    - `前往 Master`
    - `等待節點狀態回報`
- Frontend changes applied:
  - polling interval reduced from one heavy 4s cycle to a lighter 1.5s tick scheduler
  - per-resource polling cadence:
    - relay state: high frequency on `主畫面`
    - node list: lower frequency
    - session / cluster config: lowest frequency
    - no auto polling on `設定`
  - local relay:
    - immediately mutates displayed `relay_mask`
    - marks the control pending
    - disables all local write buttons while a local write is pending
  - remote relay:
    - immediately mutates the displayed remote `relay_mask`
    - marks the node pending
    - disables all buttons for that node while any of its relay writes are pending
  - after successful remote enqueue, the UI no longer waits for a separate delayed refresh before changing the switch position
- Validation:
  - `web_assets.h` regenerated after the above changes
  - Keil CLI build passed:
    - `C:\\Users\\a5522\\AppData\\Local\\Keil_v5\\UV4\\UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`

## 2026-05-07 前端首頁重構與輪詢降載
- 使用者明確要求不要再區分本機 / 叢集兩大區塊，而是以「設備名稱（本地 / 同叢集）」為主，下面直接列該設備可用繼電器。
- 重新整份改寫 `Web/index.html`：
  - `主畫面` 改成單一 `deviceList`
  - 每個設備卡片顯示名稱、角色、IP 與 16 路繼電器
  - 本機與遠端都改用同一種開關卡片樣式
- 輪詢策略同步降載：
  - `session / clusterConfig` 降頻
  - `relay` 只在主畫面高頻抓
  - `nodes` 主畫面降頻、叢集頁較高頻
  - `設定` 頁不再自動抓 relay config
- 本機與遠端控制保留 pending 鎖定，但不加入短延遲重試，等待後端狀態同步解除。
- 重新生成 `Web/web_assets.h` 並用 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 驗證編譯通過。
## 2026-05-07 首頁單一設備清單與 enabled_mask
- 使用者要求主畫面不要再分成本機 / 叢集兩塊，而是依設備名稱（本地 / 同叢集）一張卡一張卡排列。
- 後端補上 discovery announce 的 `enabled_mask`：
  - `cluster_node_t` 新增 `enabled_mask`
  - `discovery_proto_build_announce()` / `parse_announce()` 帶 `enabled=...`
  - `discovery_service` 由本地 `device_config.relay[].enabled` 組出位元遮罩
  - `/api/cluster/nodes` 現在回傳 `enabled_mask`
- 前端 `Web/index.html` 整份重寫：
  - 主畫面改成單一 `deviceList`
  - 未啟用 relay 直接隱藏
  - 每顆按鈕單獨 pending，不再整台卡一起凍結
  - 每顆 relay 旁新增紅色「停」按鈕做緊急停止
- `Web/web_assets.h` 已重新生成，並用 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 驗證編譯通過。
- 遠端 relay 名稱同步 / 修改尚未做，因 discovery 協定目前只帶 mask 與 enabled 狀態，尚未帶名稱資料。
## 2026-05-07 編譯修正：main.c 殘留字元
- `Core/Src/main.c` 的 `main()` 開頭殘留了一個裸 `0`，造成 `expected ';' after expression`。
- 已移除，並用 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 驗證編譯恢復成功。
## 2026-05-07 本地叢集設定權限與緊急停止按鈕
- 使用者澄清：本地裝置一定可以修改自己的叢集加入資訊，即使目前是 member，也要能主動退出、改密碼、改叢集名稱。
- 因此移除 `http_service.c` 中對：
  - `POST /api/config/cluster`
  - `POST /api/config/relay`
  的 `not_master` 限制，改為只要求登入即可。
- 前端 `Web/index.html` 再次整份重寫：
  - 設定頁新增「偵測到的叢集」下拉選單與套用按鈕
  - kick UI 移除
  - 主畫面每顆 relay 的一般開關與紅色「停」按鈕分離
  - 一般開關 pending 時只鎖自己
  - 緊急停止按鈕不跟著一般 pending 一起凍結
- 說明：
  - `remember_state` 的語意是：若啟用，裝置重開後恢復該路最後一次實際狀態；若停用，則走 `boot_default_on`。
## 2026-05-07 編譯修正：main.c 殘留 `{0`
- `Core/Src/main.c` 的 `main()` 開頭又被插入 `{0`，導致 `expected ';' after expression`。
- 已修正回正常的 `int main(void) {`。
- 重新用 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 驗證編譯成功。
## 2026-05-07 16:25 build gate

- User build error came from `main.c` around `main()` opening brace, but current workspace file is already clean:
  - `int main(void)`
  - `{`
- Verified by direct Keil CLI rebuild:
  - `C:\Users\a5522\AppData\Local\Keil_v5\UV4\UV4.exe -b MDK-ARM\autoControl.uvprojx -j0`
  - exit code `0`
- Current blocker is no longer compile; user needs to rebuild/download this exact workspace state before live testing can continue.

## 2026-05-07 17:05 remote latency + member relay settings

- Measured live before this round:
  - `157 -> 245` remote relay actual state change by API polling was about `69ms`
  - so backend relay execution itself was not the main latency source
- Main responsiveness issues were:
  - remote async queue path still waited for extra TCP response/cleanup inside worker
  - UI kept button pending until next `/api/cluster/nodes` refresh
  - no master-side way to edit member relay settings from the current page
- Changes made:
  - `control_queue.c`
    - queue length `8 -> 32`
    - async remote set path changed to fire-and-forget TCP send
    - async remote success now updates cached member `relay_mask` via `cluster_service_apply_remote_command()`
  - `cluster_service.[hc]`
    - added `cluster_service_apply_remote_command()`
  - `http_service.c`
    - added CORS headers on HTTP responses
    - added `OPTIONS` handling for browser preflight
    - updated async remote queue call to pass `node_id`
  - `Web/index.html`
    - fully rebuilt as clean UTF-8 Chinese UI
    - single device list on home
    - only clicked toggle freezes itself
    - red `停` remains usable
    - remote state applied immediately on queued success
    - settings tab now includes remote member relay config panels fetched directly from member HTTP APIs using CORS + per-node login token cache
  - regenerated `Web/web_assets.h`
- Keil CLI build after these changes:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`
- Live verification of this exact round still requires flashing the new image to `157/245`.

## 2026-05-07 17:20 GET / empty response root cause

- User reported `ERR_EMPTY_RESPONSE` on `192.168.0.245`.
- UART log showed repeated:
  - `[http] req GET / HTTP/1.1`
  - `[http] send err=-2`
- Root cause:
  - After adding CORS response headers, HTTP response header length for `/` became `266 bytes`
  - `http_service_send_payload_streaming()` and `http_service_send_payload()` still used `char header[256]`
  - local `snprintf()` length check therefore returned `ERR_BUF (-2)` before any body send
- Fix:
  - increased both local header buffers in `http_service.c` from `256` to `512`
- Rebuilt with Keil CLI:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

## 2026-05-07 17:35 remote relay pending state correction

- User reported current remote control behavior was faster, but unsafe:
  - button unfreezes before member relay actually changes
  - frequent clicks can leave UI showing `開` while actual member relay is `關`
- Root cause:
  - frontend `setRemoteRelay()` immediately mutated cached `node.relay_mask`
  - then immediately deleted `pendingRemote[key]`
  - this made the UI treat a queued command as already confirmed
- Fix in `Web/index.html`:
  - keep `pendingRemote[key]` after queued success
  - stop mutating cached `node.relay_mask` optimistically
  - trigger immediate `fetchNodes()` after queue success
  - let `syncPending()` clear pending only when the real node state matches the requested value
- Resulting behavior:
  - UI still flips to requested state through `effectiveMask()`
  - button remains frozen until actual node telemetry catches up
  - frequent clicking should no longer leave long-lived false visual states
- Regenerated `Web/web_assets.h`
- Rebuilt with Keil CLI:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

## 2026-05-07 18:05 live cluster validation on 157 / 245

- Current verified live topology after rejoin:
  - `192.168.0.157` = `autoControl2` = master
  - `192.168.0.245` = `autoControl` = member
- Verified behaviors:
  - member local relay control is rejected with `not_master`
  - master local relay control succeeds
  - master remote relay control to member succeeds
    - measured member state change latency around `394ms`
  - member local relay config save succeeds
  - master-side remote relay config capability works by directly writing member `/api/config/relay`
    - confirmed remote rename/write of member channel 3
  - session takeover works:
    - new login invalidates old token on same device
  - member local cluster config change works:
    - changing member password splits cluster
    - changing it back rejoins cluster after ~8s
- Cluster convergence observations:
  - split view immediately after password change is expected:
    - saving node reports temporary role `0`
    - then converges to standalone/master after announce/election window
  - rejoin also needs several seconds before `same_cluster=1`, `auth_match=1`
- Persistent config observations:
  - member returned relay config with channel 1 disabled and custom names applied
  - remote node `enabled_mask=65534` confirms disabled relay is exported to discovery
- Known remaining rough edges from live behavior:
  - remote control still has visible UX delay even though backend execution improved
  - measured backend change is sub-second, but UI perception will still depend on node refresh cadence
  - persisted old relay names `Relay19..Relay25` are still present on boards that saved older config before default-name fix

## 2026-05-07 18:35 remember-state / auto-join semantic fixes

- Fixed `auto_join=0` semantic regression:
  - previous code incorrectly treated `auto_join_cluster=0` as "do not participate in cluster"
  - corrected behavior:
    - if local cluster config is set and cluster is enabled, the node still remains in that cluster after manual join and reboot
    - `auto_join` is no longer used as a hard cluster-membership gate
- Added persistent relay runtime state:
  - `device_config_t` now includes `saved_relay_mask`
  - boot path applies `device_config_build_startup_relay_mask(device_config_get())`
  - startup mask is built from:
    - `relay.enabled`
    - `relay.remember_state`
    - `relay.boot_default_on`
    - persisted `saved_relay_mask`
- Removed synchronous flash writes from the hot local relay control path:
  - `control_queue` now updates runtime relay state through `device_config_runtime_relay_mask_set()`
  - actual flash persistence is deferred by `device_config_poll()`
  - delay currently set to `1500ms`
- Added consistency guard for deferred runtime save:
  - save path snapshots the current config
  - after save completes, dirty state is only cleared if `saved_relay_mask` still matches the saved snapshot
  - if another relay change happened during the save window, dirty remains set and save is rescheduled
- Fixed settings page checkbox alignment:
  - `啟用叢集` now uses dedicated `.checkline` layout in `Web/index.html`
- Keil CLI build after these changes:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

## 2026-05-07 18:52 final live test blocked by board availability

- User asked for a final 20-minute, 4-round live validation before commit
- I prepared and ran automated live-test scripts against:
  - `192.168.0.157`
  - `192.168.0.245`
- Initial PowerShell harness was invalid because expected `401 Unauthorized` from old session tokens was being treated as an unhandled exception
- Replaced that harness with direct `curl` checks and then a Python-based HTTP test runner
- Current blocker is not the harness anymore:
  - `192.168.0.157/api/login` returned connection refused during the Python live test
  - earlier checks also showed intermittent timeout / refusal behavior on both devices
- Because the requested end-to-end live test could not be completed successfully in the current device state, no commit should be created yet

## 2026-05-07 20:20 final 20-minute live validation passed

- Replaced the earlier brittle harness with direct LAN `curl` / PowerShell validation against:
  - `192.168.0.157`
  - `192.168.0.245`
- Final script executed 4 rounds with 5-minute gaps and all rounds passed
- Verified in each round:
  - `/` reachable on both boards
  - `/api/session` reachable
  - session takeover invalidates the old token and keeps the new token valid
  - `/api/config/cluster`, `/api/config/relays`, `/api/status`, `/api/cluster/nodes` all respond
  - master can control local relay
  - member local relay control is rejected with `not_master`
  - master can control member relay and the relay state converges back through cluster telemetry
  - local relay config save works
  - remote relay config save works
  - different password splits the cluster
  - same password rejoins the cluster
  - `auto_join_cluster=0` still preserves a manually joined cluster
- Observed round results:
  - Round 1: master `192.168.0.157`, remote convergence `1417ms`
  - Round 2: master `192.168.0.245`, remote convergence `2438ms`
  - Round 3: master `192.168.0.245`, remote convergence `1405ms`
  - Round 4: master `192.168.0.157`, remote convergence `3543ms`
- Final result:
  - `all_ok = true`
- This run is the basis for allowing the final commit
