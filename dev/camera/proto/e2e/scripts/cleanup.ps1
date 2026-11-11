# C4: removes the prototype camera, the C4 tasks and processes. Run ELEVATED.
$bin = "C:\Users\Alex\camera-c4\bin"
Get-Process camrecv -ErrorAction SilentlyContinue | Stop-Process -Force
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | ? { $_.CommandLine -like "*camera-c4*" } | % { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
& "$bin\vcamctl.exe" remove system user
& "$bin\vcamctl.exe" remove system all
& "$bin\vcamctl.exe" unregcom hklm
& "$bin\vcamctl.exe" unregcom hkcu
Get-ScheduledTask C4-* -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false
Get-Service FrameServer | ? Status -eq Running | Stop-Service -Force
Start-Sleep 1
Remove-Item -Recurse -Force C:\ProgramData\BeamerCamC2 -ErrorAction SilentlyContinue
"devices:"; Get-PnpDevice | ? InstanceId -like "SWD\VCAMDEVAPI*"
"programdata:"; Get-ChildItem C:\ProgramData -Filter BeamerCam* -ErrorAction SilentlyContinue
"tasks:"; Get-ScheduledTask C4-*, C2-* -ErrorAction SilentlyContinue
"clsid:"; Test-Path "Registry::HKLM\Software\Classes\CLSID\{EF41A4B9-7102-4C33-A218-3AF46FED9B0D}"
