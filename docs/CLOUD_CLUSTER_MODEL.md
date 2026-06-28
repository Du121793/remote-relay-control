# 雲端與本地叢集模型

## 1. 名詞

- 本地叢集名稱：LAN 內 MCU discovery 與選主使用。相同名稱加上相同叢集密碼才會互相信任。
- 本地叢集密碼：MCU 之間 discovery auth、service proxy、設定同步的信任基礎。
- 本機登入密碼：只用於登入單台 MCU Web，不是叢集密碼。
- Cloud Device ID：後端認領 MCU 用的唯一裝置 ID，由 MCU 產生並只讀顯示。
- Cloud Cluster ID：後端歸類 cloud devices 的 cluster id。一般情況跟本地叢集名稱保持一致。
- Cloud Key ID：provisioning / MQTT 身分的一部分，不應顯示給一般使用者手動修改。
- Binding Secret：MQTT TLS-PSK 使用的 secret，不應顯示給一般使用者。
- Backend Key：歷史欄位，不應作為新流程的使用者設定項。

## 2. 使用者流程

一般使用者應只需要：

1. 登入 MCU Web。
2. 在設定頁確認 Cloud Device ID。
3. 登入後端管理頁。
4. 在後端輸入 Cloud Device ID / Node ID 完成認領。
5. 後端產生 provisioning。
6. MCU 寫入 provisioning 後啟用後端連線。

目前測試版仍允許管理端直接輸入 Node ID / Device ID 認領。正式版建議改成 challenge / signature 流程：

1. 後端產生認領 challenge。
2. 使用者把 challenge 貼回 MCU。
3. MCU 用本機裝置祕密或叢集祕密簽章。
4. 後端驗證後才完成認領。

## 3. 安全政策

### 3.1 MCU Web 登入

- 使用本機登入密碼。
- 不等於叢集密碼。
- 不應由 backend server 遠端修改。

### 3.2 本地叢集安全

- 使用 `cluster_password_hash` 驗證 discovery 與 service proxy。
- 預設叢集密碼為 `autoControl-cluster`。
- ROM 沒資料時會使用預設叢集密碼。
- UI 必須提示「目前使用燒錄預設叢集密碼，建議修改」。
- 正式部署前應修改本地叢集密碼。

### 3.3 Master 到 Member service proxy

- 目前是 LAN HTTP。
- 透過 service-scope token 與叢集密碼衍生 auth tag 驗證。
- 目前不是端到端加密，LAN 內仍可能被觀察封包。
- 已討論未來應改為 AEAD，例如 ChaCha20-Poly1305。

### 3.4 後端到叢集

- 後端只應直接控制 Master。
- Master 負責轉發到 Member。
- 後端不應要求瀏覽器直接打 Member IP。

### 3.5 MQTT

- Master 透過 MQTT TLS-PSK 連 broker。
- Binding Secret 由後端 provisioning 下發。
- TLS 失敗必須釋放 client / TLS config，避免 RAM 長期占用。
- 連線失敗必須 backoff，避免反覆握手造成 HTTP chunk memory 壓力。

## 4. Provisioning 欄位

後端 `/api/provision` 回傳：

- `cloud_key_id`
- `cloud_transport`
- `cloud_broker_host`
- `cloud_broker_port`
- `cloud_cluster_ca_fingerprint`
- `cloud_cluster_ca_cert_pem`
- `cloud_cluster_binding_secret`
- `provisioning_revision`
- `ca_revocation_tag`

MCU 啟用 cloud 前需要至少具備：

- broker host
- broker port
- cloud device id
- cloud key id
- cloud cluster id
- cluster CA fingerprint
- binding secret

缺少 CA fingerprint 或 binding secret 時，MCU 會拒絕啟用 cloud。

## 5. 多叢集使用情境

如果一個使用者有兩個叢集，每個叢集三台設備：

- 後端應有兩個 Cloud Cluster。
- 每台 MCU 仍有自己的 Cloud Device ID。
- 每個 Cloud Device 歸屬到其中一個 Cloud Cluster。
- 本地 LAN 內如果兩個獨立叢集共存，必須使用不同本地叢集名稱與不同本地叢集密碼。

## 6. MQTT 連線驗證欄位

MQTT TLS-PSK 與 CONNECT 會同時檢查多個欄位：

- PSK identity：Cloud Device ID。
- MQTT username：Cloud Key ID。
- MQTT password：Binding Secret。
- Broker registry：Cloud Device ID、Cloud Key ID、Binding Secret 必須與後端 provisioning 後的資料一致。

如果 broker log 顯示 `connect-rejected` 而不是 `psk-rejected`，通常代表 TLS-PSK 已通過，但 MQTT CONNECT 的 username/password 或 device registry 驗證失敗。MCU 套用 provisioning 時必須同步寫入 `cloud_key_id`，否則會保留舊 key id 導致連線被拒。

## 7. Master 上報 Member

目前只有 Master 需要連線後端 MQTT broker。Member 不直接連後端時，後端仍需要看到整個叢集，因此 Master 的 MQTT status 會附帶精簡 `nodes` 陣列。

`nodes` 只包含必要欄位：

- Node ID
- LAN IP
- 角色
- term
- relay mask
- online/device state
- cloud device/key id

後端 broker 會保存最近一次 status 裡的 `nodes`，後端管理頁再從 `/api/clients` 展開成 known nodes。
