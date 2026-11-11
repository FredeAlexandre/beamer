#!/bin/bash
# Batch 3: token-bucket pacer (32-packet bursts, then 25 Mbit/s) at 1080p and at 720p with C1's 120 KB keyframes.
cd "$(dirname "$0")"
while pgrep -f batch2.sh >/dev/null; do sleep 5; done
./run_pair.sh v1080h 600 "--rate 5e6 --key-every 60 --key-kb 160 --pace-bps 25e6 --burst-pk 32 --seed 7"
./run_pair.sh v720h 600 "--rate 2.5e6 --key-every 60 --key-kb 120 --pace-bps 25e6 --burst-pk 32 --seed 8"
