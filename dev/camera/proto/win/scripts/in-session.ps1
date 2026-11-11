# Runs a command line in Alex's interactive session (session 1) through a temporary scheduled task,
# waits for it, prints its output, removes the task. Usage (from SSH):
#   powershell -File in-session.ps1 -Command "<powershell command>" [-TimeoutSec 120] [-NoWait]
param([Parameter(Mandatory)][string]$Command, [int]$TimeoutSec = 120, [switch]$NoWait, [string]$Name = "C2-insession")
$dir = "C:\Users\Alex\camera-c2\run"
New-Item -ItemType Directory -Force $dir | Out-Null
$id = [guid]::NewGuid().ToString("N").Substring(0, 8)
$out = Join-Path $dir "$Name-$id.out"
$ps1 = Join-Path $dir "$Name-$id.ps1"
Set-Content -Encoding UTF8 $ps1 "Set-Location C:\Users\Alex\camera-c2`r`n& { $Command } *> '$out'`r`n'__EXIT__' + `$LASTEXITCODE | Add-Content '$out'"
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ps1`""
$principal = New-ScheduledTaskPrincipal -UserId "Alex" -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
$task = "$Name-$id"
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $task
if ($NoWait) { "started task $task, output $out"; return }
$deadline = (Get-Date).AddSeconds($TimeoutSec)
Start-Sleep -Milliseconds 500
while ((Get-ScheduledTask -TaskName $task).State -eq "Running" -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
if ((Get-ScheduledTask -TaskName $task).State -eq "Running") { "TIMEOUT"; Stop-ScheduledTask -TaskName $task }
Unregister-ScheduledTask -TaskName $task -Confirm:$false
if (Test-Path $out) { Get-Content $out }
Remove-Item $ps1 -ErrorAction SilentlyContinue
