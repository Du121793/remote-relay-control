# mem

## 2026-05-09 http-backpressure-debug branch summary

- This branch captures the unstable cloud/backend/http investigation after commit `41a16e8`.
- Main code changes on this branch:
  - delayed background task bring-up until link-up for `backend_link_service`, `discovery_service`, and `ClusterHealth`
  - enlarged several task stacks: `defaultTask`, `BackendLinkTask`, `CfgSave`, `DiscoveryService`, `ClusterHealth`
  - added backend runtime diagnostics: `should_run_reason`, periodic backend loop counters, send/recv diagnostics
  - changed HTTP request parsing to per-connection buffering with `Content-Length` accumulation
  - changed dynamic HTTP responses to per-connection response buffers instead of one shared global buffer
  - reduced backend cloud auto-provision side effects; backend now mostly observes instead of aggressively pushing config
  - added cloud registry / identity WIP files and backend tooling for future MQTT/TLS migration
  - added hard HTTP backpressure experiment with low connection pool and single-flight accept pause/resume

- Problems observed on this branch:
  - HTTP service can still stall under repeated browser/API access.
  - `GET /api/config/cluster` is the most reproducible trigger: request enters handler and prints `cfg len=...`, but close completion often never happens.
  - local frontend/browser access can still wedge the board even when backend heartbeat is otherwise stable.
  - cluster election is still not fully converged; duplicate-master condition is still possible.
  - cloud identity / provisioning work is incomplete and not production-safe yet.

- What was confirmed:
  - startup MemManage / HardFault regression was improved by delaying early task creation.
  - backend long link can stay alive much longer than earlier failing versions.
  - the newest instability is concentrated in MCU-side HTTP handling, not only frontend polling pressure.

- Recommendation:
  - do not merge this branch into `master` yet.
  - keep it as a WIP diagnostic branch for HTTP / cloud / cluster convergence work.
## 2026-05-06 ??唳??暺?
- HTTP ?暹?撌脤??啣?瘀??桀?銝?? `GET /` 擐? body ?芣嚗 listener 瘝帘摰遣蝡????舐汗?券?port 80 鋡急?蝯?銝?UART 瘝? `[http] up 80`??- 撌脫炎??[FINISH.md](/C:/Users/Public/Code/stm32/f767/autoControl/FINISH.md) 隞亙???HTTP ??嚗?  - ??憿?擐??之?lose 憭芣?ody ?芣
  - ?桀???嚗?嚗TTP listener 撱箇?頝臬?銝帘
- 撌脣? [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c) ?寞?銝?靘陷?函? `HttpService` task嚗?  - 蝘駁 task create 頝臬?
  - `http_service_poll()` ?寧?湔 `tcpip_try_callback(http_service_tcpip_step, NULL)`
- 撌脣 [Core/Src/main.c](/C:/Users/Public/Code/stm32/f767/autoControl/Core/Src/main.c) ??`StartDefaultTask()` 銝餉艘?葉? `http_service_poll();`
- ???build嚗?  - `Program Size: Code=107366 RO-data=13426 RW-data=15940 ZI-data=50852`
  - `0 Error(s), 0 Warning(s)`

## 2026-05-06 Web relay 撱園?啁?隢?
- 雿輻???啣??梧?擐?撌脩??賣???雿?Web UI ?批 relay ??璆菜嚗?鈭雯????銝帘??- ?撖拇敺Ⅱ隤?HTTP ??TCP control ?賣??lwIP `tcpip` thread 鋆∟???瘙????甇亙??`control_queue_submit()` 蝑?worker 摰?嚗??餃??游?lwIP thread??- 撌脫??
  - [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c)嚗TTP relay API ?湔 `relay_domain_execute(...)`
  - [App/Service/control_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/control_service.c)嚗CP control 銋??`relay_domain_execute(...)`
- ?血?撌脫撖?HTTP ???鞈?嚗?  - `HTTP_CONN_POOL_SIZE: 4 -> 8`
  - `tcp_listen_with_backlog(...): 2 -> 4`
- ???build嚗?  - `Program Size: Code=107038 RO-data=13426 RW-data=15940 ZI-data=50932`
  - `0 Error(s), 0 Warning(s)`

## 2026-05-06 ?券撖拇鋆?

