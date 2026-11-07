#!/bin/bash
# Run one Mac(client) <-> PC(server) session.
#   run_pair.sh NAME HOST DURATION "server args" "client args"
# Server: C:\Users\Alex\audio-proto\transport on `ssh lebig`, venv python. Results in out/NAME.*
# No PC firewall rule needed: the PC first sends to the Mac's fixed client port (--punch), so
# Windows' stateful UDP filter admits the Mac's datagrams (what audio_open would announce).
set -u
cd "$(dirname "$0")"
name=$1 host=$2 dur=$3 sargs=$4 cargs=$5
lport=${LPORT:-40123}
case $host in 100.*) me=100.103.100.52 ;; *) me=192.168.1.138 ;; esac
sargs="--punch $me:$lport $sargs"
cargs="--local-port $lport $cargs"
mkdir -p out
ssh lebig "cd C:\\Users\\Alex\\audio-proto\\transport; ..\\venv\\Scripts\\python.exe peer.py serve --key-file test.key --duration $((${dur%.*} + 10)) --quiet $sargs" > "out/$name.server.log" 2>&1 &
spid=$!
sleep 2.5
uv run -q --python 3.13 --with pynacl --with numpy peer.py send --to "$host:24823" --key-file out/test.key \
  --duration "$dur" --quiet --stats "out/$name.jsonl" $cargs > "out/$name.summary.json" 2> "out/$name.client.log"
rc=$?
wait $spid
echo "rc=$rc"; tail -c 3000 "out/$name.summary.json"
