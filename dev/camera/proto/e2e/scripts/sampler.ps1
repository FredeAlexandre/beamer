# C4: samples CPU (seconds) and working set of camrecv, the Frame Server svchost and the C4 Chrome processes every
# $Every seconds into a CSV. Runs elevated over ssh (session 0 can read session-1 processes).
param([int]$Seconds = 600, [int]$Every = 10, [string]$Out = "C:\Users\Alex\camera-c4\run\sampler.csv")
"t,name,pid,cpu_s,ws_mb,priv_mb" | Set-Content $Out
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
  $t = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $fs = (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId
  $procs = New-Object System.Collections.ArrayList
  foreach ($p in @(Get-Process camrecv -ErrorAction SilentlyContinue)) { [void]$procs.Add([pscustomobject]@{ n = "camrecv"; p = $p }) }
  if ($fs) { foreach ($p in @(Get-Process -Id $fs -ErrorAction SilentlyContinue)) { [void]$procs.Add([pscustomobject]@{ n = "frameserver"; p = $p }) } }
  $cids = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | ? { $_.CommandLine -like "*camera-c4*" } | % ProcessId
  foreach ($id in $cids) { foreach ($p in @(Get-Process -Id $id -ErrorAction SilentlyContinue)) { [void]$procs.Add([pscustomobject]@{ n = "chrome"; p = $p }) } }
  foreach ($x in $procs) { "$t,$($x.n),$($x.p.Id),$([math]::Round($x.p.CPU,3)),$([math]::Round($x.p.WorkingSet64/1MB,1)),$([math]::Round($x.p.PrivateMemorySize64/1MB,1))" | Add-Content $Out }
  Start-Sleep $Every
}