- 瘝?? thread / queue 鋡怎??撱箇??湔??嚗蜓閬?task/queue ?賣??格活撱箇?摰???- 雿?曆???航霈?鈭擃??Ｕ?暺?
  - [App/Service/control_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/control_service.c) ??? response 撠梁???`tcp_close()`
  - ???剝??擃?批敺?迎?? send queue ?芣?蝛箸???銋暹楊???PCB 憸券
- 撌脖耨甇?`control_service` ????嚗?  - ?寧 `tcp_sent` / `tcp_poll` 蝑? queue 皜征敺? close
  - ?踵??曹澈 `static client_ctx`

## 撠??暹?

- 撠?嚗STM32F767ZI + FreeRTOS + lwIP + Keil/ARMCLANG`
- 銝餉??嚗?  - 銋云蝬脰楝 DHCP
  - TCP control service
  - UDP discovery service
  - HTTP service / Web UI
  - 16 頝?relay ?批

## 撌脣??????箇?

- 憭折? lwIP `UNALIGNED HardFault` 撌脰???  - `ip4.c`
  - `dhcp.c`
  - `etharp.c`
  - `icmp.c`
  - `igmp.c`
  - `pbuf.c`
  - `tcp_in.c`
  - `cc.h`
- ETH RX custom pbuf ???歇?寞?摰??頝臬?
- `HAL_ETH_ReleaseTxPacket()` 撌脫??冽? pending TX ???澆
- PHY fallback 撌脫??
  - `100HD`
  - `10HD`
- `control_service` / `discovery_service` / `http_service` ?賢歇 RTOS ??
## ?桀?甇?餈賜???

- ?桀?銝 HardFault
- 銝 scheduler ?⊥香
- 銝 `osDelay(100)` ?⊥香
- 銝 `HAL_ETH_Start()` ?? PHY/MAC bring-up
- ??啣暺?嚗?  - `ethlink start mac ok`
  - `ethlink netif link up`
  - 銋?瘝??游? log

## ??唳?銵霈

### 蝚砌??挾?方?

- ???瑞?嚗?  - `tcpip_mbox` 鋡?service tasks ??`tcpip_callback()` 憛遛
- 撌脣?靽格迤嚗?  - `control_service_task()` ?寞? `tcpip_try_callback()`
  - `discovery_service_task()` ?寞? `tcpip_try_callback()`
  - `http_service_task()` ?寞? `tcpip_try_callback()`
  - `ethernetif_schedule_netif_callback()` ?寞? `tcpip_try_callback()`
  - `TCPIP_MBOX_SIZE` 敺?`6` ? `16`
- 雿蝙?刻???log 憿舐內嚗?憿??嚗?  - `ethlink netif link up`

### 蝚砌??挾?方?

- `netif_set_link_up()` ?折??頝?
  - `dhcp_network_changed(netif)`
- 撠????[lwip.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/App/lwip.c) ??`MX_LWIP_Init()` 鋆∪停??`dhcp_start(&gnetif)`
- ??質? `netif_set_link_up()` ??link up 頝臬?鋆∟???DHCP ?????
## ??唬耨甇?
### 1. 撱嗅? DHCP ????

- 撌脣? [lwip.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/App/lwip.c) 蝘駁嚗?  - `dhcp_start(&gnetif);`
- ?寞???[ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c) ??`ethernetif_netif_up_callback()` 鋆∴?
  1. `netif_set_up()`
  2. `netif_set_link_up()`
  3. `dhcp_start(netif)`

### 2. ? callback ?折?撠?雿?log

- ??log嚗?  - `netif cb up a`
  - `netif cb up b`
  - `netif cb up c`
  - `netif cb dhcp start`
  - `netif cb up d`

## 銝?頛芾??獐?方?

- 憒??芰??堆?
  - `netif cb up a`
  - `netif cb up b`
  - ?嗅???
  - 隞?”?∪ `netif_set_link_up()`

- 憒??嚗?  - `netif cb up c`
  - 雿???`netif cb dhcp start`
  - 隞?”?∪ `dhcp_start()`

- 憒??券?賣?嚗?  - 銝?甇亙停??
    - `[net] tx ok ...`
    - DHCP Discover ??
    - DHCP state ?臬敺?`selecting` ??
## 雿輻???啣祕皜祉?隢?
- ???桀????唬遙雿?DHCP request
- ??啣嗾頛?log ?賢??剁?
  - `ethlink start mac ok`
  - `ethlink netif link up`

## ???build

- `Program Size: Code=108350 RO-data=14082 RW-data=15940 ZI-data=33508`
- `0 Error(s), 0 Warning(s)`

## 2026-05-06 ?撖拇敺??交???

### ??頛芰?甇?Ⅱ隤???暺?
- ???那?瑞?撘Ⅳ撌脩??漲?乩噩??頝臬?嚗??賢??輸鈭?log ?嗆??- ?拙??＊??甇賂?
  - `HardFault_Handler()` 鋡急?征甇餃儐?堆???fault ???函??  - DHCP / lwIP / HAL ETH TX ?扯◤?憭折??郊 `HAL_UART_Transmit()`嚗??湔瘙⊥?蝚砌??????潮楝敺?
### ??頛芸歇??鈭?
- ?Ｗ儔 [Core/Src/stm32f7xx_it.c](/C:/Users/Public/Code/stm32/f767/autoControl/Core/Src/stm32f7xx_it.c) ??`HardFault_Handler()`嚗?  - ?孵??脣?Ｘ? `hardfault_dump()` ??naked wrapper
- 蝘駁瘛勗惜?郊閮箸嚗?  - [Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c](/C:/Users/Public/Code/stm32/f767/autoControl/Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c)
  - [LWIP/Target/ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c)
  - [Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_eth.c](/C:/Users/Public/Code/stm32/f767/autoControl/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_eth.c)
- 靽?擃惜???
  - link up/down
  - DHCP ???閬?  - 敹???PHY fallback 閮

### ?箔?暻潮見??
- 雿輻??敺??啣??剁?
  - `dhcp_discover()` 銋?
  - ??`ethlink netif link up` 銝撣?- 雿??code 撌脩??剁?
  - lwIP core
  - ethernetif TX path
  - HAL ETH driver
  ?折?賣?鈭?blocking UART
- ??霈??撠???????? MAC/DMA?隞嗡?憭勗?臬?颲冽?
### ???build

- `Program Size: Code=107366 RO-data=14066 RW-data=15940 ZI-data=33500`
- `0 Error(s), 0 Warning(s)`

### 銝?甇交?閰脫獐皜?
1. ???圈?擃?2. ??佗?
   - ?亙?甈?fault嚗?冽???啣 `[fault] HardFault ... PC=...`
   - ?乩? fault嚗?阡?憪????DHCP Discover
3. ?郊??蝣箄?嚗?   - `DHCP Discover` ?臬銝?
   - ?仿??舀???銝?甇亦?亙??剔? `udp_sendto_if_src -> ip4_output_if_src -> ethernet_output -> low_level_output`

### ?交???

- 銝????郊 UART log ?莎?
  - `dhcp.c`
  - `udp.c`
  - `ip4.c`
  - `ethernet_output()`
  - `HAL_ETH_Transmit()`
- 憒?銝?頛芷?閬?雿??芸??剁?
  - ?Ｗ儔敺? HardFault dump
  - 擃惜?? log
  - ??

## 2026-05-06 ?啁Ⅱ隤???fault 暺?
- 雿輻????敺??潭?啁?甇?fault嚗?  - `PC=0x0800EBE8`
  - `LR=0x0800EB2F`
  - `CFSR=0x01000000`
- ??霅臬???[LWIP/Target/ethernetif.c](/C:/Users/Public/Code/stm32/f767/autoControl/LWIP/Target/ethernetif.c) ??`low_level_output()`
- 銝 DHCP state machine ?祈澈?豢?嚗嚗?  - `low_level_output()` ?函?甇????閫?? `tx_dma_buffer`
  - 霈鈭? `tx_dma_buffer + 2`?+26`?+30` ?車??4-byte 撠?雿蔭??32-bit ??  - `armclang` ?Ｙ???`LDR` ?湔閫貊 `UNALIGNED UsageFault`

### ?活撌脖耨甇?
- 蝘駁 `low_level_output()` ?折??挾??閬? TX frame 閫??/蝯梯?
- 靽??迤敹???
  - TX pbuf 銴ˊ??DMA buffer
  - `SCB_CleanDCache_by_Addr(...)`
  - `HAL_ETH_Transmit(...)`

### ?誨銵其?暻?
- 銋??HCP ?寞瘝?颯撠?銝?典??臬??箇洵銝??TX 頝臬??券?停??fault
- 銝?頛芸???fault 瘨仃嚗????潛匱蝥?瘀?
  - DHCP ?臬???
  - ?交??雿???嚗????PHY/duplex/?嗅?

### ???build

- `Program Size: Code=107142 RO-data=14066 RW-data=15940 ZI-data=33452`
- `0 Error(s), 0 Warning(s)`

## 銝?甇?
1. ????圈?擃?2. 閫撖?`netif cb up a/b/c/d`
3. ?亦???`dhcp start`嚗???血?橘?
   - `[net] tx ok ...`
4. ???冽??Ⅱ隤?DHCP Discover ?臬??銝?
## 2026-05-06 ?交???嚗HCP packed header fault

### ?活撌脩Ⅱ隤????
- ?桀???蝺??臬銝 DHCP ?摩?航炊嚗憭?lwIP packed header ??`armclang + Cortex-M7` 銝??芸?朣?32-bit 摮?
- 撌脩Ⅱ隤蒂靽格???fault 憿?嚗?  - `low_level_output()` ?批? TX frame 蝺抵????敹?閫?????芸?朣???  - `igmp_input()` 撠?packed `igmp_group_address` ??亥???  - `dhcp_recv()` 撠?packed `reply_msg->xid` ??亥???
### ??頛芸祕?耨??
- [dhcp.c](/C:/Users/Public/Code/stm32/f767/autoControl/Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c)
  - ?啣? bytewise safe helper
  - ?寞?嚗?    - `reply_msg->xid`
    - `msg_in->yiaddr`
    - `msg_in->siaddr`
    - `msg_out->xid`
    - `msg_out->ciaddr`
    - `msg_out->cookie`
- ???build嚗?  - `Program Size: Code=107614 RO-data=14046 RW-data=15940 ZI-data=33452`
  - `0 Error(s), 0 Warning(s)`

### 雿輻??銝甇亥?皜砌?暻?
1. ????圈?擃?2. 蝣箄? `PC=0x08007974` ??HardFault 銝??箇
3. 閫撖?DHCP ?臬?賢? `selecting` ?脣 `checking` ??`bound`
4. ?仿????`CFSR=0x01000000`嚗????湛?
   - `PC`
   - `LR`
   - `CFSR`

### 銝?甇亙極雿遣霅?
- 憒???敺?DHCP ?賢??莎?
  - ???嗆?憭?蝬脰楝 log
  - ???剖? HTTP / discovery / control ?飛
- 憒??歲?啁? unaligned fault嚗?  - ?湔??`PC/LR` 撠?銝???lwIP packed header 頝臬?
  - 銝??蝣????? log 餈賣
## 2026-05-06 ?交?鋆?嚗THARP packed header fault

- DHCP ?桀?撌脩??質粥??`bound`
- ?啁???fault嚗?  - `PC=0x0800992E`
  - `CFSR=0x01000000`
- 撌脣???[etharp.c](/C:/Users/Public/Code/stm32/f767/autoControl/Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c) ??`etharp_output()`
- ??暺 `LWIP_AUTOIP` ??湔霈 packed `ip_hdr.src`
- 撌脖耨甇??嚗?  - bytewise 銴ˊ packed `src`
  - ? aligned `ip4_addr_t` ??`ip4_addr_islinklocal()`
- ???build嚗?  - `0 Error(s), 0 Warning(s)`
# mem

## 2026-05-06 ?交???

- ?嗅?撖行????
  - DHCP 撌脣 `bound`
  - `ping` 甇?虜
  - TCP control `PING` 甇?虜
  - Web/HTTP 隞蝣箄?????
- ?祈憚撌脣?憭折??急? debug log ?嗆?嚗靽?嚗?  - `boot`
  - `link up/down`
  - `dhcp ...`
  - `http up/err`
  - `fault`
- HTTP ??頝臬?撌脫嚗?  - [App/Service/http_service.c](/C:/Users/Public/Code/stm32/f767/autoControl/App/Service/http_service.c)
  - `tcpip_try_callback()` -> `tcpip_callback()`
  - ?啣??撠???
    - `[http] up 80`
    - `[http] pcb err`
    - `[http] bind err`
    - `[http] listen err`
    - `[http] cb err=...`
- ???build嚗?  - `Program Size: Code=107502 RO-data=13482 RW-data=15940 ZI-data=50852`
  - `0 Error(s), 0 Warning(s)`

- ?血歇靽格迤嚗?  - [Core/Inc/FreeRTOSConfig.h](/C:/Users/Public/Code/stm32/f767/autoControl/Core/Inc/FreeRTOSConfig.h)
    - `configTOTAL_HEAP_SIZE` ????`32768`
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

# 2026-05-07 ?垢 pending ?撮 session/頛芾岷??
- 雿輻???綽??啁??畾澆??剁?雿????摰嫣?蝛箇嚗??汗??network ?瑟???pending嚗?? session 璈????- 瑼Ｘ `Web/index.html` 敺Ⅱ隤?
  - `refreshAll()` 瘥?3 蝘 `setInterval` ?⊥?隞嗉孛??  - 憭???雿???`await refreshAll()`
  - 瘝?隞颱? in-flight guard嚗?瘙?摰寞?????
- 撌脖耨?孵?蝡荔?
  - `state.refreshInFlight`
  - `state.refreshPending`
  - `apiFetch()` ? 4 蝘?timeout / abort
  - ?芸?頛芾岷?寧 5 蝘?銝?`document.hidden` ??銝餃???- 撌脤??啁???`Web/web_assets.h`
- 敺?霅??啁????敺?`/api/config/relays`?/api/cluster/nodes` ?臬銝? pending嚗蜓?恍?身摰??批捆?臬甇?虜憿舐內??

