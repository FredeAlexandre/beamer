# Opens the Windows Camera app in Alex's session for ~12 s with the feeder running, then closes it.
# Evidence is source-side: demand events (media type the app picked) and samples delivered.
$r = powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -NoWait -Name C2-feeder -Command "C:\Users\Alex\camera-c2\bin\feeder.exe pattern --follow --seconds 30"
Start-Sleep 2
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -TimeoutSec 30 -Command "Start-Process explorer.exe 'microsoft.windows.camera:'"
Start-Sleep 12
Get-Process WindowsCamera -ErrorAction SilentlyContinue | Select Id, SessionId, MainWindowTitle | ft -Auto
Get-Process WindowsCamera -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 16
Get-Content (($r -split "output ")[1])
Get-ScheduledTask C2-feeder* | Unregister-ScheduledTask -Confirm:$false
