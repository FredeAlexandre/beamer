#!/bin/bash
# Real-camera measurements (Q1 cold start, Q2 latency/fps, Q4 sharing, Q5 effects). Unattended,
# about 3 minutes; the camera light comes on several times. Needs camera permission for the
# responsible app of the shell that runs it (Terminal, T3 Code, ...): see dev/camera/reports/c1.md.
#   experiments/camera_suite.sh [OUTDIR]      (default out/camera)
set -u
cd "$(dirname "$0")/.."
T=build/beamer-camera
O=${1:-out/camera}
mkdir -p "$O"
[ -x $T ] || ./build.sh
if ! $T auth --request --auth-timeout 20 > "$O/auth.jsonl" 2>&1; then
  cat "$O/auth.jsonl"
  echo "camera not authorized for the responsible app of this shell; nothing measured" >&2
  exit 3
fi
$T list > "$O/list.json"
echo "1/5 cold start (separate processes)"
for i in 1 2 3 4 5; do
  for sz in 1280x720 1920x1080; do
    $T run --source camera --size $sz --seconds 3 --no-encode --stats 0 2> "$O/cold_${sz}_$i.jsonl"
    sleep 3
  done
done
echo "2/5 warm restarts (one process, 5 x start/stop)"
$T run --source camera --seconds 2 --cycles 5 --gap 1 --no-encode --stats 0 2> "$O/warm.jsonl"
echo "3/5 capture + encode: 720p auto rate, 720p pinned 30, 1080p"
RUNNER_EVENTS="$O/cam720.jsonl" python3 experiments/runner.py "$O/runs.jsonl" cam720 -- run --source camera --size 1280x720 \
  --bitrate 2500000 --seconds 20 --trace "$O/cam720_trace.jsonl" --out "$O/cam720.h264" > /dev/null
RUNNER_EVENTS="$O/cam720_pin.jsonl" python3 experiments/runner.py "$O/runs.jsonl" cam720_pin -- run --source camera --size 1280x720 \
  --bitrate 2500000 --seconds 20 --pin-fps > /dev/null
RUNNER_EVENTS="$O/cam1080.jsonl" python3 experiments/runner.py "$O/runs.jsonl" cam1080 -- run --source camera --size 1920x1080 \
  --bitrate 5000000 --seconds 20 --trace "$O/cam1080_trace.jsonl" --out "$O/cam1080.h264" > /dev/null
echo "4/5 sharing: our 720p session, then a second 1080p session of ours, then ffmpeg at 1080p"
$T run --source camera --size 1280x720 --seconds 26 2> "$O/share_main.jsonl" &
MAIN=$!
sleep 5
$T run --source camera --size 1920x1080 --seconds 6 --no-encode 2> "$O/share_second.jsonl"
sleep 3
if command -v ffmpeg > /dev/null; then
  ffmpeg -hide_banner -nostdin -f avfoundation -framerate 30 -video_size 1920x1080 -i "MacBook Air Camera:none" -t 6 -f null - \
    > "$O/share_ffmpeg.txt" 2>&1
fi
wait $MAIN
echo "5/5 video effects state while capturing"
$T run --source camera --seconds 8 --no-encode --effects 2> "$O/effects.jsonl"
python3 experiments/camera_report.py "$O" | tee "$O/report.txt"
