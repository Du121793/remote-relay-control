param(
    [string]$OutputName = "autoControlTool"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$venvPython = Join-Path $repoRoot ".VENV\\Scripts\\python.exe"

if (-not (Test-Path $venvPython)) {
    throw ".VENV 不存在，請先在專案根目錄執行：python -m venv --system-site-packages .VENV"
}

Push-Location $repoRoot
try {
    & $venvPython -m PyInstaller `
        --noconfirm `
        --clean `
        --onefile `
        --windowed `
        --name $OutputName `
        --distpath tools\dist `
        --workpath tools\build `
        --specpath tools `
        tools\autocontrol_gui.py
}
finally {
    Pop-Location
}
