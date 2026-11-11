# SPDX-License-Identifier: GPL-3.0-only
. C:\Users\Alex\camera-cw5\scripts\common.ps1
Stop-Service FrameServer -Force; Start-Sleep 1
InS CW5-squat "$B\feeder.exe squat --open --seconds 20" -NoWait; Start-Sleep 2
InS CW5-feeder "$B\feeder.exe run --follow --seconds 15" -NoWait; Start-Sleep 1
InS CW5-probe "$B\camprobe.exe read --seconds 4 --size 1280x720 --tag squat-open" | Select-String '"ok"'
& "$B\feeder.exe" inspect
Get-Content C:\ProgramData\Beamer\Camera\source.log -Tail 6 | Select-String 'squat|refus'
EndS CW5-feeder | Select-String 'section_open'
EndS CW5-squat
