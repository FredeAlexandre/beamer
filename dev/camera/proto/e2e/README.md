# Camera C4: the end-to-end prototype chain

Findings: `dev/camera/reports/c4.md`. This file says what the pieces are and how to reproduce the runs.

```
Mac  camsend ─ C1 synthetic source (code strip: frame number + Mac capture time)
             ─ VideoToolbox low-latency H.264, LTR (C1 settings)
             ─ C3 packets (36 B header + <= 1148 B + 16 B tag), pacer: P/LTR via a 32-packet / 25 Mbit/s bucket,
               IDR at k x bitrate, retransmissions first, 1 s history
        UDP 40160 <-> 24860 over the Wi-Fi (both sides send first: no firewall rule, no dialog)
PC   camrecv (Alex's session, scheduled task) ─ demand from the virtual camera -> kStart / kStop to the Mac
             ─ reassembly, NACK (<= 64-packet holes, 3 tries), PLI, LTR acks in the 10 ms receiver report
             ─ MF H.264 decode (D3D11, AVLowLatencyMode, staging readback: C2), in order
             ─ loss -> decode but do not show until an IDR / LTR refresh after the lost frame
             ─ adaptive playout (p99 of 10 s of completion delays + 8 ms, 50-150 ms, from encoder output: C3)
             ─ C2's shared-memory ring -> C2's media source in Frame Server -> "Beamer Camera"
             ─ oracle thread: PSNR-Y of every frame written against C1's synthetic picture (test only)
     Chrome  web/gum4.html, headless: getUserMedia("Beamer Camera"), reads the code strip of every frame
```

| file | what |
|---|---|
| `proto.h` | the wire (packet types and layouts), shared by both sides |
| `mac/camsend.mm`, `mac/build.sh` | Mac sender daemon (`build/camsend --peer PC_IP --log FILE`), compiles C1's `../mac/src/synth.mm` in |
| `win/camrecv.cpp`, `win/build.cmd` | PC receiver; built on the PC with C2's `common.h` next to it |
| `web/gum4.html` | browser consumer: `?mode=latency&secs=600` or `?mode=cycles&n=20&hold=4&gap=3`, `&w=&h=` |
| `scripts/run.sh NAME SECONDS "camrecv args" "gum4 query" [sampler]` | one run, driven from the Mac; raw logs in `out/raw/` (git-ignored) |
| `scripts/*.ps1` | PC side: `setup.ps1` / `cleanup.ps1` (elevated: C2's camera from `camera-c4\bin`), `insession.ps1` (session-1 task), `browser.ps1`, `sampler.ps1` |
| `analyze.py NAME` | clock fit, per-frame join Mac -> PC -> browser, stage breakdown, freezes, integrity, CPU/memory |
| `analyze_q23.py cycles|loss|idr NAME` | on-demand cycles, loss-injection episodes, IDR pacing |
| `dfloor.py NAME` | what-if: late frames per minute for a fixed playout delay, from a measured run |
| `samplers.py NAME` | CPU / memory of `camrecv`, Frame Server, Chrome (PC sampler) and `camsend`, `VTEncoderXPCService` (Mac) |
| `out/*.json` | the summaries the report quotes (`q2_cycles_v1.json`: first Q2 run, IDR effectively paced at 2.0x by a since-fixed pacer bug) |

PC work folder `C:\Users\Alex\camera-c4\` (`src\`, `bin\`, `web\`, `run\`, the scripts). `bin\` also holds C2's
`BeamerCamC2.dll`, `vcamctl.exe`, `camprobe.exe` (copied from `camera-c2\bin`).

```sh
mac/build.sh
scp win/camrecv.cpp win/build.cmd proto.h ../win/common.h lebig:camera-c4/src/ && ssh lebig 'cmd /c C:\Users\Alex\camera-c4\src\build.cmd'
scp web/gum4.html lebig:camera-c4/web/ && scp scripts/*.ps1 lebig:camera-c4/
ssh lebig 'powershell -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c4\setup.ps1'      # elevated (ssh is)
scripts/run.sh q1_720 600 "" "mode=latency&w=1280&h=720&secs=600"
scripts/run.sh q2_cycles 200 "" "mode=cycles&w=1280&h=720&n=20&hold=4&gap=3"
scripts/run.sh q3_loss 260 "--loss 8:drop1,16:burst5,24:burst30,32:burst80,40:blackout1500@48" "mode=latency&secs=250"
scripts/run.sh q3_idr720 300 "--key-every 60 --idr-pace 4 --audio --no-oracle" "mode=latency&secs=300"
scripts/run.sh q4_soak 1800 "--audio" "mode=latency&w=1280&h=720&secs=1800"
uv run -q --python 3.13 --with numpy analyze.py q1_720
ssh lebig 'powershell -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c4\cleanup.ps1'    # at the end
```
