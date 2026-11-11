# C4: samples CPU (seconds) and working set of camrecv, the Frame Server svchost and the C4 Chrome processes every
# $Every seconds into a CSV. Runs elevated over ssh (session 0 can read session-1 processes).
param([int]$Seconds = 600, [int]$Every = 10, [string]$Out = "C:\Users\Alex\camera-c4\run\sampler.csv")
"t,name,pid,cpu_s,ws_mb,priv_mb" | Set-Content $Out
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
  $t = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $fs = (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId
  $procs = @()
  $procs += Get-Process camrecv -ErrorAction SilentlyContinue | % { ,@("camrecv", $_) }
  if ($fs) { $procs += Get-Process -Id $fs -ErrorAction SilentlyContinue | % { ,@("frameserver", $_) } }
  $cids = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | ? { $_.CommandLine -like "*camera-c4*" } | % ProcessId
  foreach ($id in $cids) { $procs += Get-Process -Id $id -ErrorAction SilentlyContinue | % { ,@("chrome", $_) } }
  foreach ($x in $procs) { "$t,$($x[0]),$($x[1].Id),$([math]::Round($x[1].CPU,3)),$([math]::Round($x[1].WorkingSet64/1MB,1)),$([math]::Round($x[1].PrivateMemorySize64/1MB,1))" | Add-Content $Out }
  Start-Sleep $Every
}
