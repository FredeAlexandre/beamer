# Camera across Beamer: Phase 1 conclusions (technical proof)

Date 2026-11-11. Branch `feat/camera`. Synthesis of the Phase 1 work packages C1 (Mac capture + VideoToolbox),
C2 (Windows 11 virtual camera), C3 (transport over the Mac <-> PC link) and C4 (the end-to-end chain: Mac
synthetic source -> VideoToolbox LTR -> paced UDP -> PC receiver -> MF decode -> virtual camera -> Chrome).
Details, methods and raw tables: `reports/c1.md`, `c2.md`, `c3.md`, `c4.md` and the prototypes in `proto/`
(C4: `proto/e2e/`). Machines: MacBook Air M5 (macOS 27.2), PC `lebig` (Windows 11 25H2 build
26200, Ryzen 5 7600X, RX 7900 XT), both on home Wi-Fi, installed Beamer (audio) running during C3.

## 1. Verdicts

### C1: Mac capture and encode

| Question | Answer | Evidence |
|---|---|---|
| Camera formats | Built-in camera: 7 formats, all `420v` (NV12 video range), 15-30 fps; 720p = format 1, 1080p = format 5 (default). Pick the format by size, never scale in AVFoundation. | c1 Q1 |
| Hardware low-latency H.264 | **Go.** `EnableLowLatencyRateControl` selects the hardware `h264.rtvc` encoder: 5.2 / 5.5 ms p50/p95 (720p), 4.7 / 4.8 ms (1080p), 1.5-2.5 % CPU, bitrate ±4 % per second, PSNR-Y 41 dB (synthetic). Software encoder unusable (436 ms). | c1 Q3 |
| GOP / recovery | Infinite GOP, no periodic keyframe, no gradual intra refresh. One lost frame corrupts everything after it. **LTR refresh heals in one 12-38 KB frame** after a real ack; IDR = 120 KB (720p) / 256 KB (1080p) plus ~10 frames of rate debt. Bit-exact recovery in libavcodec. | c1 Q3 loss table |
| Live bitrate change | Effective within one 200 ms window. | c1 Q3 |
| Encoder start | Session create 137-160 ms + first IDR 33-37 ms (720p) / 54-56 ms (1080p). | c1 Q1 |
| Camera cold start, capture latency, low light, sharing with another app | **Not measured: camera permission pending (Needs Alex 1).** Default frame duration allows **15 fps in low light** unless pinned. | c1 Q1, Q2, Q4 |
| Video effects | User-controlled per app (Control Center); Beamer can only take Center Stage and default reaction gestures off (`NSCameraReactionEffectGesturesEnabledDefault = NO`). Effects render into our frames. | c1 Q5 |
| Permission (TCC) | By design: Beamer.app is the responsible process; `NSCameraUsageDescription` + `com.apple.security.device.camera` on app and helper. First prompt not seen. | c1 Q6 |

### C2: Windows virtual camera

