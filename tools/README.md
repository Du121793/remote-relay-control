# autoControl Tools

## autocontrol_gui.py

最小 GUI 測試工具，提供：

- UDP `DISCOVER`
- Discovery node 清單
- ICMP `ping`
- TCP `PING`
- `GET RELAY`
- `SET RELAY MASK`

## 建立虛擬環境

```powershell
python -m venv --system-site-packages .VENV
```

## 執行原始碼

```powershell
.VENV\Scripts\python.exe tools\autocontrol_gui.py
```

## 打包 exe

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_autocontrol_gui.ps1
```

輸出位置：

```text
tools\dist\autoControlTool.exe
```
