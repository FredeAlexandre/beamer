# Runs gum.html in headless Chrome or Edge (temporary profile, fake permission UI, real devices) and prints the C2RESULT line.
param([string]$Browser = "chrome", [int]$W = 1280, [int]$H = 720, [int]$Secs = 10)
$exe = @{ chrome = "C:\Program Files\Google\Chrome\Application\chrome.exe"; edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" }[$Browser]
$prof = "C:\Users\Alex\camera-c2\run\profile-$Browser"
Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
$log = "C:\Users\Alex\camera-c2\run\$Browser.log"
Remove-Item $log -ErrorAction SilentlyContinue
$url = "file:///C:/Users/Alex/camera-c2/web/gum.html?w=$W&h=$H&secs=$Secs"
$p = Start-Process -PassThru -FilePath $exe -ArgumentList "--headless=new", "--user-data-dir=$prof", "--use-fake-ui-for-media-stream", "--no-first-run", "--no-default-browser-check", "--disable-extensions", "--enable-logging=stderr", "--v=0", "--remote-debugging-port=0", $url -RedirectStandardError $log
$deadline = (Get-Date).AddSeconds($Secs + 25)
while ((Get-Date) -lt $deadline) { Start-Sleep 1; if (Select-String -Path $log -Pattern "C2RESULT" -Quiet) { break } }
Start-Sleep 1
Get-CimInstance Win32_Process -Filter "Name='$(Split-Path $exe -Leaf)'" | ? { $_.CommandLine -like "*$prof*" } | % { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
$m = Select-String -Path $log -Pattern "C2RESULT (.*)`"" | Select -First 1
if ($m) { $m.Matches[0].Groups[1].Value -replace '\\"', '"' } else { "NO RESULT"; Get-Content $log -Tail 15 }
Start-Sleep 1
Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
