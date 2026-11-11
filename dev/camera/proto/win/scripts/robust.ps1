# Q6 robustness. Each case prints its consumer results and the feeder log.
param([string]$Case = "all")
Set-Location C:\Users\Alex\camera-c2\bin
function Feeder([string]$a, [int]$s) { $r = powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\in-session.ps1 -NoWait -Name C2-feeder -Command "C:\Users\Alex\camera-c2\bin\feeder.exe $a --seconds $s"; ($r -split "output ")[1] }
function EndFeeder($out) { Get-Process feeder -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1; Get-Content $out | Select-String '"demand"|"stats"|switch' ; Get-ScheduledTask C2-feeder* | Unregister-ScheduledTask -Confirm:$false }
if ($Case -in "all", "two") {
  "== two consumers: A 720p 12 s, B joins at 3 s keeping the current type, C joins at 5 s asking 1080p"
  $f = Feeder "pattern --follow" 40; Start-Sleep 2
  $a = Start-Job { & C:\Users\Alex\camera-c2\bin\camprobe.exe read --size 1280x720 --seconds 12 --tag A }
  Start-Sleep 3
  $b = Start-Job { & C:\Users\Alex\camera-c2\bin\camprobe.exe read --no-set --seconds 6 --tag B }
  Start-Sleep 2
  $c = Start-Job { & C:\Users\Alex\camera-c2\bin\camprobe.exe read --size 1920x1080 --seconds 4 --tag C }
  Receive-Job $a, $b, $c -Wait
  EndFeeder $f
}
if ($Case -in "all", "kill") {
  "== consumer killed while streaming (A killed at 4 s), then D opens"
  $f = Feeder "pattern --follow" 30; Start-Sleep 2
  $p = Start-Process -PassThru -NoNewWindow -FilePath .\camprobe.exe -ArgumentList "read --seconds 20 --tag A" -RedirectStandardOutput killed.out
  Start-Sleep 4; $tk = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() * 1000; Stop-Process -Id $p.Id -Force; "killed at $tk"
  Start-Sleep 2
  .\camprobe.exe read --seconds 3 --tag D | Select-String '"ok"'
  EndFeeder $f
}
if ($Case -in "all", "producer") {
  "== producer killed at ~4 s, restarted at ~9 s, consumer reads 14 s"
  $f = Feeder "pattern --follow" 30; Start-Sleep 2
  $a = Start-Job { & C:\Users\Alex\camera-c2\bin\camprobe.exe read --seconds 14 --per-second --tag A }
  Start-Sleep 4; $tk = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() * 1000; Get-Process feeder | Stop-Process -Force; "feeder killed at $tk"
  Start-Sleep 5; $tr = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() * 1000; $f2 = Feeder "pattern --follow" 12; "feeder restarted at $tr"
  Receive-Job $a -Wait
  EndFeeder $f2
}
if ($Case -in "all", "switch") {
  "== helper switches 720p <-> 1080p every 3 s while the app reads 720p, then 1080p"
  $f = Feeder "pattern --switch-every 3" 40; Start-Sleep 2
  $fs = (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId
  $cj = Start-Job { param($fs) Start-Sleep 2; & C:\Users\Alex\camera-c2\bin\feeder.exe cpu $fs 8 } -ArgumentList $fs
  .\camprobe.exe read --size 1280x720 --seconds 12 --tag S720 | Select-String '"ok"'
  "fs cpu: " + (Receive-Job $cj -Wait)
  .\camprobe.exe read --size 1920x1080 --seconds 12 --tag S1080 | Select-String '"ok"'
  EndFeeder $f
}
