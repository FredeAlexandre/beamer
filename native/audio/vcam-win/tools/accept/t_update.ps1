# SPDX-License-Identifier: GPL-3.0-only
# DLL update: re-point the CLSID to a new version folder and watch which DLL Frame Server runs
param([int]$IdleMin = 15)
. C:\Users\Alex\camera-cw5\scripts\common.ps1
function FsPid { (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId }
function Loaded { $p = FsPid; if (-not $p) { 'FrameServer not running' } else { "pid ${p}: " + ((Get-Process -Id $p).Modules | Where-Object ModuleName -eq 'BeamerCameraSource.dll' | ForEach-Object FileName) -join ' ' } }
function Open([string]$tag) { InS CW5-probe "$B\camprobe.exe read --seconds 3 --size 1280x720 --tag $tag" | Select-String '"ok"' | ForEach-Object { ($_ -split ',"types"')[0] } }
$log = 'C:\ProgramData\Beamer\Camera\source.log'
"$(Get-Date -Format T) start: $(Loaded)"
Open v1; "$(Get-Date -Format T) after an app used v1, app closed: $(Loaded)"
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-cw5\src\tools\devinstall.ps1 -Dll "$B\camera\BeamerCameraSource.dll" -Version 2
"$(Get-Date -Format T) CLSID -> $((Get-ItemProperty 'HKLM:\Software\Classes\CLSID\{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}\InprocServer32').'(default)')"
"$(Get-Date -Format T) right after the re-point: $(Loaded)"
Open after-repoint; "$(Get-Date -Format T) after an app opened it again: $(Loaded)"
"log 'loaded from' lines so far:"; Select-String -Path $log -Pattern 'loaded from' | Select-Object -Last 3 | ForEach-Object Line
for ($i = 1; $i -le $IdleMin * 2; $i++) { Start-Sleep 30; if ($i % 2 -eq 0) { "$(Get-Date -Format T) idle $($i / 2) min: $(Loaded)" } }
Open after-idle; "$(Get-Date -Format T) after idle $IdleMin min + an app open: $(Loaded)"
Select-String -Path $log -Pattern 'loaded from' | Select-Object -Last 2 | ForEach-Object Line
Restart-Service FrameServer -Force; Start-Sleep 1
"$(Get-Date -Format T) after Restart-Service FrameServer: $(Loaded)"
Open after-restart; "$(Get-Date -Format T) after the restart + an app open: $(Loaded)"
Select-String -Path $log -Pattern 'loaded from' | Select-Object -Last 2 | ForEach-Object Line
