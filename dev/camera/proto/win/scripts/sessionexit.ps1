# Session-lifetime camera created in Alex's session (hold 6 s) while an app streams 12 s from it.
Set-Location C:\Users\Alex\camera-c2\bin
.\vcamctl.exe remove system user
Start-Sleep 1
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -NoWait -Name C2-holder -Command "C:\Users\Alex\camera-c2\bin\vcamctl.exe create session user --hold 6" | Out-Null
Start-Sleep 2
.\camprobe.exe read --seconds 12 --per-second --tag S
Start-Sleep 2
"present after: " + ((.\camprobe.exe list | ConvertFrom-Json).mf.name -join ",")
Get-ScheduledTask C2-holder* | Unregister-ScheduledTask -Confirm:$false
"recreate system/user from the limited session x3 (timing):"
1..3 | % { powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -Command "C:\Users\Alex\camera-c2\bin\vcamctl.exe create system user --hold 0" | Select -First 1 }
