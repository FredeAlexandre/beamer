# SPDX-License-Identifier: GPL-3.0-only
# 20 quick opens/closes (separate camprobe processes, 0.5 s each) while a feeder watches the header (both from ssh)
$B = 'C:\Users\Alex\camera-cw5\bin'; $R = 'C:\Users\Alex\camera-cw5\run'
function FsPid { (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId }
$f = Start-Process -PassThru -NoNewWindow -FilePath "$B\feeder.exe" -ArgumentList 'run', '--follow', '--seconds', '60' -RedirectStandardOutput "$R\storm_feeder.out"
Start-Sleep 1; $fs0 = FsPid
$ok = 0
for ($i = 0; $i -lt 20; $i++) { $o = & "$B\camprobe.exe" read --seconds 0.5 --tag "s$i"; if ($o -match '"ok":true') { $ok++ } }
Start-Sleep 2; Stop-Process -Id $f.Id -Force; Start-Sleep 1
$d = Get-Content "$R\storm_feeder.out" | Where-Object { $_ -like '*"demand"*' } | ForEach-Object { $_ | ConvertFrom-Json }
$seqs = @($d | ForEach-Object state_seq)
$mono = $true; for ($i = 1; $i -lt $seqs.Count; $i++) { if ($seqs[$i] -le $seqs[$i - 1]) { $mono = $false } }
"opens ok: $ok / 20; demand events seen: $($d.Count) (streaming:true $(@($d | ? streaming).Count), streaming:false $(@($d | ? { -not $_.streaming }).Count)); state_seq $($seqs[0])..$($seqs[-1]) strictly increasing: $mono; last streaming: $($d[-1].streaming)"
"frameserver pid before $fs0 after $(FsPid); service $((Get-Service FrameServer).Status)"
