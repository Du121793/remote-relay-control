$ErrorActionPreference = "Stop"

$repoRoot = "C:\Users\Public\Code\stm32\f767\autoControl"

$listenerPids = @()
foreach ($line in (cmd /c netstat -ano | findstr :4300)) {
  if ($line -match "LISTENING\s+(\d+)$") {
    $listenerPids += [int]$matches[1]
  }
}

$listenerPids = $listenerPids | Select-Object -Unique
foreach ($procId in $listenerPids) {
  try {
    Stop-Process -Id $procId -Force -ErrorAction Stop
  } catch {
  }
}

Start-Process -FilePath node -ArgumentList "tools/backend-test/server.js" -WorkingDirectory $repoRoot
Start-Sleep -Seconds 2
curl.exe -s http://127.0.0.1:4300/api/summary
