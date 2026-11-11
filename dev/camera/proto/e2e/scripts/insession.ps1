# C4: runs a command line in Alex's interactive session through a temporary scheduled task (C2's in-session.ps1).
#   powershell -File insession.ps1 -Command "<powershell>" -Name C4-x [-TimeoutSec 120] [-NoWait]
# With -NoWait the task stays registered; it is removed by the next call with the same -Name or by cleanup.ps1.
param([Parameter(Mandatory)][string]$Command, [int]$TimeoutSec = 120, [switch]$NoWait, [string]$Name = "C4-insession")
$dir = "C:\Users\Alex\camera-c4\run"
New-Item -ItemType Directory -Force $dir | Out-Null
Get-ScheduledTask -TaskName $Name -ErrorAction SilentlyContinue | % { Stop-ScheduledTask -TaskName $Name; Unregister-ScheduledTask -TaskName $Name -Confirm:$false }
$out = Join-Path $dir "$Name.out"
$ps1 = Join-Path $dir "$Name.ps1"
Set-Content -Encoding UTF8 $ps1 "Set-Location C:\Users\Alex\camera-c4`r`n& { $Command } *> '$out'`r`n'__EXIT__' + `$LASTEXITCODE | Add-Content '$out'"
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ps1`""
$principal = New-ScheduledTaskPrincipal -UserId "Alex" -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
Register-ScheduledTask -TaskName $Name -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $Name
if ($NoWait) { "started $Name"; return }
$deadline = (Get-Date).AddSeconds($TimeoutSec)
Start-Sleep -Milliseconds 500
while ((Get-ScheduledTask -TaskName $Name).State -eq "Running" -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
if ((Get-ScheduledTask -TaskName $Name).State -eq "Running") { "TIMEOUT"; Stop-ScheduledTask -TaskName $Name }
Unregister-ScheduledTask -TaskName $Name -Confirm:$false
if (Test-Path $out) { Get-Content $out }