| Question | Answer | Evidence |
|---|---|---|
| Feasibility | **Go**, `MFCreateVirtualCamera` + own media source DLL, unsigned DLL loads (Smart App Control off). | c2 Q1 |
| Admin | **One elevated step at install**: COM class in HKLM (HKCU-only fails `0x80070003`), DLL outside the user profile and readable by LocalService (`0x80070005` otherwise). Camera itself created **System lifetime + CurrentUser access without admin** (first create 5.1 s, then 28 ms); it survives its creator. AllUsers needs admin. | c2 Q1 |
| Visibility | MF, DirectShow (NV12 and YUY2 added by the bridge), Chrome/Edge 157 at 720p/1080p30, 0 gaps. Name shown: **"Beamer Camera (Caméra virtuelle Windows)"** (suffix added by Windows). Camera app, Teams, Discord: Needs Alex. | c2 Q2 |
| On demand | App open -> helper 0.01-0.7 ms, app close 0.36 ms, app killed 13 ms. Frame Server re-creates the source 2-3 times after a stop: stops must be idempotent. | c2 Q3 |
| Frames in | Shared-memory ring **owned by the source** (a user process cannot create `Global\` sections), helper -> app 0.45-0.57 ms (720p), 0.83-1.02 ms (1080p); Frame Server 0.5-1 % CPU. | c2 Q4 |
| Decode | In the helper, MF H.264 on D3D11, **`CODECAPI_AVLowLatencyMode` mandatory** (else 0.5-1 s buffered): 1.8-1.9 ms (720p) / 2.8-3.1 ms (1080p) at ~1.5-2 % CPU in Alex's session; software 10-27 % CPU. NV12 only. LTR recovery bit-exact, **but corrupt frames decode without error**: the receiver must track loss itself and hold the last good frame. | c2 Q5 |
| Robustness | 2nd app refused (`0xC00D3704`) unless Windows' per-camera multi-app toggle is on (untested); helper killed -> placeholder, frames return on restart; 720p <-> 1080p switching without a gap. Logoff/reboot, sleep, GPU reset, Smart App Control: untested. | c2 Q6 |

### C3: transport

| Question | Answer | Evidence |
|---|---|---|
| Wire | **Same UDP flow and helper as audio, new stream kind `video`**, packet v1 header unchanged, 12 B video header, fragments <= 1148 B (UDP payload <= 1200 B), new `nack`/`pli`/`sr` packets, 24 B video receiver report, 1024-packet replay window for video. | c3 Q1 |
| Wi-Fi at 2.5 / 5 Mbit/s | Fine: frame network delay p50 2.5-3.3 ms, p99 22-34 ms; audio p95 unchanged (6.8 ms alone, 5.9-8.6 with video). **Every burst loss (27-82 packets, with 50-60 ms of audio) began inside a large keyframe sent unpaced.** | c3 Q2 |
| Pacing | P/LTR frames through a 32-packet / 25 Mbit/s bucket (in effect unpaced); IDRs paced at 2.5-4x the bitrate (2.5x: 0 loss in 600 keyframes, +111 ms; 4x: see C4, now the choice). | c3 Q2 |
| Recovery | **NACK (<= 3 tries, holes <= 64 packets, within the deadline) + LTR refresh on PLI; IDR only at start / last resort.** 177 ms frozen/min under 0.5 % bursty loss vs 1,350 for IDR-on-PLI; no FEC, no intra refresh. | c3 Q3 |
| Playout | Adaptive: p99 of the last 10 s + 8 ms, floor 50, cap 150 ms, **~70 ms** here: <= 1 late frame/min. Late frames decoded, never shown; jump to newest after a stall. (**Superseded by C4**: late frames are shown on arrival; D sits at its 50 ms floor.) | c3 Q4 |
| Bitrate control | No ABR needed on this LAN; a safety step-down from the receiver report (5 -> 2.5 -> 1.2 Mbit/s). | c3 Q4 |
| TCP fallback | Video ~1 Mbit/s (720p15) on the link only, dropped before audio (outbox > 32), paused after 1 s of backlog; never on the SSH tunnel. | c3 Q5 |

### C4: end-to-end chain

| Question | Answer | Evidence |
|---|---|---|
| Latency, synthetic source -> Chrome on the PC (p50 / p95 / p99) | **Go. 720p30 2.5 Mbit/s 57.5 / 60.3 / 65.3 ms; 1080p30 5 Mbit/s 56.8 / 63.3 / 107 ms** (10 min each, 0 corrupt). The playout hold is ~80 % of it: 44-45 ms (D at its 50 ms floor, measured from encoder output, for 80-98 % of frames); encode 4-5, pacing 0.1, network 3.2 p50 (p99 33-38), decode 1.9 / 3.1, virtual camera + Chrome 2.1-2.4 ms. With the real camera's own delay (unmeasured, assumed 25-70 ms): **~85-130 ms glass to browser p50**. | c4 Q1 |
| Real NACK/LTR loop | **Works as designed; 0 corrupt frames reached the browser** (code strip + PSNR oracle on 7,220 frames). 1 and 5 lost packets: NACK, 0 ms frozen; 30: NACK, 50-70 ms frozen (late frames); 80: PLI -> LTR refresh (11-33 KB), 300 ms frozen (≈ the burst); 1.5 s blackout: 1.57 s frozen, refresh 17-45 ms after the PLI. **11 recoveries, 11 LTR refreshes, 0 IDRs.** Acks must name the clean run since the last recovery (`[clean_from, acked]`). | c4 Q3 |
| IDR pacing at 4x the bitrate | **0 packets lost in 300 IDRs** (720p 120 KB at 10 Mbit/s, 1080p 256 KB at 20 Mbit/s), IDR sent in 100 / 107 ms; audio-like packets on the same socket: 0 lost. A pacer must keep an absolute schedule (sleep overshoot turned 2.5x into 2.0x). | c4 Q3 |
| On demand | **20/20 per setting.** Chrome opens Beamer Camera -> first Mac picture **p50 260 ms / p95 308 ms at 4x** (324 / 338 at 2.5x): Chrome's own open 79 ms + paced IDR 100 ms dominate. Encoder session **4 ms when the process is warm** (cold, first of the process: 163-174 ms). Close -> Mac encoder stopped **p50 24-33 ms, max 51 ms**. Real camera cold start not measured (assumed 0.2-0.8 s -> ~0.5-1.1 s in practice). | c4 Q2 |
| 30-min stability (720p) | **Stable.** 54,000 frames, 0 corrupt, 1 gap > 100 ms; latency p50 57.0 ms in every 5-min window. Clocks drifted **10.5 ppm (18 ms / 30 min)**, other runs -6.5 to +0.9 ppm: the receiver tracked the offset continuously, hold stayed 44.6 ms. Memory flat (Mac sender +2.5 MB, unexplained, watch). CPU (% of a core): PC receiver 3.1 % (720p) / 5.2 % (1080p), Frame Server 0.8 %; Mac sender 1.3 % + VT 1.3 %. | c4 Q4 |
| Lip sync (pointer) | Video reaches the PC app in 57 ms (synthetic), Mac mic via CABLE in 172 ms (shared) / 120 ms (low-latency): with a 25-70 ms camera, **video leads audio by 45-90 ms (shared) / -7..+38 ms (low-latency)**. Delay video, never audio. | c4 Q5 |
| Firewall | A **new** PC executable binding UDP in the user's session triggers the Windows Firewall prompt (Allow rules were created for the prototype, then removed): video must live in `beamer-audio.exe`, which has installer rules. | c4 Cleanup |

## 2. Numbers that drive the design

| Number | Value | Used in PHASE2 |
|---|---|---|
| Encode (Mac, HW low latency) | 5 ms, ~2 % CPU | §4.8 budget |
| IDR size | 120 KB (720p 2.5 Mbit/s), 256 KB (1080p 5 Mbit/s) | §4.4 pacer |
| LTR refresh size | 12-38 KB | §4.5 |
| Encoder session create + first IDR | ~150 + 37/56 ms | §4.7 on-demand start |
| PC decode (HW, user session) | 1.9 ms (720p), 3.1 ms (1080p), ~2 % CPU | §4.6 |
| Helper -> app through the source | 0.5 / 1.0 ms | §4.6 |
| Network frame delay | p50 ~3 ms, p99 22-34 ms | §4.5 playout |
| Playout delay | adaptive 50-150 ms, ~70 ms here | §4.5 |
| IDR pacing | **4x bitrate** (C4: 0 loss in 300 IDRs), 100 ms (720p) / 107 ms (1080p) per IDR, absolute schedule | §4.4 |
| Placeholder threshold | **500 ms** with nothing feeding the camera (C2 recommended 250; C4: 250 would flash at an 80-packet loss); 2 s of hold before "Reconnecting" on a live stream | §4.6.4 |
| Playout floor | 50 ms from encoder output (C3/C4); a 30 ms floor saves 20 ms for ~2 % late frames (C4 what-if) | §4.5.3 |
| Clock drift Mac vs PC | -6.5 to +10.5 ppm, wandering (C4); 19-25 ppm (C3) | §4.5.3 |
| Mac mic -> PC app (audio, for lip sync) | ~120 ms low-latency / ~170 ms shared (`dev/audio/STATUS.md` WP12, WP16; C4 Q5) | §4.9 |
| Encoder start (Mac) | 4 ms warm (VT XPC service alive), 163-174 ms cold (C4) | §4.7, §4.11 |
| Camera start (Mac) | **unmeasured** (permission): assumed 0.2-0.8 s; C1 real-camera run / H1 | §4.7 |
| First picture after the app opens (synthetic) | 260 ms p50 / 308 ms p95 at 4x (C4) | §4.7, §10.5 |
| Capture -> app (synthetic) | 57 ms p50, 60-63 ms p95 (C4) | §4.8, §10.5 |
| Glass-to-glass (real camera) | ~85-130 ms p50 (C4 + an assumed 25-70 ms camera) | §4.8 |

## 3. Open risks

1. **Real-camera terms unknown** until the Mac grants Camera: cold start, capture latency, low-light fps,
   sharing with another Mac app, effects in the frames (C1 Q1/Q2/Q4/Q5).
2. **Admin step on the PC** (HKLM COM + DLL under Program Files); DLL updates need it again; Smart App
   Control / WDAC would likely want a signed DLL (untested).
3. **Silent decoder corruption**: correctness of what the PC shows depends entirely on the receiver's own
   loss tracking (C2 Q5). C4 ran the real loop with 0 corrupt frames shown (9 broken frames decoded and held
   back), which validates the rules, not an implementation.
4. **Keyframe-triggered Wi-Fi holes** (C3): mechanism inferred, 4 events in 70 min, all at >= 25 Mbit/s. C4: 0
   loss in 300 IDRs at 4x, but 1080p's 20 Mbit/s is close to 25 and was one 5-min run. A soak must log every
   hole > 5 packets with the frame type.
5. **One app at a time** on the PC unless the per-camera multi-app toggle is on.
6. **Untested on Windows**: logoff/reboot survival of the System-lifetime camera, sleep/resume, GPU driver
   reset (software fallback), apps that change resolution mid-stream, Teams/Zoom/Discord, Camera app.
7. **TCP fallback** measured only through an SSH stand-in, not Beamer's base64 JSON link.
8. **"(Caméra virtuelle Windows)" suffix** on the name, no known way to remove it.
9. **From C4:**
   - the real camera's own delay and cold start are the one unmeasured latency term (assumed 25-70 ms and
     0.2-0.8 s);
   - one Wi-Fi spike pins the adaptive playout ~10 s higher (+50 ms; 2 % of a 10-min 1080p run);
   - lip sync in shared mic mode: video ~45-90 ms ahead of the audio, at or beyond EBU R37 until video is
     delayed to match;
   - VideoToolbox stays warm only while the encoding process lives; how long it stays warm when idle is
     unmeasured;
   - a new PC executable that opens a UDP port prompts the Windows Firewall;
   - Chrome's `performance` clock drifts -25 ppm against Windows time (any browser-based measurement must
     correct it);
   - the Mac sender grew 2.5 MB in 30 min, unexplained (watch in the Phase 2 soak).

## 4. What still needs Alex

1. **Camera permission for C1's real-camera suite** (~3 min, optional): allow the pending T3 Code prompt (or run
   `proto/mac/experiments/camera_suite.sh` from Terminal), then revoke. Otherwise the numbers come from
   Beamer.app's own prompt in Phase 2 (human check H1 there).
2. **Low light** (2 min, after 1): `experiments/low_light.sh`, decides whether 30 fps is pinned (§PHASE2 open question).
3. **Video effects** (5 min, after 1): Portrait / Studio Light / Center Stage / a reaction visible in our frames.
4. **PC apps** (Phase 3 test guide): Camera app, Teams, Discord show "Beamer Camera"; the multi-app toggle;
   the camera still listed after a logoff/reboot.
5. **Smart App Control**: tell us if it is ever turned on (the DLL would need a trusted signature).
