# Installs the C2 prototype camera on this PC. Run from an ELEVATED PowerShell (HKLM registration needs admin):
#   powershell -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\setup.ps1
# Expects the built binaries in C:\Users\Alex\camera-c2\bin. Undo with cleanup.ps1.
$ErrorActionPreference = "Stop"
$bin = "C:\Users\Alex\camera-c2\bin"
$dir = "C:\ProgramData\BeamerCamC2"
New-Item -ItemType Directory -Force "$dir\bin" | Out-Null
# LocalService (Frame Server) must read the DLL and write the log; the user profile is not readable by it.
icacls $dir /inheritance:r /grant "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-19:(OI)(CI)M" "*S-1-5-4:(OI)(CI)M" "*S-1-5-32-545:(OI)(CI)RX" | Out-Null
Copy-Item "$bin\BeamerCamC2.dll" "$dir\bin\" -Force
& "$bin\vcamctl.exe" regcom hklm "$dir\bin\BeamerCamC2.dll"
& "$bin\vcamctl.exe" create system user --hold 0    # System lifetime, current user: persists after this exits
& "$bin\camprobe.exe" list
