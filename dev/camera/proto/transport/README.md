# Camera C3: video over the Mac -> PC link (prototype and measurements)

Findings and recommendations: `dev/camera/reports/c3.md`. This file says how to reproduce them.

| file | what |
|---|---|
| `vprobe.py` | the probe. `send` (Mac): 30 fps synthetic H.264 access units (size model, or a CSV trace) cut into packets of the proposed wire format (24 B engine header + 12 B video header + <= 1148 B fragment + 16 B tag = <= 1200 B UDP payload), sent in bursts, paced (`--pace N` x bitrate) or through a token bucket (`--pace-bps`, `--burst-pk`); plus an audio-like stream (80 B every 10 ms) on the same socket, pings 5/s. `recv` (PC): logs every arrival (QPC), answers pings, sends a 46 B receiver report every 10 ms like the engine (keeps the RZ616 radio awake). `--tcp` / `--tcp-port`: media length-prefixed on one TCP connection instead (stand-in for the Beamer-link fallback). No crypto: the packets have the sealed sizes. |
| `run_pair.sh` | one UDP run: copies `vprobe.py` to `C:\Users\Alex\camera-c3`, starts `recv` there over ssh, runs `send` here, fetches the PC log. |
| `run_tcp.sh` | one TCP run through `ssh -N -L 40160:127.0.0.1:24851 lebig` (loopback listener on the PC: no firewall rule). |
| `batch1.sh`..`batch3.sh` | the runs of 2026-11-11 (10 min UDP per configuration, 5 min TCP). |
| `analyze.py NAME` | clock fit (lowest-RTT ping per 5 s, linear: drift 19-25 ppm), per-packet and per-frame delays, loss bursts, spike episodes, audio delay, RR delay -> `out/NAME.summary.json` + `out/NAME.trace.npz` |
| `sim.py NAME [--overlay=..] [--policies=..] [--D=..] [--key-kb=..] [--bucket=bps,pk]` | replays a measured trace under recovery policies (IDR on PLI, LTR refresh, intra refresh, NACK, XOR FEC) and playout delays; see its docstring for the model. `simtab.py` turns its output into a table. |
| `burstcheck.py`, `holcheck.py` | audio delay near keyframe bursts vs elsewhere; TCP delay events vs the UDP pings at the same moment. |

Run (Mac; the PC side needs only `C:\Users\Alex\camera-c3\venv` with numpy, made with
`uv venv --python 3.13` + `uv pip install numpy`):

```sh
./run_pair.sh v720b 600 "--rate 2.5e6 --key-every 60 --key-kb 80 --pace 0"
uv run -q --python 3.13 --with numpy analyze.py v720b
uv run -q --python 3.13 --with numpy sim.py v720b --overlay=ge0.5 --D=80 --key-kb=120 | uv run -q python simtab.py
```

Ports: PC UDP 24850 (TCP 24851 on loopback), Mac UDP 40150, i.e. outside Beamer's 24823-24830.
The PC sends first to the Mac's port (punch), so no firewall rule is needed on either machine.
LAN addresses (Mac 192.168.1.138, PC 192.168.1.134); ICMP to the PC fails from an agent shell on the
Mac (macOS local-network privacy), UDP does not.

`out/` keeps the summaries and simulation tables in git; the raw `.npz` logs (~10 MB per run) stay on
the Mac only.
