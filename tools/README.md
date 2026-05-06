# autoControl Tools

## autocontrol_gui.py

目前 GUI 工具支援：

- UDP `DISCOVER`
- Discovery node 清單
- ICMP `ping`
- TCP `PING`
- TCP `GET RELAY`
- TCP `SET RELAY MASK`
- HTTP `GET /api/status`
- HTTP `GET /api/session`
- HTTP `POST /api/login`
- HTTP `POST /api/logout`
- HTTP `GET /api/relay`
- HTTP `POST /api/relay-mask`

## 建立虛擬環境

```powershell
python -m venv --system-site-packages .VENV
```

## 執行原始碼版 GUI

```powershell
.VENV\Scripts\python.exe tools\autocontrol_gui.py
```

## 打包 exe

預設輸出名稱：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_autocontrol_gui.ps1
```

輸出檔：

```text
tools\dist\autoControlTool.exe
```

如果舊 exe 正在使用中，改用不同輸出名稱：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_autocontrol_gui.ps1 -OutputName autoControlTool_http
```

輸出檔：

```text
tools\dist\autoControlTool_http.exe
```
