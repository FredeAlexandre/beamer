# SPDX-License-Identifier: GPL-3.0-only
$T = 'C:\Users\Alex\camera-cw5\src\tools\insession.ps1'; $R = 'C:\Users\Alex\camera-cw5\run'; $B = 'C:\Users\Alex\camera-cw5\bin'
function KillTools { Get-Process feeder, camprobe, shmfuzz -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$B\*" } | Stop-Process -Force -ErrorAction SilentlyContinue }
function InS([string]$name, [string]$cmd, [switch]$NoWait, [int]$Timeout = 300) {
  if ($NoWait) { powershell -NoProfile -ExecutionPolicy Bypass -File $T -Dir $R -Name $name -NoWait -Command $cmd | Out-Null }
  else { powershell -NoProfile -ExecutionPolicy Bypass -File $T -Dir $R -Name $name -TimeoutSec $Timeout -Command $cmd }
}
# ends a -NoWait task: kills its tool process (stopping the task does not), prints its output
function EndS([string]$name) {
  KillTools; Start-Sleep -Milliseconds 300
  powershell -NoProfile -ExecutionPolicy Bypass -File $T -Dir $R -Name $name -Remove
  if (Test-Path "$R\$name.out") { Get-Content "$R\$name.out" }
}
