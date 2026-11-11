# SPDX-License-Identifier: GPL-3.0-only
# Dev stand-in for `vcam-remove` + `vcam-uninstall` (PHASE2 §9.2). ELEVATED. Removes the camera (if -Feeder is
# given: `feeder vcam remove`), the COM class, HKLM\SOFTWARE\Beamer\Camera, %ProgramFiles%\Beamer\Camera and
# %ProgramData%\Beamer\Camera (and their Beamer parents when empty); stops FrameServer first if it has the DLL
# loaded. Prints what is left (expect nothing).
param([string]$Feeder = '')
$clsid = '{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}'
if ($Feeder -and (Test-Path $Feeder)) { & $Feeder vcam remove }
$fs = Get-CimInstance Win32_Service -Filter "Name='FrameServer'"
if ($fs.ProcessId) {
    $loaded = (Get-Process -Id $fs.ProcessId -ErrorAction SilentlyContinue).Modules | Where-Object { $_.ModuleName -eq 'BeamerCameraSource.dll' }
    if ($loaded) { Stop-Service FrameServer -Force; Start-Sleep 1; 'stopped FrameServer (DLL was loaded)' }
}
Remove-Item -Recurse -Force "HKLM:\Software\Classes\CLSID\$clsid" -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force 'HKLM:\SOFTWARE\Beamer\Camera' -ErrorAction SilentlyContinue
foreach ($p in "$env:ProgramFiles\Beamer\Camera", "$env:ProgramData\Beamer\Camera") { Remove-Item -Recurse -Force $p -ErrorAction SilentlyContinue }
foreach ($p in 'HKLM:\SOFTWARE\Beamer', "$env:ProgramFiles\Beamer", "$env:ProgramData\Beamer") {
    if ((Test-Path $p) -and -not (Get-ChildItem $p -Force -ErrorAction SilentlyContinue)) {
        if ($p -like 'HKLM:*' -and (Get-Item $p).Property.Count -gt 0) { continue }
        Remove-Item -Force $p
    }
}
[pscustomobject]@{
    clsid = Test-Path "HKLM:\Software\Classes\CLSID\$clsid"
    key = Test-Path 'HKLM:\SOFTWARE\Beamer\Camera'
    program_files = Test-Path "$env:ProgramFiles\Beamer\Camera"
    program_data = Test-Path "$env:ProgramData\Beamer\Camera"
    vcam_devices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object InstanceId -like 'SWD\VCAMDEVAPI*' | ForEach-Object { "$($_.Status) $($_.FriendlyName)" })
} | ConvertTo-Json -Compress
