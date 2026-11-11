# SPDX-License-Identifier: GPL-3.0-only
# 60 s per offered size, feeder (helper stand-in, --follow) -> camprobe, both in Alex's session; FS CPU at 1080p
param([int]$Secs = 60)
. C:\Users\Alex\camera-cw5\scripts\common.ps1
foreach ($s in '1280x720', '1920x1080', '640x360') {
  InS CW5-feeder "$B\feeder.exe run --follow --seconds $($Secs + 20)" -NoWait; Start-Sleep 2
  $cpu = $null
  if ($s -eq '1920x1080') {
    $cpu = Start-Job -ArgumentList $B { param($B) Start-Sleep 10; $fs = (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId; & "$B\feeder.exe" cpu $fs 30 }
  }
  InS CW5-probe "$B\camprobe.exe read --seconds $Secs --size $s --tag $s" -Timeout ($Secs + 60) | Select-String '"ok"'
  if ($cpu) { 'framesrv_cpu ' + (Receive-Job $cpu -Wait) }
  EndS CW5-feeder | Select-String 'stats|section_open|"streaming":true' | Select -First 3
}
