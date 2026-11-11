# C4: runs web/gum4.html in headless Chrome (throwaway profile, auto-granted camera) until C4DONE; the console
# lines land in run\<Tag>.chrome.log. Run in Alex's session (insession.ps1).
param([string]$Query = "mode=latency&w=1280&h=720&secs=30", [string]$Tag = "b", [int]$MaxSec = 700)
$exe = "C:\Program Files\Google\Chrome\Application\chrome.exe"
$prof = "C:\Users\Alex\camera-c4\run\profile-$Tag"
Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
$log = "C:\Users\Alex\camera-c4\run\$Tag.chrome.log"
Remove-Item $log -ErrorAction SilentlyContinue
$url = "file:///C:/Users/Alex/camera-c4/web/gum4.html?$Query"
$p = Start-Process -PassThru -FilePath $exe -ArgumentList "--headless=new", "--user-data-dir=$prof", "--use-fake-ui-for-media-stream", "--no-first-run", "--no-default-browser-check", "--disable-extensions", "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--enable-logging=stderr", "--v=0", "--remote-debugging-port=0", $url -RedirectStandardError $log
"chrome pid $($p.Id)"
$deadline = (Get-Date).AddSeconds($MaxSec)
while ((Get-Date) -lt $deadline) { Start-Sleep 1; if (Select-String -Path $log -Pattern "C4DONE" -Quiet) { break } }
Start-Sleep 1
Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | ? { $_.CommandLine -like "*$prof*" } | % { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 1
Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
"done"
