# SPDX-License-Identifier: GPL-3.0-only
# (a) built-in image after the beat stops (feeder --beat-stop-at), (b) waiting -> slot 3 (feeder --waiting-at); N runs each
param([int]$N = 5)
. C:\Users\Alex\camera-cw5\scripts\common.ps1
function Run([string]$fopt, [string]$want) {
  InS CW5-feeder "$B\feeder.exe run --follow --seconds 14 $fopt" -NoWait; Start-Sleep 1
  $p = InS CW5-probe "$B\camprobe.exe read --seconds 9 --size 1280x720 --events --tag t" -Timeout 60
  Start-Sleep 1
  $f = EndS CW5-feeder
  $ev = $p | Where-Object { $_ -like '{*' } | ForEach-Object { $_ | ConvertFrom-Json }
  $fe = $f | Where-Object { $_ -like '{*' } | ForEach-Object { $_ | ConvertFrom-Json }
  $sum = $ev | Where-Object { $_.ok -ne $null } | Select-Object -First 1
  if ($want -eq 'builtin') {
    $stop = $fe | Where-Object event -eq 'beat_stop' | Select-Object -First 1
    $first = $ev | Where-Object { $_.event -eq 'class' -and $_.class -eq 'builtin' -and $_.t -gt $stop.t } | Select-Object -First 1
    $lastHelper = $ev | Where-Object { $_.event -eq 'class' -and $_.class -eq 'helper' } | Select-Object -Last 1
    '{0}: builtin {1:N1} ms after the last beat ({2:N1} after beat_stop); frames {3} helper {4} builtin {5} other {6}' -f $want, (($first.t - $stop.last_beat_t) / 1000), (($first.t - $stop.t) / 1000), $sum.frames, $sum.helper, $sum.builtin, $sum.other
  } else {
    $m = $fe | Where-Object { $_.event -eq 'mode' } | Select-Object -First 1
    $first = $ev | Where-Object { $_.event -eq 'class' -and $_.class -eq 'slot3' } | Select-Object -First 1
    '{0}: slot 3 {1:N2} ms after mode=waiting; frames {2} helper {3} slot3 {4} other {5}; slot3 latency p50 {6} p95 {7}' -f $want, (($first.t - $m.t) / 1000), $sum.frames, $sum.helper, $sum.slot3, $sum.other, $sum.slot3_lat_ms_p50, $sum.slot3_lat_ms_p95
  }
}
for ($i = 0; $i -lt $N; $i++) { Run '--beat-stop-at 4' 'builtin' }
for ($i = 0; $i -lt $N; $i++) { Run '--waiting-at 4' 'slot3' }
