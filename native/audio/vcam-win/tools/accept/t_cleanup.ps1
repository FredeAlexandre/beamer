# SPDX-License-Identifier: GPL-3.0-only
. C:\Users\Alex\camera-cw5\scripts\common.ps1
KillTools
"== remove the camera (unelevated, as its creator)"
InS CW5-vcam "$B\feeder.exe vcam remove"
"== unregister and delete (elevated)"
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-cw5\src\tools\devuninstall.ps1
Get-ScheduledTask CW5-* -ErrorAction SilentlyContinue | ForEach-Object { Unregister-ScheduledTask -TaskName $_.TaskName -Confirm:$false }
Remove-Item -Recurse -Force "$R\profile-*" -ErrorAction SilentlyContinue
"== verify"
"camera devices (any state): " + ((Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'SWD\VCAMDEVAPI*' -or $_.FriendlyName -like '*Beamer Camera*' } | ForEach-Object { "$($_.Status) $($_.InstanceId) $($_.FriendlyName)" }) -join '; ')
"HKLM CLSID: " + (Test-Path 'HKLM:\Software\Classes\CLSID\{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}')
"HKCU CLSID: " + (Test-Path 'HKCU:\Software\Classes\CLSID\{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}')
"HKLM\SOFTWARE\Beamer: " + (Test-Path 'HKLM:\SOFTWARE\Beamer')
"ProgramFiles\Beamer: " + (Test-Path "$env:ProgramFiles\Beamer") + "; ProgramData\Beamer: " + (Test-Path "$env:ProgramData\Beamer") + "; ProgramData\BeamerCam*: " + (@(Get-ChildItem "$env:ProgramData\BeamerCam*" -ErrorAction SilentlyContinue).Count)
"CW5 tasks: " + (@(Get-ScheduledTask CW5-* -ErrorAction SilentlyContinue).Count) + "; tool processes: " + (@(Get-Process feeder, camprobe, shmfuzz -ErrorAction SilentlyContinue).Count)
"FrameServer: " + (Get-Service FrameServer).Status
"camprobe list: " + (& "$B\camprobe.exe" list)
"Beamer tasks untouched: " + ((Get-ScheduledTask Beamer* | ForEach-Object { "$($_.TaskName)=$($_.State)" }) -join ', ')
