#!/bin/sh
# Local-mute experiment: play a 1 kHz tone with afplay (another process, so it is in the global
# tap) while capturing (a) the process tap and (b) the built-in microphone, which hears the
# speakers acoustically. Run once with the tap unmuted and once with --mute-local.
# Expected if mute works: tap level identical in both runs, microphone tone gone when muted.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/../.build/beamer-audio"
out=${1:-$(mktemp -d)}
mkdir -p "$out"
tone="$out/tone_1k.wav"
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=1000:duration=4:sample_rate=48000" -ac 2 "$tone"

run() {  # name, extra capture-system flags
    name=$1; shift
    "$bin" capture-mic --stats 0 >"$out/mic_$name.f32" 2>"$out/mic_$name.log" &
    mic=$!
    if [ "$name" != baseline ]; then
        "$bin" capture-system --stats 0 "$@" >"$out/sys_$name.f32" 2>"$out/sys_$name.log" &
        sys=$!
    fi
    sleep 2
    if [ "$name" != baseline ]; then afplay "$tone"; else sleep 4; fi
    sleep 0.3
    kill -INT $mic; wait $mic || true
    if [ "$name" != baseline ]; then kill -INT $sys; wait $sys || true; fi
}

run baseline
run unmuted
run muted --mute-local
cd "$out"
uv run -q --with numpy python "$here/tone_level.py" 1000 48000 1 mic_baseline.f32 mic_unmuted.f32 mic_muted.f32
uv run -q --with numpy python "$here/tone_level.py" 1000 48000 2 sys_unmuted.f32 sys_muted.f32
echo "files in $out"
