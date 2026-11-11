# SPDX-License-Identifier: GPL-3.0-only
# Dev stand-in for `beamer-audio.exe vcam-install` (PHASE2 §9.2) until CW4's verb exists. ELEVATED (an ssh
# session to the PC is). Copies the DLL to %ProgramFiles%\Beamer\Camera\<Version>\, creates
# %ProgramData%\Beamer\Camera (SYSTEM, LocalService, Administrators write; Users read), registers the COM class
# in HKLM and writes HKLM\SOFTWARE\Beamer\Camera (InstallGuid kept if present, OwnerSid, DllVersion, DllPath).
# The camera itself is then created unelevated by the helper (or `feeder vcam create`).
#   powershell -ExecutionPolicy Bypass -File devinstall.ps1 -Dll <path> [-Version 1] [-OwnerSid S-1-5-21-...]
param([Parameter(Mandatory)][string]$Dll, [string]$Version = '1', [string]$OwnerSid = '')
$ErrorActionPreference = 'Stop'
$clsid = '{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}'
if (-not $OwnerSid) { $OwnerSid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value }
$dir = Join-Path $env:ProgramFiles "Beamer\Camera\$Version"
New-Item -ItemType Directory -Force $dir | Out-Null
$dst = Join-Path $dir 'BeamerCameraSource.dll'
Copy-Item $Dll $dst -Force
$data = Join-Path $env:ProgramData 'Beamer\Camera'
New-Item -ItemType Directory -Force $data | Out-Null
icacls $data /inheritance:r /grant '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-19:(OI)(CI)M' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
$k = "HKLM:\Software\Classes\CLSID\$clsid\InprocServer32"
New-Item -Force $k | Out-Null
Set-ItemProperty -Path $k -Name '(default)' -Value $dst
Set-ItemProperty -Path $k -Name 'ThreadingModel' -Value 'Both'
$b = 'HKLM:\SOFTWARE\Beamer\Camera'
if (-not (Test-Path $b)) { New-Item -Force $b | Out-Null }
$g = (Get-ItemProperty -Path $b -Name InstallGuid -ErrorAction SilentlyContinue).InstallGuid
if (-not $g) { $g = '{' + [guid]::NewGuid().ToString().ToUpper() + '}'; Set-ItemProperty -Path $b -Name InstallGuid -Value $g }
Set-ItemProperty -Path $b -Name OwnerSid -Value $OwnerSid
New-ItemProperty -Path $b -Name DllVersion -Value ([int]$Version) -PropertyType DWord -Force | Out-Null
Set-ItemProperty -Path $b -Name DllPath -Value $dst
[pscustomobject]@{ dll = $dst; install_guid = $g; owner_sid = $OwnerSid; data = $data } | ConvertTo-Json -Compress
