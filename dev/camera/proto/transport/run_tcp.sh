#!/bin/bash
# TCP stand-in for the fallback over the Beamer link: media length-prefixed on one TCP connection
# through `ssh -L` (sshd on the PC, loopback listener there: no firewall rule). Pings stay on UDP.
#   run_tcp.sh NAME DURATION_S "send args"
set -u
cd "$(dirname "$0")"
name=$1 dur=$2 sargs=$3
pc=${PC_HOST:-192.168.1.134} me=${MAC_HOST:-192.168.1.138}
mkdir -p out
scp -q vprobe.py lebig:camera-c3/vprobe.py
ssh -N -o ExitOnForwardFailure=yes -L 40160:127.0.0.1:24851 lebig &
tpid=$!
ssh lebig "cd C:\\Users\\Alex\\camera-c3; .\\venv\\Scripts\\python.exe vprobe.py recv --port 24850 --punch $me:40150 --tcp-port 24851 --duration $((${dur%.*} + 40)) --out $name.pc.npz" > "out/$name.pc.log" 2>&1 &
spid=$!
sleep 4
uv run -q --python 3.13 --with numpy vprobe.py send --to "$pc:24850" --local-port 40150 --tcp 127.0.0.1:40160 --duration "$dur" --out "out/$name.mac.npz" $sargs > "out/$name.mac.log" 2>&1
rc=$?
wait $spid
kill $tpid 2>/dev/null
scp -q "lebig:camera-c3/$name.pc.npz" "out/$name.pc.npz"
echo "rc=$rc"; cat "out/$name.mac.log" "out/$name.pc.log"
