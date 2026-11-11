# SPDX-License-Identifier: GPL-3.0-only
# How long Frame Server stays after the last app closes the camera (3 runs without a helper, 2 with the feeder holding the section)
. C:\Users\Alex\camera-cw5\scripts\common.ps1
function FsPid { (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId }
function Loaded { $p = FsPid; if ($p) { ((Get-Process -Id $p).Modules | Where-Object ModuleName -eq 'BeamerCameraSource.dll' | ForEach-Object FileName) -join ' ' } else { '-' } }
foreach ($withHelper in $false, $false, $false, $true, $true) {
  if ($withHelper) { InS CW5-feeder "$B\feeder.exe run --follow --seconds 200" -NoWait; Start-Sleep 1 }
  $o = & "$B\camprobe.exe" read --seconds 2 --tag idle
  $closed = Get-Date; $dll = Loaded; $pid0 = FsPid
  $gone = $null
  while (((Get-Date) - $closed).TotalSeconds -lt 180) { Start-Sleep -Milliseconds 500; if (-not (FsPid)) { $gone = ((Get-Date) - $closed).TotalSeconds; break } }
  "helper holding the section: $withHelper; FrameServer pid $pid0 ran $dll; stopped $(if ($gone) { '{0:N1} s' -f $gone } else { 'not within 180 s' }) after the app closed"
  if ($withHelper) { EndS CW5-feeder | Select-String 'section_open' | Select -First 1 }
}
