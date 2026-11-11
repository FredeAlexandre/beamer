# Feeder in the session + browsers (in-session) at 720p and 1080p.
param([string[]]$Browsers = @("chrome", "edge"))
$r = powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -NoWait -Name C2-feeder -Command "C:\Users\Alex\camera-c2\bin\feeder.exe pattern --follow --seconds 150"
Start-Sleep 2
foreach ($b in $Browsers) { foreach ($s in @(@(1280, 720), @(1920, 1080))) {
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -TimeoutSec 60 -Command "& C:\Users\Alex\camera-c2\browser.ps1 -Browser $b -W $($s[0]) -H $($s[1])"
} }
Get-Content (($r -split "output ")[1]) | Select-String '"stats"|"streaming":true'
Get-ScheduledTask C2-feeder* | Stop-ScheduledTask; Get-ScheduledTask C2-feeder* | Unregister-ScheduledTask -Confirm:$false
