#!/bin/bash
# Batch 4: 1080p with C1's real keyframe size (256 KB) through the token bucket.
cd "$(dirname "$0")"
while pgrep -f batch3.sh >/dev/null; do sleep 5; done
./run_pair.sh v1080k 600 "--rate 5e6 --key-every 60 --key-kb 256 --pace-bps 25e6 --burst-pk 32 --seed 9"
