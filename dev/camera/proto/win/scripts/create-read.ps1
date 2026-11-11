# Creates the virtual camera from this process's session, then enumerates and reads it.
param([string]$Lifetime = "session", [string]$Access = "user", [int]$Hold = 25, [int]$ReadSeconds = 4, [string]$Out = "vc.out")
Set-Location C:\Users\Alex\camera-c2\bin
$p = Start-Process -PassThru -NoNewWindow -FilePath .\vcamctl.exe -ArgumentList "create $Lifetime $Access --hold $Hold" -RedirectStandardOutput $Out
Start-Sleep 3
Get-Content $Out
.\camprobe.exe list
if ($ReadSeconds -gt 0) { .\camprobe.exe read --seconds $ReadSeconds }
