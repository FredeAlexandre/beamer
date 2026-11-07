# Functional checks of every command, run from dev\audio\proto\win. Anything it mutes it restores.
param([string]$Only = "all")
$ErrorActionPreference = "Continue"
$exe = "build\beamer-audio.exe"
$g733 = (& cmd /c "$exe list --json" | ConvertFrom-Json) | Where-Object { $_.flow -eq "render" -and $_.default_console }
$rid = $g733.id
function Tool([string]$cmdline) { & cmd /c "$exe $cmdline" }
function Section([string]$s) { "`n=== $s" }
function EndpointState() {
    $d = (& cmd /c "$exe list --json" | ConvertFrom-Json) | Where-Object { $_.id -eq $rid }
    "mute=$($d.mute) volume=$($d.volume)"
}
function Bg([string]$cmdline) { Start-Process -FilePath cmd -ArgumentList "/c $cmdline" -NoNewWindow -PassThru }

Section "glitch detector negative control (expect holes > 0)"
Tool "measure glitch --pad-periods 2 --seconds 21 --stall-every-s 5"

Section "gen | play, capture-system stereo through own fold-down (expect tone -20 dBFS on both channels)"
$p = Bg "$exe measure gen --seconds 6 --amp 0.1 --channels 2 | $exe play --device G733 2> build\play.err"
Start-Sleep -Milliseconds 800
& cmd /c "$exe capture-system --seconds 3 2>nul | $exe measure pcm-stats --channels 2 --label capture_system_48k_stereo"
$p.WaitForExit(); Get-Content build\play.err | Select-Object -Last 1

Section "capture-system at 16 kHz mono through WASAPI SRC (expect tone -20 dBFS)"
$p = Bg "$exe measure gen --seconds 5 --amp 0.1 | $exe play 2>nul"
Start-Sleep -Milliseconds 800
& cmd /c "$exe capture-system --seconds 3 --rate 16000 --channels 1 2>nul | $exe measure pcm-stats --rate 16000 --channels 1 --label capture_system_16k_mono"
$p.WaitForExit()

Section "play 44.1 kHz mono: gen is unpaced so elapsed must match duration (backpressure)"
$p = Bg "$exe measure gen --seconds 5 --amp 0.1 --rate 44100 --channels 1 | $exe play --rate 44100 --channels 1 2> build\play.err"
Start-Sleep -Milliseconds 800
& cmd /c "$exe capture-system --seconds 3 2>nul | $exe measure pcm-stats --channels 2 --label loopback_of_44k_mono_play"
$p.WaitForExit(); Get-Content build\play.err

Section "capture-mic: formats"
foreach ($f in @(@(48000,1), @(48000,2), @(44100,1), @(16000,1))) {
    & cmd /c "$exe capture-mic --seconds 3 --rate $($f[0]) --channels $($f[1]) 2>nul | $exe measure pcm-stats --rate $($f[0]) --channels $($f[1]) --label mic_$($f[0])_$($f[1])"
}

Section "capture-system --process-loopback exclude (self): a foreign tone must come through"
$p = Bg "$exe measure gen --seconds 5 --amp 0.1 | $exe play 2>nul"
Start-Sleep -Milliseconds 800
& cmd /c "$exe capture-system --seconds 3 --process-loopback exclude --no-keepalive 2>nul | $exe measure pcm-stats --channels 2 --label process_loopback_exclude_self"
$p.WaitForExit()

Section "--mute-local: signal stays, endpoint muted during, restored after"
"before: $(EndpointState)"
$p = Bg "$exe measure gen --seconds 7 --amp 0.1 | $exe play 2>nul"
Start-Sleep -Milliseconds 600
$c = Bg "$exe capture-system --seconds 4 --mute-local 2>build\mute.err | $exe measure pcm-stats --channels 2 --label mute_local > build\mute.out"
Start-Sleep -Seconds 2
"during: $(EndpointState)"
$c.WaitForExit(); $p.WaitForExit()
Get-Content build\mute.out; Get-Content build\mute.err | Select-Object -Last 2
"after: $(EndpointState)"

Section "--mute-local, process killed with taskkill /F: guard process must restore"
"before: $(EndpointState)"
$c = Bg "$exe capture-system --seconds 60 --mute-local > build\kill.pcm 2>nul"
Start-Sleep -Seconds 2
"during: $(EndpointState)"
$victim = Get-CimInstance Win32_Process -Filter "Name='beamer-audio.exe'" | Where-Object { $_.CommandLine -match "capture-system" }
foreach ($v in $victim) { Stop-Process -Id $v.ProcessId -Force }
Start-Sleep -Seconds 1
"1 s after kill: $(EndpointState)"
"journal left: $(Test-Path "$env:TEMP\beamer-audio-endpoint-journal.txt")"

Section "--mute-local, capture and guard both killed: restore-endpoint must recover"
try {
    "before: $(EndpointState)"
    $c = Bg "$exe capture-system --seconds 60 --mute-local > build\kill.pcm 2>nul"
    Start-Sleep -Seconds 2
    "during: $(EndpointState)"
    $victims = Get-CimInstance Win32_Process -Filter "Name='beamer-audio.exe'" | Where-Object { $_.CommandLine -match "_guard|capture-system" }
    foreach ($v in ($victims | Sort-Object { $_.CommandLine -notmatch "_guard" })) { Stop-Process -Id $v.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 1
    "both dead: $(EndpointState)  journal left: $(Test-Path "$env:TEMP\beamer-audio-endpoint-journal.txt")"
} finally {
    Tool "restore-endpoint"
}
"after restore-endpoint: $(EndpointState)"

Section "default-device change (communications role, render), seen by the watch log"
$watchBefore = (Get-Content results\watch.jsonl | Measure-Object -Line).Lines
Tool "measure default-swap --to `"$rid`""
Start-Sleep -Seconds 1
Get-Content results\watch.jsonl | Select-Object -Skip $watchBefore | Where-Object { $_ -match "default_changed" }

Section "CPU (cycles): 30 s each, % of one logical core"
& cmd /c "$exe capture-mic --seconds 30 2>&1 >nul" | Select-Object -Last 1
& cmd /c "$exe capture-system --seconds 30 2>&1 >nul" | Select-Object -Last 1
& cmd /c "$exe measure gen --seconds 30 --amp 0.05 | $exe play 2>&1" | Select-Object -Last 1

Section "latency through the stdio tools"
Tool "measure latency-stdio --clicks 60 --pad-periods 2 2>nul"
Tool "measure latency-stdio --clicks 60 --pad-periods 1 2>nul"
Tool "measure latency-stdio --clicks 60 --pad-periods 1 --queue-ms 15 2>nul"
