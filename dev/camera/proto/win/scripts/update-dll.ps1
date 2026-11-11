# Replaces the media source DLL: stop the (demand-start) Frame Server so it unloads the DLL, copy, done.
Get-Service FrameServer | ? Status -eq Running | Stop-Service -Force
Start-Sleep 1
Copy-Item C:\Users\Alex\camera-c2\src\build\BeamerCamC2.dll C:\ProgramData\BeamerCamC2\bin\ -Force
Copy-Item C:\Users\Alex\camera-c2\src\build\*.exe C:\Users\Alex\camera-c2\bin\ -Force
"dll updated"
