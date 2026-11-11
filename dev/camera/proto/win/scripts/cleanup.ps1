# Removes everything setup.ps1 (or the C2 tests) created. Run from an ELEVATED PowerShell.
$bin = "C:\Users\Alex\camera-c2\bin"
& "$bin\vcamctl.exe" remove system user            # the virtual camera (device node)
& "$bin\vcamctl.exe" remove system all
& "$bin\vcamctl.exe" unregcom hklm                  # HKLM\Software\Classes\CLSID\{EF41A4B9-7102-4C33-A218-3AF46FED9B0D}
& "$bin\vcamctl.exe" unregcom hkcu
Get-ScheduledTask C2-* -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false
Get-Process feeder, camprobe, vcamctl -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Service FrameServer | ? Status -eq Running | Stop-Service -Force   # demand-start; unloads the DLL
Start-Sleep 1
Remove-Item -Recurse -Force C:\ProgramData\BeamerCamC2 -ErrorAction SilentlyContinue
Get-PnpDevice | ? InstanceId -like "SWD\VCAMDEVAPI*"                   # expect nothing