# 2026-05-07 敺垢 tcpip callback 蝬剛風蝭瘚?- ?湔?祆? curl 皜祈岫 `192.168.0.245` ??`192.168.0.157` ??`/`?/api/session`?/api/config/relays`?/api/cluster/nodes`嚗??6 蝘?timeout??- ?”蝷箏?憿歇銝?桃??垢?恍嚗 HTTP listener / tcpip thread ?典祕璈??脣銝?????- 撌脣?靽桀?蝡荔?
  - refresh ??  - pending ?蔥
  - 4 蝘?fetch timeout
  - 5 蝘憚閰?- ?脖?甇交??`defaultTask` 瘥?100ms ?澆 `http_service_poll()` / `control_service_poll()`嚗?蝥 `tcpip_callback()` ??lwIP thread 憛雁霅瑕極雿?鞎?銝????迤 TCP ????- 撌脣???`http_service.c` ? `g_http_poll_pending` 蝭瘚??踹???? callback??- 銝?甇交迤?典? `control_service.c` ??璅??瘚?

# 2026-05-07 HTTP ?券 timeout ??poll 蝭瘚?- ?湔?祆? `curl` 皜?`192.168.0.245`?192.168.0.157` ??`/`?/api/session`?/api/config/relays`?/api/cluster/nodes`嚗?典 6 蝘 connection timeout??- ?”蝷箇??憿歇銝?桃??垢 DOM嚗?踹?銝? HTTP listener / tcpip thread 撖阡?銝脣銝?????- ?脖?甇交炎?亦?橘?
  - `Web/index.html` ??歇??`refreshInFlight`嚗?隞?箏?頛芾岷嚗歇?寞? `setTimeout` 銝脰?頛芾岷??4 蝘?fetch timeout??  - `Core/Src/main.c` 瘥?100ms ?澆 `http_service_poll()` / `control_service_poll()`??  - ?抵?湔 `tcpip_callback(...)`嚗??遙雿?pending 蝭瘚?鞎?銝?賣?蝥? maintenance callback 憛?lwIP thread嚗?憯?甇?? TCP ????- 撌脖耨?對?
  - `Web/index.html`嚗葡銵?refresh?ending ?蔥?? 蝘憚閰Ｕ???hidden 銝憚閰Ｕ?  - `App/Service/http_service.c`嚗憓?`g_http_poll_pending`??  - `App/Service/control_service.c`嚗憓?`g_control_poll_pending`??- 撌脤??啁???`Web/web_assets.h` 銝衣 Keil CLI build 撽?????

