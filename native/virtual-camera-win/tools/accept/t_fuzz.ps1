# SPDX-License-Identifier: GPL-3.0-only
# shmfuzz from Alex's unelevated session at 1 kHz while camprobe (also in the session) reads Beamer Camera
param([int]$Secs = 600)
. C:\Users\Alex\camera-cw5\scripts\common.ps1
function FsPid { (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId }
$t0 = Get-Date
InS CW5-probe "$B\camprobe.exe read --seconds $($Secs + 20) --size 1280x720 --tag fuzz --dump $R\fuzz_frames.txt" -NoWait
Start-Sleep 4
$fs0 = FsPid; $st0 = (Get-Process -Id $fs0).StartTime
"frameserver pid $fs0 started $st0"
InS CW5-fuzz "$B\shmfuzz.exe --seconds $Secs --hz 1000" -NoWait
$changes = 0
$deadline = (Get-Date).AddSeconds($Secs + 40)
while ((Get-Date) -lt $deadline) {
  Start-Sleep 10
  $p = FsPid
  if ($p -ne $fs0) { $changes++; "$(Get-Date -Format o) FrameServer pid changed: $fs0 -> $p" }
  if ((Test-Path "$R\CW5-probe.out") -and (Select-String -Path "$R\CW5-probe.out" -Pattern '__EXIT__' -Quiet)) { break }
}
"fuzz:"; Get-Content "$R\CW5-fuzz.out"
"probe:"; Get-Content "$R\CW5-probe.out"
powershell -NoProfile -ExecutionPolicy Bypass -File $T -Dir $R -Name CW5-fuzz -Remove
powershell -NoProfile -ExecutionPolicy Bypass -File $T -Dir $R -Name CW5-probe -Remove
$fs1 = FsPid
"frameserver pid after: $fs1 (start $((Get-Process -Id $fs1 -ErrorAction SilentlyContinue).StartTime)); pid changes seen: $changes; service: $((Get-Service FrameServer).Status)"
"crash events since start:"
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001, 1002; StartTime = $t0 } -ErrorAction SilentlyContinue | ForEach-Object { $_.TimeCreated.ToString('o') + ' ' + ($_.Message -split "`n")[0] }
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 7031, 7034, 7024; StartTime = $t0 } -ErrorAction SilentlyContinue | ForEach-Object { $_.TimeCreated.ToString('o') + ' ' + ($_.Message -split "`n")[0] }
"log tail:"; Get-Content C:\ProgramData\Beamer\Camera\source.log -Tail 6
