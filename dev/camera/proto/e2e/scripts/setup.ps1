# C4: installs C2's prototype virtual camera from C:\Users\Alex\camera-c4\bin. Run ELEVATED (ssh is). Undo: cleanup.ps1.
$ErrorActionPreference = "Stop"
$bin = "C:\Users\Alex\camera-c4\bin"
$dir = "C:\ProgramData\BeamerCamC2"
New-Item -ItemType Directory -Force "$dir\bin" | Out-Null
icacls $dir /inheritance:r /grant "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-19:(OI)(CI)M" "*S-1-5-4:(OI)(CI)M" "*S-1-5-32-545:(OI)(CI)RX" | Out-Null
Copy-Item "$bin\BeamerCamC2.dll" "$dir\bin\" -Force
& "$bin\vcamctl.exe" regcom hklm "$dir\bin\BeamerCamC2.dll"
& "$bin\vcamctl.exe" create system user --hold 0
& "$bin\camprobe.exe" list