# 2026-05-07 HTTP 閮箸 log 撘瑕?
- 雿輻??瘙?亙? log 皜祈岫嚗???皜研?- 撌脣 `App/Service/http_service.c` ??撠?閬?log嚗?  - `[http] listener down`
  - `[http] accept active=...`
  - `[http] accept full`
  - `[http] close ok active=...`
  - `[http] close defer err=...`
  - `[http] err=... active=...`
  - `[http] poll skip=...`
  - `[http] poll err=...`
- ?桃?嚗??HTTP ?湧? timeout ????listener 鋡恍?????詨?皛蹂??????poll callback ????

# 2026-05-07 HTTP request/send 頝臬??湔??log
- ?寞??桀? log嚗?  - ??`accept`
  - 憭銋? `close ok`
  - 瘝? `accept full` / `close defer` / `poll skip`
- ?”蝷箏?銝頛芣?? connection pool ?⊥香??callback ??嚗??舐???湔?蜓??- 瘥? `81c776a` 敺??憭抒?憸券暺嚗?  - ?圈???HTML 蝝?25KB
  - ?啣? `/api/config/cluster`?/api/config/relays`?/api/cluster/nodes` 蝑?憭?JSON
  - 雿?`http_service_recv()` 隞?亙蕭??`http_service_handle_request()` ???喳?- 撌脖耨??`http_service.c`嚗?  - `recv()` ?曉? request first line嚗[http] req ...`
  - ??`http_service_handle_request()` ???`ERR_OK`嚗??堆?`[http] send err=...`
  - 銝衣??喲??府???嚗??request ???∩?
- ????湔?儘嚗?  - request ?寞瘝脖?
  - ??request ?脖?鈭?雿?send/tcp_write/tcp_output 憭望?

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
    - 銝餌??    - ?ａ???暺?    - 閮剖?
    - 蝟餌絞???  - ?祆? relay and same-cluster remote relay are both on 銝餌??  - member nodes see state but cannot control, and get a "?? Master" button
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
    - `銝餌?瓩
    - `??隞?暺
    - `?? Master`
    - `蝑?蝭暺????常
