#!/bin/bash
# Low light (needs a person): run, and during each 20 s run cover the lens with a finger from
# second 5 to 15 (or switch off the room light). Prints frames per second per run.
set -u
cd "$(dirname "$0")/.."
O=${1:-out/camera}; mkdir -p "$O"
for mode in auto pin; do
  extra=""; [ $mode = pin ] && extra="--pin-fps"
  echo "run $mode: cover the lens from second 5 to 15"
  build/beamer-camera run --source camera --size 1280x720 --seconds 20 --no-encode $extra 2> "$O/lowlight_$mode.jsonl"
done
python3 - "$O" <<'PY'
import json, sys
for mode in ("auto", "pin"):
    ev = [json.loads(l) for l in open(f"{sys.argv[1]}/lowlight_{mode}.jsonl") if l.startswith("{")]
    print(mode, [e["fps_from_pts"] for e in ev if e.get("ev") == "cam_stats"])
PY
