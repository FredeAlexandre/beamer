#!/bin/bash
# Phase-1 C3 batch: 10 min per configuration, sequential (they would disturb each other).
cd "$(dirname "$0")"
./run_pair.sh v720p 600 "--rate 2.5e6 --key-every 60 --key-kb 80 --pace 2.5"
./run_pair.sh v720b 600 "--rate 2.5e6 --key-every 60 --key-kb 80 --pace 0 --seed 2"
./run_pair.sh aonly 600 "--no-video"
./run_pair.sh v1080p 600 "--rate 5e6 --key-every 60 --key-kb 160 --pace 2.5 --seed 3"
