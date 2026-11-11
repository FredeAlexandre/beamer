#!/bin/bash
# Phase-1 C3 batch 2 (after batch1): 1080p burst, then TCP stand-ins for the link fallback.
cd "$(dirname "$0")"
while pgrep -f batch1.sh >/dev/null; do sleep 5; done
./run_pair.sh v1080b 600 "--rate 5e6 --key-every 60 --key-kb 160 --pace 0 --seed 4"
./run_tcp.sh t720 300 "--rate 2.5e6 --key-every 60 --key-kb 80 --pace 0 --seed 5"
./run_tcp.sh t1m 300 "--rate 1e6 --key-every 60 --key-kb 32 --pace 0 --seed 6"
./run_tcp.sh taudio 300 "--no-video"
