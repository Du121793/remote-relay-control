param(
  [string]$ClusterId = "default",
  [string]$BaseUrl = "http://127.0.0.1:4300"
)

$ErrorActionPreference = "Stop"

Write-Host "[apply-cloud] cluster=$ClusterId base=$BaseUrl"

$summary = curl.exe -s "$BaseUrl/api/summary" | ConvertFrom-Json
Write-Host ("[apply-cloud] before activeClients={0} knownNodes={1} cloudDevices={2} cloudClusters={3}" -f `
  $summary.activeClients, $summary.knownNodes, $summary.cloudDevices, $summary.cloudClusters)

$result = curl.exe -s -X POST "$BaseUrl/api/cloud/clusters/$ClusterId/apply-online" | ConvertFrom-Json
$result | ConvertTo-Json -Depth 8

$nodes = curl.exe -s "$BaseUrl/api/known-nodes" | ConvertFrom-Json
$clusters = curl.exe -s "$BaseUrl/api/cloud/clusters" | ConvertFrom-Json
$devices = curl.exe -s "$BaseUrl/api/cloud/devices" | ConvertFrom-Json

Write-Host "[apply-cloud] known nodes:"
$nodes.nodes | Select-Object nodeId,device,cloudClusterId,cloudDeviceId,cloudKeyId,ip,online,lastSeenAt | Format-Table -AutoSize

Write-Host "[apply-cloud] cloud cluster:"
$clusters.clusters | Where-Object { $_.clusterId -eq $ClusterId } | Select-Object clusterId,keyId,enabled,knownDevices,onlineDevices,caFingerprint,lastMasterNodeId,lastMasterDeviceId | Format-Table -AutoSize

Write-Host "[apply-cloud] cloud devices:"
$devices.devices | Where-Object { $_.clusterId -eq $ClusterId } | Select-Object clusterId,deviceId,keyId,nodeId,device,ip,online,lastSeenAt | Format-Table -AutoSize