- Frontend changes applied:
  - polling interval reduced from one heavy 4s cycle to a lighter 1.5s tick scheduler
  - per-resource polling cadence:
    - relay state: high frequency on `銝餌?瓩
    - node list: lower frequency
    - session / cluster config: lowest frequency
    - no auto polling on `閮剖?`
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

## 2026-05-07 ?垢擐????憚閰ａ?頛?- 雿輻??蝣箄?瘙?閬???璈?/ ?ａ??拙之?憛??隞乓身??蝔梧??砍 / ????銝鳴?銝?湔?府閮剖??舐蝜潮?具?- ??港遢?孵神 `Web/index.html`嚗?  - `銝餌?瓩 ?寞??桐? `deviceList`
  - 瘥身??＊蝷箏?蝔晞??脯P ??16 頝舐匱?餃
  - ?祆???蝡舫?寧??蝔桅???見撘?- 頛芾岷蝑?郊??嚗?  - `session / clusterConfig` ?
  - `relay` ?芸銝餌?ａ??餅?
  - `nodes` 銝餌?ａ??颯??頛???  - `閮剖?` ????? relay config
- ?祆???蝡舀?嗡???pending ??嚗?銝??亦撱園?岫嚗?敺?蝡舐???甇亥圾?扎?- ??? `Web/web_assets.h` 銝衣 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 撽?蝺刻陌????## 2026-05-07 擐??桐?閮剖?皜??enabled_mask
- 雿輻??瘙蜓?恍銝????璈?/ ?ａ??拙?嚗靘身??蝔梧??砍 / ???銝撘萄銝撘萄????- 敺垢鋆? discovery announce ??`enabled_mask`嚗?  - `cluster_node_t` ?啣? `enabled_mask`
  - `discovery_proto_build_announce()` / `parse_announce()` 撣?`enabled=...`
  - `discovery_service` ?望??`device_config.relay[].enabled` 蝯雿??桃蔗
  - `/api/cluster/nodes` ?曉? `enabled_mask`
