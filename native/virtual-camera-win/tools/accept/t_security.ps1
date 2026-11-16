# SPDX-License-Identifier: GPL-3.0-only
# DACLs, squatted .Frame, inert load in a user process, MFCreateVirtualCamera validation leaves no log
. C:\Users\Alex\camera-cw5\scripts\common.ps1
$log = 'C:\ProgramData\Beamer\Camera\source.log'
$dll = 'C:\Program Files\Beamer\Camera\1\BeamerCameraSource.dll'
function FsPid { (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId }
"== 1. DACLs while streaming (feeder created .Demand)"
InS CW5-feeder "$B\feeder.exe run --follow --seconds 30" -NoWait; Start-Sleep 1
InS CW5-probe "$B\camprobe.exe read --seconds 8 --size 1280x720 --tag dacl" -NoWait; Start-Sleep 4
& "$B\feeder.exe" inspect
Start-Sleep 6; EndS CW5-probe | Select-String '"ok"'; EndS CW5-feeder | Out-Null
"== 2. foreign pre-created .Frame (FrameServer stopped first: no source object, no objects)"
Stop-Service FrameServer -Force; Start-Sleep 1
& "$B\feeder.exe" inspect
InS CW5-squat "$B\feeder.exe squat --seconds 25" -NoWait; Start-Sleep 2
InS CW5-feeder "$B\feeder.exe run --follow --seconds 20" -NoWait; Start-Sleep 1
InS CW5-probe "$B\camprobe.exe read --seconds 5 --size 1280x720 --tag squatted" | Select-String '"ok"'
& "$B\feeder.exe" inspect
Get-Content $log -Tail 30 | Select-String 'squatted|refusing'
EndS CW5-feeder | Select-String 'section_open|demand_event'
EndS CW5-squat
"== recovery after the squatter is gone"
Stop-Service FrameServer -Force; Start-Sleep 1
InS CW5-feeder "$B\feeder.exe run --follow --seconds 20" -NoWait; Start-Sleep 1
InS CW5-probe "$B\camprobe.exe read --seconds 4 --size 1280x720 --tag recovered" | Select-String '"ok"'
EndS CW5-feeder | Out-Null
"== 3. inert load in a user process (FrameServer stopped: no objects to confuse)"
Stop-Service FrameServer -Force; Start-Sleep 1
InS CW5-load "$B\feeder.exe loadtest '$dll'"
"== 4. MFCreateVirtualCamera validation in a user process: log lines only from Frame Server"
Stop-Service FrameServer -Force; Start-Sleep 1
$n0 = (Get-Content $log).Count
InS CW5-vcam "$B\feeder.exe vcam create"
Start-Sleep 2
$fs = FsPid
$new = Get-Content $log | Select-Object -Skip $n0
"frameserver pid $fs; new log lines $($new.Count):"; $new
$pids = $new | ForEach-Object { if ($_ -match 'pid=(\d+)') { $matches[1] } } | Sort-Object -Unique
"pids in new lines: $($pids -join ',')"
