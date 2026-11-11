# Helper (feeder, in Alex's session) -> shared memory -> media source (Frame Server) -> camprobe (here).
# Measures demand signal latency, helper->app latency, CPU of the feeder and of the Frame Server svchost.
param([string]$Size = "1280x720", [int]$Seconds = 12, [string]$FeederArgs = "pattern --follow", [string]$Tag = "pipe", [string]$ProbeArgs = "")
Set-Location C:\Users\Alex\camera-c2\bin
$fs = (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId
$feedSecs = $Seconds + 10
$r = powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -NoWait -Name "C2-feeder" -Command "C:\Users\Alex\camera-c2\bin\feeder.exe $FeederArgs --seconds $feedSecs"
$feedOut = ($r -split "output ")[1]
Start-Sleep 3
$cpuJob = Start-Job -ScriptBlock { param($fs, $s) Start-Sleep 3; & C:\Users\Alex\camera-c2\bin\feeder.exe cpu $fs ($s - 5) } -ArgumentList $fs, $Seconds
$probe = Invoke-Expression ".\camprobe.exe read --size $Size --seconds $Seconds --tag $Tag $ProbeArgs"
$cpu = Receive-Job $cpuJob -Wait
Start-Sleep -Seconds ($feedSecs - $Seconds - 1)
$probe
"framserver_cpu: $cpu"
Get-Content $feedOut
Get-ScheduledTask C2-feeder* | Unregister-ScheduledTask -Confirm:$false