- ?垢 `Web/index.html` ?港遢?神嚗?  - 銝餌?Ｘ?銝 `deviceList`
  - ?芸???relay ?湔?梯?
  - 瘥????桃 pending嚗???啣銝韏瑕?蝯?  - 瘥? relay ?憓??脯?????蝺亙?甇?- `Web/web_assets.h` 撌脤??啁???銝衣 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 撽?蝺刻陌????- ?垢 relay ?迂?郊 / 靽格撠????discovery ???桀??芸葆 mask ??enabled ???撠撣嗅?蝔梯???## 2026-05-07 蝺刻陌靽格迤嚗ain.c 畾?摮?
- `Core/Src/main.c` ??`main()` ?畾?鈭??ㄧ `0`嚗? `expected ';' after expression`??- 撌脩宏?歹?銝衣 `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 撽?蝺刻陌?Ｗ儔????## 2026-05-07 ?砍?ａ?閮剖?甈????亙?甇Ｘ???- 雿輻??皜??砍鋆蔭銝摰隞乩耨?寡撌梁??ａ??鞈?嚗雿輻? member嚗?閬銝餃???箝撖Ⅳ??ａ??迂??- ?迨蝘駁 `http_service.c` 銝剖?嚗?  - `POST /api/config/cluster`
  - `POST /api/config/relay`
  ??`not_master` ?嚗?箏閬??餃?喳??- ?垢 `Web/index.html` ?活?港遢?神嚗?  - 閮剖??憓皜砍?????株?憟??
  - kick UI 蝘駁
  - 銝餌?Ｘ?憿?relay ???祇???蝝????????  - 銝?祇???pending ??撌?  - 蝺亙?甇Ｘ???頝?銝??pending 銝韏瑕?蝯?- 隤芣?嚗?  - `remember_state` ???嚗?嚗?蝵桅????Ｗ儔閰脰楝?敺?甈∪祕?????亙??剁??粥 `boot_default_on`??## 2026-05-07 蝺刻陌靽格迤嚗ain.c 畾? `{0`
- `Core/Src/main.c` ??`main()` ??◤? `{0`嚗???`expected ';' after expression`??- 撌脖耨甇??甇?虜??`int main(void) {`??- ???`UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0` 撽?蝺刻陌????## 2026-05-07 16:25 build gate

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
    - red `? remains usable
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
  - frequent clicks can leave UI showing `? while actual member relay is `?
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
  - `??ａ?` now uses dedicated `.checkline` layout in `Web/index.html`
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

## 2026-05-07 backend long-link phase started

- User selected the next phase as backend long-lived connection before ADC/CT work.
- Added `tools/backend-test/` with:
  - `package.json`
  - `server.js`
  - `README.md`
- Test backend design:
  - Express HTTP server for inspection/command injection
  - TCP server for the firmware long link
  - shared key check on `hello`
- Added `App/Service/backend_link_service.[hc]`
  - only cluster master should keep the outbound TCP session
  - current minimal messages:
    - `hello`
    - `heartbeat`
    - `status`
  - basic inbound command handling:
    - `set_relay`
    - `set_mask`
    - `set_all`
- Current defaults are compile-time only:
  - host `192.168.0.10`
  - port `42001`
  - key `dev-key`
- Service was wired into `StartDefaultTask()` through `backend_link_service_poll()`
- Keil project updated to include `backend_link_service.c`
- Keil CLI rebuild passed after this slice:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

## 2026-05-07 backend config wiring

- Added backend persistence fields into `device_config`:
  - `backend_host`
  - `backend_port`
  - `backend_key`
- Migrated `device_config_load()` so old v1/v2 flash layouts are promoted into v3 and receive backend defaults:
  - host `192.168.0.10`
  - port `42001`
  - key `dev-key`
- Updated `backend_link_service` to read runtime host/port/key from `device_config_get()` instead of compile-time constants.
- Added backend fields to `/api/config/cluster`:
  - GET now returns `backend_host`, `backend_port`, `backend_key_configured`
  - POST now saves `backend_host`, `backend_port`, `backend_key`
- Updated `Web/index.html` settings page with Chinese fields for:
  - 敺垢銝餅?
  - 敺垢??
  - 敺垢?
- Regenerated `Web/web_assets.h` from the updated `Web/index.html`.
- Verified local backend-test service is running:
  - `GET http://127.0.0.1:4300/` returned `{"ok":true,"httpPort":4300,"tcpPort":42001,"backendKey":"dev-key","clients":[]}`
- Confirmed current PC LAN IP is `192.168.0.139`, so boards must be configured to connect to:
  - host `192.168.0.139`
  - port `42001`
  - key `dev-key`
- Keil CLI rebuild passed after backend config wiring:
  - `UV4.exe -b MDK-ARM\\autoControl.uvprojx -j0`
  - exit code `0`

## 2026-05-07 backend long-link live test

- User started the local backend-test service.
- Verified backend-test HTTP endpoints locally:
  - `GET http://127.0.0.1:4300/` returned:
    - `{"ok":true,"httpPort":4300,"tcpPort":42001,"backendKey":"dev-key","clients":[]}`
  - `GET http://127.0.0.1:4300/clients` returned `{"ok":true,"clients":[]}`
- Verified current PC LAN IP is `192.168.0.139`, so boards were updated via `/api/config/cluster` to:
  - `backend_host = 192.168.0.139`
  - `backend_port = 42001`
  - `backend_key = dev-key`
- After a short convergence delay:
  - `192.168.0.157` became `master`
  - `192.168.0.245` remained `member`
  - `/clients` showed exactly one backend connection from the master only
- Confirmed `backend_link_service` live behavior:
  - master connected successfully to backend-test
  - backend-test `/clients` showed:
    - `nodeId = 579948958`
    - `device = autoControl`
    - `cluster = default`
    - `role = 2`
    - `relayMask` updates
- Confirmed backend downlink commands to master local relay:
  - `POST /send/579948958/relay` with `{"channel":1,"value":1}` turned `ch1` on
  - `POST /send/579948958/mask` with `{"mask":3}` produced `relay_mask = 3`
  - `POST /send/579948958/all` with `{"value":0}` turned all relays off
- Current scope is now validated for:
  - master-only outbound long link
  - hello / heartbeat / status uplink
  - local relay control downlink through backend-test
- Not yet implemented in this slice:
  - backend command routing to member via master
  - UI display of backend link state
## 2026-05-07 backend long-link boot fault triage
- User reported boot log stops after `[net] dhcp off` with bare `[fault] MemManage`.
- Compared current worktree vs stable commit `58c4ebf`; high-risk new boot-path changes were backend link poll and config worker init behavior.
- Applied three mitigations:
  1. `backend_link_should_run()` now requires network ready (`netif up`, `link up`, non-zero IPv4) before backend task attempts any socket work.
  2. `BackendLinkTask` stack increased from 768 to 1024.
  3. `device_config_init()` no longer creates queue/task before scheduler when flash config is invalid; it only writes defaults directly.
- Improved `MemManage_Handler` to print `CFSR/HFSR/MMFAR/BFAR` instead of only `[fault] MemManage`.
- Keil CLI rebuild passed after patch.
## 2026-05-07 MemManage root cause and fix
- User provided detailed fault after boot: bare `[fault] MemManage` followed by HardFault with `PC=0x080093D6`, `MMFAR/BFAR=0x20080000`.
- Disassembly showed `PC=0x080093D6` inside `device_config_is_valid()` CRC validation path.
- Root cause: config CRC helpers (`device_config_is_valid`, v1/v2 validators, `device_config_compute_crc32`) created full struct temporaries on stack; during early boot `main()` stack/local layout put the temp buffer right at top of SRAM and optimized CRC code read past `0x2007FFFF` to `0x20080000`, triggering MemManage.
- Fix: replaced temp-copy CRC approach with in-place CRC computation that zeroes `config_crc32` logically during iteration (`device_config_crc32_zeroed_range`). This removes large stack temporaries from config validation.
- Earlier mitigations retained: backend only runs after network ready, backend task stack increased, MemManage handler prints register context.
- Keil CLI rebuild passed after fix.
2026-05-07

- backend ?琿???啗???
  - ??箏???砍???寡??ａ?頨思遢嚗??蝔?撖Ⅳ嚗?嚗TTP ?脣??摩???蝛?`backend_key`
  - backend host/port 靽?嚗靘蹂?敺??啗撓?仿???  - `backend_link_should_run()` ?曉閬? `backend_key` ?征嚗??蝛箏?隞?fallback ??`dev-key` 撠????
- backend-test `server.js` ?啣? duplicate master ??嚗?  - 隞?`key|cluster` ??
  - grace period 15 蝘?  - 頞?敺閮?`duplicate-master-warning` event嚗?銝餃??瑞?
- live 皜祈岫鋆?嚗?  - `245` ??箏??嚗/api/config/cluster` 撌脩Ⅱ隤?`backend_key_configured=0`
  - routed backend control ?隞???master ?祆?嚗??? parser ???唳璈?`set_relay` ?嚗歇?寞??芾? payload ??`node_id` 撠勗?粥 routed path
  - backend-test ???拙祕?冽??塚?
    - ??`nodeId` ?圈????`replace-client` 銝阡??? socket
    - `CLIENT_STALE_MS=12000` 敺??`stale-client-close`
  - 隞乩? server 霈?閬???backend-test ????
- 撌脫?圈?霅?
  - Keil CLI build ??
  - `node --check tools/backend-test/server.js` ??
2026-05-07
- backend status uplink ?啣? dirty push 璈
- backend_link_service 憓? `backend_link_service_mark_status_dirty()`
- local relay change?emote relay change?ode online/offline?ole/master change ?賣?璅? dirty
- backend task ???摮暑??瘥?200ms loop ?????dirty status嚗像撣訾?靽? 3 蝘望? status

