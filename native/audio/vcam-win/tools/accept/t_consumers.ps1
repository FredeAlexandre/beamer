# SPDX-License-Identifier: GPL-3.0-only
. C:\Users\Alex\camera-cw5\scripts\common.ps1
EndS CW5-feeder | Out-Null
$sc = 'C:\Users\Alex\camera-cw5\scripts'
foreach ($s in '1280x720', '1920x1080') {
  InS CW5-feeder "$B\feeder.exe run --follow --seconds 40" -NoWait; Start-Sleep 1
  InS CW5-cons "powershell -NoProfile -ExecutionPolicy Bypass -File $sc\cons_dshow.ps1 -Size $s" -Timeout 90
  EndS CW5-feeder | Out-Null
}
foreach ($br in 'chrome', 'edge') { foreach ($wh in @(@(1280, 720), @(1920, 1080))) {
  InS CW5-feeder "$B\feeder.exe run --follow --seconds 50" -NoWait; Start-Sleep 1
  InS CW5-cons "powershell -NoProfile -ExecutionPolicy Bypass -File $sc\cons_browser.ps1 -Browser $br -W $($wh[0]) -H $($wh[1])" -Timeout 90
  EndS CW5-feeder | Out-Null
} }
