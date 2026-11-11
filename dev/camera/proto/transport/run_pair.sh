#!/bin/bash
# One Mac (send) -> PC (recv) vprobe run.   run_pair.sh NAME DURATION_S "send args" ["recv args"]
# PC side: C:\Users\Alex\camera-c3 (plain folder, own venv). Results: out/NAME.mac.npz, out/NAME.pc.npz
# Ports: PC 24850 (UDP), Mac 40150; outside Beamer's 24823-24830. No firewall rule: the PC punches.
set -u
cd "$(dirname "$0")"
name=$1 dur=$2 sargs=$3 rargs=${4:-}
pc=${PC_HOST:-192.168.1.134} me=${MAC_HOST:-192.168.1.138}
mkdir -p out
scp -q vprobe.py lebig:camera-c3/vprobe.py
ssh lebig "cd C:\\Users\\Alex\\camera-c3; .\\venv\\Scripts\\python.exe vprobe.py recv --port 24850 --punch $me:40150 --duration $((${dur%.*} + 40)) --out $name.pc.npz $rargs" > "out/$name.pc.log" 2>&1 &
spid=$!
sleep 3
uv run -q --python 3.13 --with numpy vprobe.py send --to "$pc:24850" --local-port 40150 --duration "$dur" --out "out/$name.mac.npz" $sargs > "out/$name.mac.log" 2>&1
rc=$?
wait $spid
scp -q "lebig:camera-c3/$name.pc.npz" "out/$name.pc.npz"
echo "rc=$rc"; cat "out/$name.mac.log" "out/$name.pc.log"
