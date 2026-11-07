# Second batch, run from dev\audio\proto\win once the drift run is over.
$exe = "build\beamer-audio.exe"
function Section([string]$s) { "`n=== $s" }
function Bg([string]$cmdline) { Start-Process -FilePath cmd -ArgumentList "/c $cmdline" -NoNewWindow -PassThru }

Section "periods, event cadence, packet age"
& cmd /c "$exe measure periods"

Section "in-process latency, render write to loopback"
& cmd /c "$exe measure latency --clicks 60 --pad-periods 2" | Select-Object -Last 1
& cmd /c "$exe measure latency --clicks 60 --pad-periods 1" | Select-Object -Last 1

Section "glitch test idle, 60 s"
& cmd /c "$exe measure glitch --pad-periods 2 --seconds 60"
& cmd /c "$exe measure glitch --pad-periods 1 --seconds 60"

Section "glitch test with every logical core spinning, 60 s each"
foreach ($pad in 2, 1) {
    $b = Bg "$exe measure burn --seconds 62"
    & cmd /c "$exe measure glitch --pad-periods $pad --seconds 60"
    $b.WaitForExit()
}

Section "keepalive: frames in 3 s from an endpoint nothing renders to (DualSense)"
"without keepalive:"
& cmd /c "$exe capture-system --device DualSense --seconds 3 --no-keepalive 2>nul | $exe measure pcm-stats --skip-s 0 --channels 2 --label no_keepalive"
"with keepalive:"
& cmd /c "$exe capture-system --device DualSense --seconds 3 2>nul | $exe measure pcm-stats --skip-s 0 --channels 2 --label keepalive"

Section "capture-mic | play, 5 s, all through pipes"
& cmd /c "$exe capture-mic --seconds 5 --channels 1 2>nul | $exe play --channels 1 --live-drop-ms 40 2>&1" | Select-Object -Last 1

Section "bad device spec"
& cmd /c "$exe play --device NoSuchDevice < NUL 2>&1"
"exit $LASTEXITCODE"
