# SPDX-License-Identifier: GPL-3.0-only
param([string]$Browser = 'chrome', [int]$W = 1280, [int]$H = 720, [int]$Secs = 10)
$exe = @{ chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe'; edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe' }[$Browser]
$R = 'C:\Users\Alex\camera-cw5\run'; $prof = "$R\profile-$Browser"; $log = "$R\$Browser.log"
Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue; Remove-Item $log -ErrorAction SilentlyContinue
$url = "file:///C:/Users/Alex/camera-cw5/web/gum.html?w=$W&h=$H&secs=$Secs"
$p = Start-Process -PassThru -FilePath $exe -ArgumentList '--headless=new', "--user-data-dir=$prof", '--use-fake-ui-for-media-stream', '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--enable-logging=stderr', '--v=0', '--remote-debugging-port=0', $url -RedirectStandardError $log
$deadline = (Get-Date).AddSeconds($Secs + 25)
while ((Get-Date) -lt $deadline) { Start-Sleep 1; if (Select-String -Path $log -Pattern 'C2RESULT' -Quiet) { break } }
Start-Sleep 1
Get-CimInstance Win32_Process -Filter "Name='$(Split-Path $exe -Leaf)'" | ? { $_.CommandLine -like "*$prof*" } | % { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
$m = Select-String -Path $log -Pattern 'C2RESULT (.*)"' | Select -First 1
if ($m) { "$Browser ${W}x${H}: " + ($m.Matches[0].Groups[1].Value -replace '\\"', '"') } else { "$Browser NO RESULT"; Get-Content $log -Tail 10 }
Start-Sleep 1; Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
