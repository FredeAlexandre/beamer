#!/bin/bash
# One C4 end-to-end run, driven from the Mac.
#   scripts/run.sh NAME SECONDS "camrecv args" "gum4 query" [sampler 0|1]
# PC: camrecv (session 1, scheduled task C4-recv) + headless Chrome on gum4.html (task C4-browser) + an
# elevated sampler over ssh; Mac: camsend + a ps sampler. Raw logs land in out/raw/NAME.*
set -u
cd "$(dirname "$0")/.."
name=$1 secs=$2 rargs=$3 query=$4 samp=${5:-1}
raw=out/raw; mkdir -p "$raw"
PC='C:\Users\Alex\camera-c4'
MAC_IP=${MAC_IP:-192.168.1.138} PC_IP=${PC_IP:-192.168.1.134}
cat > "$raw/$name.recv.ps1" <<PS
& '$PC\\bin\\camrecv.exe' --mac $MAC_IP --log '$PC\\run\\$name.recv.jsonl' --seconds $((secs + 90)) $rargs
PS
scp -q "$raw/$name.recv.ps1" "lebig:camera-c4/run/$name.recv.ps1"
ssh lebig "powershell -ExecutionPolicy Bypass -File $PC\\insession.ps1 -Name C4-recv -NoWait -Command \"& '$PC\\run\\$name.recv.ps1'\"" >/dev/null
if [ "$samp" = 1 ]; then
  ssh lebig "powershell -ExecutionPolicy Bypass -File $PC\\sampler.ps1 -Seconds $((secs + 60)) -Out $PC\\run\\$name.sampler.csv" >/dev/null 2>&1 &
  spid=$!
fi
sleep 3
mac/build/camsend --peer "$PC_IP" --log "$raw/$name.mac.jsonl" --seconds $((secs + 80)) &
mpid=$!
( echo "t,pid,cpu_time,rss_kb,comm"; while kill -0 $mpid 2>/dev/null; do
    t=$(python3 -c 'import time;print(int(time.time()*1000))')
    ps -A -o pid=,time=,rss=,comm= | grep -E "camsend|VTEncoderXPCService" | grep -v grep | while read -r p tm rss comm; do echo "$t,$p,$tm,$rss,$(basename "$comm")"; done
    sleep 10; done ) > "$raw/$name.macps.csv" 2>/dev/null &
ppid_=$!
sleep 2
ssh lebig "powershell -ExecutionPolicy Bypass -File $PC\\insession.ps1 -Name C4-browser -NoWait -Command \"& '$PC\\browser.ps1' -Query '$query' -Tag $name -MaxSec $((secs + 120))\"" >/dev/null
t_end=$(( $(date +%s) + secs + 150 ))
while [ "$(date +%s)" -lt $t_end ]; do
  sleep 10
  if ssh lebig "Select-String -Path $PC\\run\\$name.chrome.log -Pattern C4DONE -Quiet" 2>/dev/null | grep -q True; then break; fi
done
sleep 3
ssh lebig "Get-Process camrecv -ErrorAction SilentlyContinue | Stop-Process -Force; Get-ScheduledTask C4-recv,C4-browser -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:\$false" >/dev/null 2>&1
kill $mpid 2>/dev/null; wait $mpid 2>/dev/null
kill $ppid_ 2>/dev/null
[ "$samp" = 1 ] && { kill $spid 2>/dev/null; ssh lebig "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | ? { \$_.CommandLine -like '*sampler.ps1*' } | % { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1; }
for f in recv.jsonl chrome.log sampler.csv recv.jsonl.audio.csv; do scp -q "lebig:camera-c4/run/$name.$f" "$raw/$name.$f" 2>/dev/null; done
ls -la "$raw" | grep "$name"
