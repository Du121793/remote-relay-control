# autoControl 系統架構文件

> 工業 IoT 電源控制節點 — STM32F767ZI + FreeRTOS + LwIP + MQTT TLS-PSK

---

## 1. 整體架構圖

```
┌──────────────────────────────────────────────────────────────────────┐
│                       後端 VM (vm402)                                │
│                                                                      │
│  Browser ──HTTPS──► nginx (:80/:443)                                 │
│                       │ /api/* → proxy                               │
│                       │ /      → proxy (dashboard)                    │
│                       ▼                                              │
│  ┌──────────────────────────┐         ┌─────────────────┐           │
│  │  server.js (:4300)       │  MQTT   │  mqtt-psk-broker.js      │           │
│  │  • dashboard HTML        │◄───────►│  (:8883)        │           │
│  │  • provisioning (HTTPS)  │ TLS-PSK │  PSK auth + ACL │           │
│  │  • broker control API (HTTP)           │         │                 │           │
│  │  • key management        │         └─────────────────┘           │
│  └──────────────────────────┘                                        │
└──────────────────────────────────────────────────────────────────────┘
                                    │
                              MQTT TLS-PSK
                              Internet (直連)
                                    │
┌───────────────────────────────────┼──────────────────────────────────┐
│                        LAN (192.168.0.x)                             │
│                                                                      │
│  ┌──────────────────────┐    ┌──────────────────────┐               │
│  │  MCU Master          │    │  MCU Member(s)       │               │
│  │  HTTP :80            │◄──►│  HTTP :80            │               │
│  │  UDP :40000          │    │  UDP :40000          │               │
│  │  MQTT :8883 (唯一)   │    │                      │               │
│  │                      │    │  (no MQTT — commands │               │
│  │  config sync ────────┼───►│   forwarded by       │               │
│  │  (AES-128-GCM)       │    │   master via LAN)    │               │
│  └──────────────────────┘    └──────────────────────┘               │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 2. 通訊埠總覽

| Port | 協定 | 方向 | 用途 |
|:----:|------|------|------|
| 80 | HTTP | Browser → MCU | LAN 本機設定頁 / API |
| 443 | HTTPS | MCU → Cloudflare → Backend | 憑證換發 (provisioning) |
| 40000 | UDP | MCU ↔ MCU | Discovery / Announce |
| 8883 | MQTT TLS-PSK | MCU ↔ Broker | 後端即時通訊 |
| 4300 | HTTP | localhost (nginx → server.js) | 後端 dashboard + API |

> Raw TCP :42001 已退役。port 41000 TCP 僅保留診斷用途（寫入已鎖）。

---

## 3. 雙路徑通訊

### 路徑 A — HTTPS 憑證換發（一次性）

```
MCU ──HTTPS──► Cloudflare ──► nginx ──► server.js (:4300)
                  │                        POST /api/provision
                  │                        { cluster_id, device_id, fingerprint }
                  │                        ▼
                  │                      server.js 驗證 → 產生 binding_secret
                  │                        ▼
                  ◄── { binding_secret, ca_cert_pem, provisioning_revision }
