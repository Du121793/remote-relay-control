$ErrorActionPreference = 'Stop'

function Invoke-CurlJson {
  param(
    [string]$Url,
    [string]$Method = 'GET',
    [string]$Body = '',
    [hashtable]$Headers = @{}
  )

  $args = @('--noproxy', '*', '--silent', '--show-error', '--max-time', '5', '-X', $Method)
  foreach ($k in $Headers.Keys) {
    $args += @('-H', ("{0}: {1}" -f $k, $Headers[$k]))
  }
  if ($Method -ne 'GET' -and $Body -ne '') {
    $args += @('--data', $Body)
  }
  $args += $Url
  $raw = & curl.exe @args
  return ($raw | ConvertFrom-Json)
}

$base157 = 'http://192.168.0.157'
$base245 = 'http://192.168.0.245'
$backend = 'http://127.0.0.1:4300'

$login157 = Invoke-CurlJson -Url "$base157/api/login" -Method 'POST' -Body 'password=admin'
$login245 = Invoke-CurlJson -Url "$base245/api/login" -Method 'POST' -Body 'password=admin'
$token157 = [string]$login157.token
$token245 = [string]$login245.token

$status157 = Invoke-CurlJson -Url "$base157/api/status"
$status245 = Invoke-CurlJson -Url "$base245/api/status"
$nodes157 = Invoke-CurlJson -Url "$base157/api/cluster/nodes"
$memberSelf = Invoke-CurlJson -Url "$base245/api/relay-channel" -Method 'POST' -Body 'channel=1&value=1' -Headers @{ 'X-Auth-Token' = $token245 }
$masterRemote = Invoke-CurlJson -Url "$base157/api/cluster/node-relay" -Method 'POST' -Body 'node_id=1165657530&channel=2&value=1' -Headers @{ 'X-Auth-Token' = $token157 }
Start-Sleep -Milliseconds 1200
$relay245After = Invoke-CurlJson -Url "$base245/api/relay"
$clientsBefore = Invoke-CurlJson -Url "$backend/api/clients"
$knownBefore = Invoke-CurlJson -Url "$backend/api/known-nodes"
$simulate = Invoke-CurlJson -Url "$base245/api/debug/simulate-offline" -Method 'POST' -Body 'duration_ms=5000' -Headers @{ 'X-Auth-Token' = $token245 }

$timeline = @()
for ($i = 0; $i -lt 8; $i++) {
  Start-Sleep -Seconds 1

  $status245Loop = $null
  $status245Up = $true
  try {
    $status245Loop = Invoke-CurlJson -Url "$base245/api/status"
  } catch {
    $status245Up = $false
  }

  $nodes157Loop = Invoke-CurlJson -Url "$base157/api/cluster/nodes"
  $knownLoop = Invoke-CurlJson -Url "$backend/api/known-nodes"
  $clientsLoop = Invoke-CurlJson -Url "$backend/api/clients"
  $seen245 = $nodes157Loop.nodes | Where-Object { $_.node_id -eq 1165657530 } | Select-Object -First 1
  $known245 = $knownLoop.nodes | Where-Object { $_.nodeId -eq 1165657530 } | Select-Object -First 1

  $timeline += [pscustomobject]@{
    second = $i + 1
    node245_up = $status245Up
    node245_paused = if ($status245Up) { $status245Loop.sim.network_paused } else { $null }
    node245_pause_remaining_ms = if ($status245Up) { $status245Loop.sim.network_pause_remaining_ms } else { $null }
    node245_role = if ($status245Up) { $status245Loop.cluster.role } else { $null }
    master_sees_245_online = $seen245.online
    master_sees_245_state = $seen245.device_state
    backend_sees_245_online = $known245.online
    backend_sees_245_state = $known245.deviceStateText
    backend_clients = $clientsLoop.clients.Count
  }
}

$result = [pscustomobject]@{
  login157 = $login157
  login245 = $login245
  status157 = $status157
  status245 = $status245
  nodes157 = $nodes157
  memberSelf = $memberSelf
  masterRemote = $masterRemote
  relay245After = $relay245After
  clientsBefore = $clientsBefore
  knownBefore = $knownBefore
  simulate = $simulate
  timeline = $timeline
}

$result | ConvertTo-Json -Depth 6
