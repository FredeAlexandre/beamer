# SPDX-License-Identifier: GPL-3.0-only
# Runs a PowerShell command in the signed-in user's interactive session (unelevated, like Beamer and its
# helper) through a temporary scheduled task, waits, prints its output, removes the task (C2/C4 method).
# Over ssh (session 0):  powershell -File insession.ps1 -Command "<powershell>" [-Name CW5-x] [-TimeoutSec 120] [-NoWait]
# With -NoWait the task stays registered until the next call with the same -Name or `-Remove`.
param([string]$Command = '', [int]$TimeoutSec = 120, [switch]$NoWait, [switch]$Remove, [string]$Name = 'CW5-insession',
      [string]$Dir = (Join-Path $PSScriptRoot 'run'), [string]$User = '')
if (-not $User) { $User = (Get-CimInstance Win32_ComputerSystem).UserName }   # the console user
if (-not $User) { 'no interactive user signed in (H8)'; exit 3 }
New-Item -ItemType Directory -Force $Dir | Out-Null
Get-ScheduledTask -TaskName $Name -ErrorAction SilentlyContinue | ForEach-Object { Stop-ScheduledTask -TaskName $Name; Unregister-ScheduledTask -TaskName $Name -Confirm:$false }
if ($Remove) { return }
$out = Join-Path $Dir "$Name.out"
$ps1 = Join-Path $Dir "$Name.ps1"
Remove-Item $out -ErrorAction SilentlyContinue
Set-Content -Encoding UTF8 $ps1 "Set-Location '$Dir'`r`n& { $Command } *> '$out'`r`n'__EXIT__ ' + `$LASTEXITCODE | Add-Content '$out'"
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ps1`""
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
Register-ScheduledTask -TaskName $Name -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $Name
if ($NoWait) { "started $Name -> $out"; return }
$deadline = (Get-Date).AddSeconds($TimeoutSec)
Start-Sleep -Milliseconds 400
while ((Get-ScheduledTask -TaskName $Name).State -eq 'Running' -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
if ((Get-ScheduledTask -TaskName $Name).State -eq 'Running') { 'TIMEOUT'; Stop-ScheduledTask -TaskName $Name }
Unregister-ScheduledTask -TaskName $Name -Confirm:$false
if (Test-Path $out) { Get-Content $out }