```

MCU 收到後存入 Flash，binding_secret 作為 MQTT PSK 的原料。

### 路徑 B — MQTT TLS-PSK 即時通訊

```
MCU (master only) ──MQTT TLS-PSK──► mqtt-psk-broker.js (:8883)
  │                                   │
  ├─ PUBLISH:                        ├─ 轉發給 subscribe 的 client
  │   cluster/{id}/status            │   (server.js or other MCU)
  │   cluster/{id}/events
  │   cluster/{id}/measure
  │
  └─ SUBSCRIBE:
      cluster/{id}/command/relay        ← 後端 → MCU 單路控制
      cluster/{id}/command/mask         ← 後端 → MCU 全路控制
      cluster/{id}/command/config/relay ← 後端 → MCU relay 設定
      cluster/{id}/command/time         ← 後端 → MCU 時間同步
      cluster/{id}/command/auto-hold    ← 後端感測器 Auto Hold
      cluster/{id}/command/control      ← 後端使用者控制
      cluster/{id}/command/scheduler/*  ← 後端排程控制
      cluster/{id}/command/protection/* ← 後端安全保護
```

---

## 4. 安全模型

### 4.1 認證方式

| 路徑 | 方式 | 說明 |
|------|------|------|
| MCU → Broker | TLS-PSK | PSK = base64url_decode(binding_secret)，256-bit |
| server.js → Broker | TLS-PSK | 獨立 PSK (`BACKEND_PSK_HEX`)，broker ACL 授權 |
| Browser → Backend | HTTPS | Cloudflare TLS 憑證 |
| MCU → Cloudflare | HTTPS | CA 根憑證驗證（mbedTLS） |
| MCU ↔ MCU | HMAC-SHA256 + anti-replay | 叢集認證，±30s 時間窗 |
| Browser → MCU | Session Token | HTTP `X-Auth-Token` header |

### 4.2 金鑰生命週期

```
1. 用戶在後端 dashboard 產生金鑰 (server.js)
2. 用戶手動複製 binding_secret → 貼入 MCU HTTP 設定頁
   或 MCU 透過 HTTPS provisioning 自動取得
3. binding_secret 存入 MCU Flash（不經網路傳送）
4. PSK 可由後端觸發輪替 (rotate-psk)
5. CA 根憑證由後端簽發，MCU 儲存驗證 broker 證書鏈
```

### 4.3 密碼儲存

| 項目 | 演算法 | 儲存位置 |
|------|--------|----------|
| 叢集密碼 | SHA-256 | Flash device_config |
| binding_secret | 256-bit base64url | Flash device_config |
| CA 根憑證 | PEM ≤ 2KB | Flash device_config |
| Discovery auth_tag | HMAC-SHA256 | 每次計算（含 timestamp） |
| Config sync | AES-128-GCM | cluster_password 前 16 bytes 為 key |

---

## 5. 控制路徑

### 5.1 Dashboard → MCU（直接 HTTP）

```
Browser dashboard
  │ POST /api/relay/channels/:ch/auto-hold
  │ Content-Type: application/x-www-form-urlencoded
  ▼
MCU HTTP API (:80)
  │ scheduler_handle_auto_hold()
  ▼
control_queue → relay_protection → GPIO
```

### 5.2 Backend → MCU（MQTT）

```
server.js
  │ mqttPublishCommand(clusterName, "relay", payload)
  ▼
mqtt-psk-broker.js :8883
  │ topic: cluster/{id}/command/relay
  ▼
MCU Master
  │ mqtt_link_process_command()
  ├─ target==self → 本地執行
  └─ target!=self → control_queue_submit_remote_async() → LAN → Member
```

### 5.3 控制優先權

```
1. 安全保護（過流/黏住/冷卻/故障鎖定）— 最高，不可覆蓋
2. 使用者手動控制（清 Auto Hold + 設 pause）
3. pause_blocks_auto_trigger + pause 有效
4. 後端感測器 Auto Hold
5. 使用者暫停排程器
6. 一般設備排程器
```

---

## 6. 部署

### 6.1 VM 服務

```bash
cd /srv/project/autoControl

# 1. 啟動 MQTT broker（TLS-PSK，port 8883）
MQTT_PORT=8883 node mqtt-psk-broker.js &

# 2. 啟動後端 server（dashboard + API，port 4300）
#    server.js 透過 broker control API (:4301) 發送 MQTT 命令
#    不需要自己持有 PSK，不需要 broker control API (HTTP)
node server.js &
```

### 6.2 nginx 設定

```nginx
# /etc/nginx/sites-enabled/autocontrol.conf
server {
    listen 80;
    listen 443 ssl;
    server_name autocontrol.du1217.club;

    ssl_certificate     /etc/nginx/ssl/autocontrol.pem;
    ssl_certificate_key /etc/nginx/ssl/autocontrol.key;

    location /api/ {
        proxy_pass http://127.0.0.1:4300;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
    location / {
        proxy_pass http://127.0.0.1:4300;
        proxy_set_header Host $host;
    }
}
```

### 6.3 MCU 燒錄

```bash
# Keil CLI
UV4.exe -b MDK-ARM\autoControl.uvprojx -j0
UV4.exe -f MDK-ARM\autoControl.uvprojx -j0  # flash via ST-LINK
```

---

## 7. API 文件

詳細 MCU HTTP API 請見 [MCU_API.md](./MCU_API.md)

### 7.1 後端 API

| 路由 | 用途 |
|------|------|
| `GET /` | Dashboard HTML |
| `GET /api/summary` | 系統摘要（MQTT 狀態、金鑰數、裝置數） |
| `GET /api/keys` | 金鑰清單 |
| `POST /api/keys` | 產生新金鑰 |
| `PATCH /api/keys/:id` | 編輯金鑰（標籤/啟用） |
| `DELETE /api/keys/:id` | 刪除金鑰 |
| `POST /api/provision` | MCU 憑證換發 |
| `GET /api/cloud/devices` | 雲端裝置清單 |
| `GET /api/cloud/clusters` | 雲端叢集清單 |
| `POST /api/cloud/clusters` | 建立叢集身份 |
| `POST /api/cloud/devices` | 建立裝置身份 |
| `POST /send/:nodeId/relay` | MQTT 單路控制 |
| `POST /send/:nodeId/mask` | MQTT 全路控制 |
| `POST /send/:nodeId/all` | MQTT 全部開/關 |
| `POST /send/:nodeId/auto-hold` | MQTT Auto Hold |
| `POST /send/:nodeId/control` | MQTT 使用者控制 |
| `POST /send/:nodeId/scheduler/set` | MQTT 排程設定 |
| `POST /send/:nodeId/scheduler/enabled` | MQTT 排程啟用 |
| `POST /send/:nodeId/scheduler/pause` | MQTT 排程暫停 |
| `POST /send/:nodeId/protection/force-stop` | MQTT 強制停止 |
| `POST /send/:nodeId/protection/unlock` | MQTT 解鎖 |
| `POST /send/:nodeId/protection/clear-learning` | MQTT 清除學習 |
| `GET /api/stream` | SSE 事件串流 |
| `GET /clients` | 舊 TCP 鏈路清單（已廢棄） |

---

## 8. Flash 配置

| Sector | 位址 | 用途 |
|--------|------|------|
| 0-5 | 0x08000000 | 韌體程式碼 |
| 10 | 0x081C0000 | 排程器資料持久化 |
| 11 | 0x081E0000 | device_config 持久化 |

---

## 9. 資源使用

| 資源 | 用量 | 上限 |
|------|:----:|:----:|
| SRAM (RW + ZI) | ~338KB | 512KB |
| Flash (code) | ~536KB | 2MB |
| FreeRTOS heap | 28.5KB | 62KB |
| lwIP heap | ~3KB | 48KB |

---

## 10. 開發

### 專案結構
```
App/
  Config/     device_config 持久化
  Domain/     業務邏輯 (ct_measurement)
  Platform/   硬體抽象 (board_relay)
  Proto/      通訊協定 (control_proto, discovery_proto)
  Service/    FreeRTOS 服務層
    scheduler.c/h        排程器 + Auto Hold
    mqtt_link_service.c  MQTT 後端通訊
    http_service.c       HTTP API + Web UI
    discovery_service.c  UDP 自動發現
    cluster_service.c    叢集管理
    relay_protection.c   安全保護
    control_queue.c      控制佇列
    ...
docs/
  ARCHITECTURE.md  本文件
  MCU_API.md       MCU HTTP API 參考
tools/
  backend-test/    後端測試伺服器 (server.js)
Web/
  index.html       內建 Web UI
```

### 編譯
- Keil MDK-ARM v5, ARM Compiler 6 (armclang)
- `UV4.exe -b MDK-ARM\autoControl.uvprojx -j0`
- 0 Error(s), 0 Warning(s) 才能燒錄
