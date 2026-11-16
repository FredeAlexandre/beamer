# Existing performance measurements: audio and camera (baseline of the Python app)

Extracted from `dev/audio/` and `dev/camera/` on `main` @ `18a4451` (2026-11-16). Nothing new was measured.
All paths are relative to the repo root; `path:line` points to the quoted number.

## What "final shipped" means here

- **Audio**: delivered state is `feat/audio` @ `7b83e71` (2026-11-08 23:18), deployed on both machines
  (`dev/audio/TEST-ALEX.md:3`, `dev/audio/STATUS.md:47-48`). The last full E2E (WP16) ran at `44e7291`, i.e.
  **before** WP17 (PC: mono vmic up-mix so the 128-frame low-latency period applies) and WP18 partial (AEC no
  longer injects the reference, TCC poll 2 s). Those fixes were never re-measured end to end
  (`dev/audio/TEST-ALEX.md:60`: "correctif déployé, pas remesuré").
- **Camera**: deployed `9a268a4` on both (2026-11-12), marked done on Alex's call; **the automated E2E on the
  product never produced a picture** (synthetic test source bug), so every camera latency number is from the
  Phase 1 prototype (C4) or from component tests (`dev/camera/STATUS.md:129-132`, `dev/camera/reports/cw11.md:134-151`).
- Both features were merged into `main` 2026-11-12 (`dev/README.md`). No record of an audio E2E re-run after the merge.
- Machines: MacBook Air M5 (macOS 27.x) and `lebig` (Ryzen 5 7600X, RX 7900 XT, Windows 11 build 26200), **both on
  home Wi-Fi** in every run (`dev/camera/PHASE1.md:7-8`, `dev/audio/proto/transport/README.md:7-8`).

Flags used below: **FINAL** = measured on the product in its final configuration; **PRE-FINAL** = product, but
a later fix changes the path; **COMPONENT** = one stage of the shipped code, not end to end; **PROTO** =
Phase 1 prototype / standalone tool, not the product.

---

## 1. Audio end-to-end latency (one way)

Method common to the product E2E rows (WP10/WP11/WP16): two-machine harness `dev/audio/e2e/`, marker
signals A/B from `sig.py`; mic cases use the helper's dev **test source** (latency starts where a mic capture
would deliver, so no microphone/ADC/acoustics); sound cases play with the Phase 1 tool. Sinks recorded "as an app
would": Mac "Beamer Microphone", PC "CABLE Output", **Mac process tap of the speakers, PC endpoint loopback of the
headset** (digital, so no DAC / headset radio). Clocks joined by ssh pings, error <= 2 ms (RTT min 4.1 ms)
(`dev/audio/e2e/run.py:12-18`, `dev/audio/reports/wp10.md:3`). Default case length 75 s, soak 1860 s
(`dev/audio/e2e/run.py:515`). All Wi-Fi. Only p50/p95 are reported; no p99.

| Path | p50 / p95 ms | n / duration | Config | Commit / date | Flag | Source |
|---|---|---|---|---|---|---|
| **PC mic -> Mac app (Beamer Microphone)** | a_1 **56.5 / 62.0** (JB 37/38); after Mac helper restart **41.4** (hk_mac_1), **42.2** (ll_1) | 75 s default | test source on PC, Mac IO 128 frames | `44e7291`, 2026-11-08 | PRE-FINAL (closest to final; WP18 AEC changes not re-run) | `dev/audio/reports/wp16.md:8` |
| same | a_f1 41.1 / 46.2 | short run | WP11 final Mac build | `62beabc`, 2026-11-08 | PRE-FINAL | `dev/audio/reports/wp11.md:16` |
| same | a_1 43.4 / 43.7 | 75 s | Mac IO 480 frames | `6487938` | PRE-FINAL | `dev/audio/reports/wp10.md:8` |
| same (user guide headline) | "41-56 ms" | - | - | `7b83e71` | summary of WP16 | `dev/audio/TEST-ALEX.md:57` |
| same, Phase 1 | 29.5 / 37.1 to JB output; 55.4 / 63.2 to Mac speaker mix | 68 markers, 75 s | Python transport, Wave:3 | 2026-11-07 | PROTO | `dev/audio/proto/scenarios/README.md:107` |
| **PC system sound -> Mac speakers (PC muted)** | soak **65.1 / 84.9**; b_1 94.0 / 94.9 (JB 43/43); b_2 75.5 / 82.8 (JB 23/20) | soak 31 min; b_* 75 s | PC process loopback 480-frame packets, Mac render 128 frames | `44e7291` | FINAL path (no later change to it) | `dev/audio/reports/wp16.md:9`, `:14` |
| same | b10_f1 **72.9 / 87.5** | 10 min | WP11 final (128-frame Mac IO, 10 ms period floor) | `62beabc` | PRE-FINAL | `dev/audio/reports/wp11.md:13` |
| same | b_3 81.0 / 95.7 | 75 s | Mac IO 480 | `6487938` | PRE-FINAL | `dev/audio/reports/wp10.md:9` |
| same (user guide) | "65 ms" | - | - | - | summary | `dev/audio/TEST-ALEX.md:58` |
| same, Phase 1 | 55.6 / 64.1 (s4_b); 51.1 / 71.4 (s4_a) | 67 markers | Python transport | 2026-11-07 | PROTO | `dev/audio/proto/scenarios/README.md:109` |
| **Mac system sound -> PC G733 (Mac muted)** | c_1 **69.5 / 80.2** (Mac mic hears speakers 10-20 dB lower) | 75 s | G733 render 480 x 1.5 (WP12) | `44e7291` | FINAL path | `dev/audio/reports/wp16.md:10` |
| same | c_3 80.3 / 85.0; c_4 88.0 / 163.8 (Wi-Fi episode, PC JB 52 ms) | 75 s | before WP12 | `6487938` | PRE-FINAL | `dev/audio/reports/wp10.md:10` |
| same (user guide) | "70 ms" | - | - | - | summary | `dev/audio/TEST-ALEX.md:59` |
| same, Phase 1 | 95.6 / 103.8 (Mac tap delivery -> G733 loopback) | 69 markers | Python transport | 2026-11-07 | PROTO | `dev/audio/proto/scenarios/README.md:108` |
| **Mac mic -> PC app via CABLE Output, shared (default)** | d_1 **172.4 / 185.0** (PC JB 42/55); soak 174.9 / 191.1 | 75 s; soak 31 min | low-latency period **refused live** (mono -> autoconvert, fell back to 480-frame period) | `44e7291` | PRE-FINAL: WP17 fixed this path, E2E not re-run | `dev/audio/reports/wp16.md:11`, `:14`, `:20` |
| same, low-latency switch (exclusive into CABLE Input) | d_ex1 **120.3 / 166.7** (PC JB 50/57; 4 markers missed; AEC on) | 75 s | `set_vmic_exclusive` | `44e7291` | FINAL path for the opt-in switch | `dev/audio/reports/wp16.md:12` |
| same, user guide | "172 ms ... normal, 120 ms ... faible latence; correctif déployé, pas remesuré" | - | - | `7b83e71` | summary | `dev/audio/TEST-ALEX.md:60` |
| same, expected after WP17 (not measured) | "shared ~110 is expected" | - | 128-frame x 3 shared render | - | estimate only | `dev/audio/reports/wp16.md:28`, `dev/audio/reports/wp12.md:20` |
| same | d_3 152.4 / 165.8 | 70 s | shared 480 x 3 | `6487938` | PRE-FINAL | `dev/audio/reports/wp10.md:11` |
| same, Phase 1 | after W3 fix: **107.7 / 115.8** (595/596 markers, 10 min); before: 118.5 / 130.1 | 10 min | Python transport, `play` 128-frame period | 2026-11-07 | PROTO | `dev/audio/proto/scenarios/README.md:106` |
| same, Phase 1, **real Mac mic** (acoustic chirps from Mac speakers) | total 185.7 / 193.9 (play -> Mac mic 71.0 / 73.5; mic delivery -> CABLE Output 112.8 / 123.6) | 12 markers, 20 s | before W3 fix | 2026-11-07 | PROTO (only acoustic-in run) | `dev/audio/proto/scenarios/README.md:106` |
| Both at once (Mac mic -> CABLE + PC sound -> Mac) | e_1 172.6 / 185.8 ; 95.2 / 98.7 | 75 s | AEC on | `44e7291` | PRE-FINAL | `dev/audio/reports/wp16.md:13` |

Component numbers on the PC virtual-mic render path (click from render callback -> app-style shared capture of CABLE Output; COMPONENT):

| What | Median (p95) ms | Holes | Source |
|---|---|---|---|
| WP17 final: mono up-mix, 128-frame period x 3 | 79.1, 70.7, 71.5, 72.3; 300 s: **80.1** (p95 81.5) | 0 in 4x60 s + 300 s | `dev/audio/reports/wp17.md:13` |
| old path (autoconvert, 480) same session | 89.4 | 0 in 60 s | `dev/audio/reports/wp17.md:14` |
| WP12 stereo 128 x 3 | 72.2, 59.0, 66.3, 65.0; 300 s: 69.2 (p95 70.7) | 0 | `dev/audio/reports/wp12.md:12` |
| WP12 before (480 x 3) | 114.5, 102.3, 116.5 | 0 | `dev/audio/reports/wp12.md:8` |
| exclusive (opt-in) | 35.9 (WP4: 43-45) | 0 in 20 s; WP4: 1 hole in 300 s | `dev/audio/reports/wp12.md:13`, `dev/audio/reports/wp4.md:18` |
| G733 render 480 x 1.5 (chosen) | 75.8, 71.1, 65.6; 300 s: 65.9 | 0 | `dev/audio/reports/wp12.md:15` |

Context (not E2E): Phase 1 transport alone 30-34 ms one way = network 2.7 + JB 20-24 + Opus 6.5
(`dev/audio/proto/transport/README.md:137-139`); Mac hardware mic->speaker 44.2 ms acoustic, constant
(`dev/audio/STATUS.md:146`); Mac Beamer Microphone driver loopback 17.5 ms at 512 frames / 4.2 ms at 128
(`dev/audio/STATUS.md:14-15`); echo delay seen by AEC 44.9 ms (`dev/audio/reports/wp9.md:20`).

## 2. Audio dropouts, holes, jitter (soaks and long runs)

"Glitches" = detector events per minute on the pilot signal (episodes in brackets). JB = receiver jitter buffer level/target.

| Run | Duration | Figures | Config / note | Commit | Flag | Source |
|---|---|---|---|---|---|---|
| WP16 soak: Mac test mic -> CABLE + PC sound -> Mac | 31 min | Mac->PC **22.1/min (7.6 ep)**; PC->Mac 133 raw, **1.9 in quiet seconds** (1.88/min over 14.3 min without room sound); JB Mac 23.9/21.6 ms, 1.39 underruns + 2.6 expansions/min; PC 29.1/27.3 ms, 1.69 underruns + 2.6/min; **0 loss, 0 decode errors** | Mac->PC inflated by AEC injecting the reference (fixed in WP18, not re-measured); AEC off d_1: 5.0/min; d_ex1 2.0/min | `44e7291` | PRE-FINAL | `dev/audio/reports/wp16.md:14-15`, `:23` |
| WP16 PC capture-loss counters | soak | 185,881 packets, 89,222,880 frames, 0 silent/discontinuity/timestamp errors, 0 position gaps, max wake gap 19.5 ms | - | `44e7291` | FINAL path | `dev/audio/reports/wp16.md:15` |
| WP16 drift integral | soak | Mac recv 15.9..16.5 ppm (true ~16.2); PC recv -23.8..-15.3 (end -16.4) | pass | `44e7291` | FINAL path | `dev/audio/reports/wp16.md:15` |
| WP16 howl guard | soak | 23 false `howl` events (307-523 Hz), PC sound ducked 20 dB for 3-19 s each | not fixed (WP18 stopped) | `44e7291` | FINAL behaviour | `dev/audio/reports/wp16.md:17` |
| WP16 short-run JB | 75 s runs | JB held at 38-55 ms in a_1, b_1, d_1, e_1, fb_1, hk_pc_1; fell to ~20 after Mac helper restart | cause not found | `44e7291` | - | `dev/audio/reports/wp16.md:21` |
| WP10 soak (same routes) | 31 min | Mac->PC **2.16/min (1.17 ep)**; PC->Mac 32.9/min (room sound via Wave Link, not the stream); PC JB 27.3/27.6, 1.3 underruns/min; Mac JB 23.8/21.0, 2.4 underruns/min | before AEC existed | `6487938` | PRE-FINAL | `dev/audio/reports/wp10.md:18`, `:21` |
| WP11 PC sound -> Mac | 10 min | 9.4/min (3.9 ep); JB 24.5/24.2; 1.85 underruns/min | 20 of 63 follow an expansion | `62beabc` | PRE-FINAL | `dev/audio/reports/wp11.md:13`, `:17` |
| WP14 PC system capture | 10 min idle + 10 min with 12 busy threads | **0 samples lost**; encoded frames vs QPC -6.2 ±17 / -0.9 ±17 ppm; wake gaps up to 19.8 ms | sample-exact ramp | `pc/wp14` | COMPONENT | `dev/audio/reports/wp14.md:7-8`, `:13-14` |
| WP12/WP17 CABLE render holes | 4x60 s + 300 s | 0 holes at 128 x 3; 128 x 2 had 1,1,0,0 (rejected) | - | - | COMPONENT | `dev/audio/reports/wp12.md:11-12`, `dev/audio/reports/wp17.md:13` |
| WP2 two helpers, 2 % loss, +80 ppm | 120 s | JB level p99 - target 11.3-13.4 ms (Mac), 8.6-13.0 (PC); 0 decode errors | null backends | `audio/wp2` | COMPONENT | `dev/audio/reports/wp2.md:15-16` |
| Phase 1 scenario 1 (Mac mic -> CABLE), after W3 | 10 min | **2.1/min (0.9 ep/min)**; PC JB 8 underruns, 12 expands, 0 drops; 73 frames flushed after one 0.66 s stall; 0 packets lost | before: 50.3/min (5 min), 61.7/min | 2026-11-07 | PROTO | `dev/audio/proto/scenarios/README.md:106`, `dev/audio/STATUS.md:121` |
| Phase 1 scenarios 2-5 | 75 s each | 10.2 / 1.9 / 4.0 / PC 3.1 + Mac 8.0 per min; 0 packets lost | before W3 fix | 2026-11-07 | PROTO | `dev/audio/STATUS.md:171-174` |
| Phase 1 transport loss (Opus FEC on/off, glitch 10 ms windows per 45 s) | 45 s | 1 %: 4/13; 5 %: 48/60; 10 %: 73/87 | injected loss | 2026-11-07 | PROTO | `dev/audio/STATUS.md:161` |

## 3. Camera: latency, fps, start

**No glass-to-glass number exists.** The product E2E (CW11 Q2, 2026-11-12, `e09ea72` / product `4394e01`) got 0
Mac frames: `cam_latency` p50/p95/p99 = null, `cam_start` first picture = null
(`dev/camera/e2e/runs/SUMMARY.md:7-10`, `dev/camera/reports/cw11.md:143-151`). Alex's only verdict on the real
camera is qualitative ("it really seems to work well", `dev/camera/STATUS.md:23`).

| Item | Number(s) | n / duration | Method | Config | Flag | Source |
|---|---|---|---|---|---|---|
| **Capture -> frame in Chrome on PC, 720p** | p50 **57.5** / p95 **60.3** / p99 **65.3** (max 78) ms | 17,992 frames, 10 min | synthetic source with code strip (frame no. + Mac capture time) read by Chrome `requestVideoFrameCallback`; Mac<->PC clock by lowest-RTT ping per 5 s + linear fit (residual <= 1.3 ms); Chrome clock drift (-25 ppm) corrected | 720p30, 2.5 Mbit/s, playout floor 50 ms, Wi-Fi | PROTO (C4) | `dev/camera/reports/c4.md:12`, `:83-94`, `:50-55` |
| same, 1080p | p50 **56.8** / p95 **63.3** / p99 **106.6** (max 144) ms | 17,975 frames, 10 min | same | 1080p30, 5 Mbit/s; p99 from one 133 ms Wi-Fi spike | PROTO (C4) | `dev/camera/reports/c4.md:92`, `:97-99` |
| stage split 720p (p50/p95/p99) | encode 5.15/5.44/5.59; pacing 0.11; network 3.22/8.1/33.0; decode 1.92/2.03/2.09; **hold 44.8/47.3/52.6**; vcam+Chrome 2.12/2.47/2.61 | same | per-frame join | - | PROTO | `dev/camera/reports/c4.md:85-91` |
| 30-min soak, 720p | p50 **57.0** in every 5-min window; p99 58 calm / 84-88 in 2 windows with Wi-Fi spikes; **29.99 fps**, 54,000 frames, 0 corrupt, 1 gap > 100 ms (102 ms) | 30 min | same | 720p | PROTO | `dev/camera/reports/c4.md:15`, `:207-211` |
| Glass-to-glass (real camera) | "**~85-130 ms**" p50 | - | **estimate**: 57 ms + assumed 25-70 ms camera delay | - | estimate, never measured | `dev/camera/reports/c4.md:111-114`, `dev/camera/PHASE1.md:82` |
| Real camera: camera PTS -> encoder output | p50/p95 48/60, 49/62 (1080p), 46/56 ms | 4 runs, ~40 s total | Mac helper backend in an agent shell (T3 Code's camera grant), not Beamer.app | 720p/1080p, dark room | COMPONENT (shipped Mac backend code, CW3) | `dev/camera/reports/cw3.md:98-105` |
| Real camera fps | **15.4, 16.0, 16.2, 16.1 fps** | same | `fps` events | evening, dark room (auto exposure) | COMPONENT | `dev/camera/reports/cw3.md:103`, `:107` |
| Real camera cold start (capture start -> first frame) | 1218 (before format fix), 723 (1080p), 742, 749 ms; `start_encoding` -> first AU 1044, 646, 670 ms | 4 runs | same | - | COMPONENT | `dev/camera/reports/cw3.md:100-101` |
| Real camera stop (last stream -> capture stopped) | 210, 37, 20, 35 ms | 4 runs | same | - | COMPONENT | `dev/camera/reports/cw3.md:104` |
| Synthetic on-demand: Chrome opens -> first Mac picture | **260 / 308 ms** (p50/p95, max 421) at IDR 4x; 324 / 338 (max 510) at 2.5x | 20 cycles each | C4 chain | warm encoder | PROTO | `dev/camera/reports/c4.md:13`, `:134` |
| Close -> Mac encoder stopped | p50 24-33 ms, max 51 ms | 20 cycles | C4 | - | PROTO | `dev/camera/reports/c4.md:138` |
| Engine first frame (two helpers, null codec) | 107.6 ms (720p), 116 ms (1080p); `cam_start_ms=800` -> 898 ms | 120 s runs | CW2 harness, null video backend | - | COMPONENT | `dev/camera/reports/cw2.md:44-46` |
| Mac warm start (`open_camera` -> first AU, standby session) | p95 **36.7** (p50 35.5) ms; after 1/10/60 min idle: 19.2 / 40.4 / 37.5 ms | 20 opens; 1 sample per idle cell | synthetic, `video_check` | standby shipped | COMPONENT | `dev/camera/reports/cw3.md:40`, `:81-85` |
| Encoder session create | cold 97-155 ms (CW3), 163-174 ms (C4); warm 1.0-5.0 ms (CW3), 4 ms (C4) | - | - | - | COMPONENT / PROTO | `dev/camera/reports/cw3.md:41`, `dev/camera/reports/c4.md:13` |
| Mac encode (synthetic 720p30) | p50 5.4 / p95 5.8 ms (capture -> AU) | 10 s | `video_check` | HW low-latency `h264.rtvc` | COMPONENT | `dev/camera/reports/cw3.md:33` |
| PC HW decode | 720p p95 1.89-2.00 ms (4 runs, 30 s); 1080p p95 3.18-3.30 ms (6 runs, 60-90 s) | - | `video_check play` in session 1 | D3D11, low-latency mode | COMPONENT | `dev/camera/reports/cw4.md:42` |
| Helper -> app through the camera source DLL | 720p p95 0.54 ms, 1080p p95 0.80 ms; 30.0 fps 0 gaps over 60 s | - | `feeder` + `camprobe` | - | COMPONENT | `dev/camera/reports/cw5.md:41`, `dev/camera/STATUS.md:72` |
| Placeholder after helper stops | 507.2-509.2 ms (CW5); live product: 547.2 ms after helper kill | 5 runs; 1 run | - | 500 ms threshold | COMPONENT; FINAL (one live value) | `dev/camera/reports/cw5.md:42`, `dev/camera/e2e/runs/cam_helper_kill_1112_195720/result.json` |
| Virtual camera creation at install | 208 ms (Alex's install through the page, H6) | 1 | Beamer log | - | FINAL | `dev/camera/STATUS.md:25-26` |
| Lip sync (video held to the mic path) | unit tests only: A/V skew <= 2 ms in simulation; live `cam_lipsync` never run | - | engine tests | - | not measured live | `dev/camera/STATUS.md:99`, `dev/camera/reports/cw11.md:151` |

## 4. CPU and memory (% of one core)

| Machine | Process | State | CPU | Memory | Duration / method | Flag | Source |
|---|---|---|---|---|---|---|---|
| Mac | Beamer app | audio soak (Mac test mic -> CABLE + PC sound -> Mac) | **4.6 %** | **401.6 -> 411.3 MB** | 31 min | PRE-FINAL (`44e7291`) | `dev/audio/reports/wp16.md:15` |
| Mac | `beamer-audio` helper | same soak | **8.0 %** (of which AEC 2.9) | 17.8 -> 20.4 MB | 31 min, helper `stats.cpu_pct` | PRE-FINAL (AEC on a test source; WP18 changed it) | `dev/audio/reports/wp16.md:14-15` |
| PC | Beamer app | same soak | **1.6 %** | 82.1 -> 82.2 MB | 31 min | PRE-FINAL | `dev/audio/reports/wp16.md:15` |
| PC | `beamer-audio.exe` | same soak | **2.5 %** | 4.5 MB flat | 31 min | PRE-FINAL | `dev/audio/reports/wp16.md:14-15` |
| Mac / PC | helper per path (WP16 short runs) | (a) 1.3 / 1.3; (b) 1.7 / 1.3; (c) 4.1 / 0.8; (d) 3.3 / 0.7; d_ex1 7.2 (AEC on) / 1.0; (e') 8.4 / 2.4 | - | 75 s | PRE-FINAL | `dev/audio/reports/wp16.md:8-13` |
| Mac / PC | Beamer app, helper (WP10 soak) | app 4.6 / 1.6; helper 4.6 / 2.6 | Beamer Mac 399 -> 408 MB, PC private 77 -> 80 MB; helpers Mac 16.9 MB, PC 7.3 MB flat | 31 min | PRE-FINAL (`6487938`) | `dev/audio/reports/wp10.md:19` |
| Mac / PC | helper, 4 streams, null backends | Mac 2.5-7.8 % (5.7 % final), PC 3.0-4.9 % | - | WP2 acceptance | COMPONENT | `dev/audio/reports/wp2.md:18` |
| Mac / PC | AEC alone | Mac 2.0-2.9 %, PC 1.1-1.2 % | - | WP9 | COMPONENT | `dev/audio/reports/wp9.md:23` |
| Mac | helper, single mic send | 1.2-1.6 % (one run 2.9-3.3 %) | - | WP3 | COMPONENT | `dev/audio/reports/wp3.md:11` |
| Mac | tccd (side effect) | 3.6 % while mic permission undetermined (50 ms poll; WP18 moved to 2 s) | - | WP16 | PRE-FINAL | `dev/audio/reports/wp16.md:26`, `dev/audio/STATUS.md:91` |
| Mac | coreaudiod (system) | ~2.4 % idle; ~11 % with output device as tap clock (avoided) | - | Phase 1 tool | PROTO | `dev/audio/proto/mac/README.md:168-169`, `dev/audio/STATUS.md:150` |
| Mac | helper, video standby, idle | **0.08 %** (whole idle loop) | RSS 31.3-33.4 MB (15.5 MB before `video_warm`) + `VTEncoderXPCService` 26.8-33.4 MB | 60 s | COMPONENT (shipped standby) | `dev/camera/reports/cw3.md:67-68` |
| Mac / PC | helpers, camera stream open but 0 frames (product, bug) | Mac p50 0.5 %, max 1.0 %; PC p50 0.0 %, max 6.2 % | - | 1 s stats, 2026-11-12 | FINAL build, but "idle-stream numbers, not marks" | `dev/camera/reports/cw11.md:160` |
| PC | vcam sink, idle | 0.0-0.3 % | - | `scenario idlecpu` | COMPONENT | `dev/camera/reports/cw4.md:54` |
| Mac | helper, synthetic 720p30 camera send | **2.14 %** mean (1.99-2.53), synthetic drawing included | - | 30 s, real helper, 2 helpers on 127.0.0.1 | COMPONENT | `dev/camera/reports/cw3.md:35` |
| Mac | process with **real camera** (agent shell, not Beamer.app) | 12.8, 9.9, 8.8, 9.1 % (~9 %; suspected reaction-gesture detection; Beamer.app turns gestures off, never re-measured) | - | ~40 s | COMPONENT | `dev/camera/reports/cw3.md:105`, `:111-114` |
| PC | Frame Server (camera source DLL) | 1.44 % (cycles) / 1.15 % (times) at 1080p | - | CW5 | COMPONENT | `dev/camera/reports/cw5.md:49` |
| PC | software H.264 decode fallback | 14.7, 26.6, 27.0 % at 1080p; 10.4-15.3 % at 720p | - | CW4 `--sw` | COMPONENT (fallback only) | `dev/camera/reports/cw4.md:44` |
| Mac / PC | engine video overhead (mic+video minus mic, null codec included) | PC +0.31 to +0.93 %; Mac +0.81 / +1.04 % | - | 30-120 s | COMPONENT | `dev/camera/reports/cw2.md:56` |
| PC | `camrecv` (prototype receiver) | 3.1 % (720p) / 5.2 % (1080p) | private 69.3 -> 68.6 MB | 30 min soak | PROTO (C4) | `dev/camera/reports/c4.md:229`, `:235` |
| PC | Frame Server (prototype) | 0.8 % | 6.2 -> 6.0 MB | 30 min | PROTO | `dev/camera/reports/c4.md:230` |
| Mac | `camsend` + `VTEncoderXPCService` (prototype) | 1.3 % + 1.3 % (720p); 1.1 % + 1.2 % (1080p) | RSS 38.6 -> 41.1 MB; XPC 42.4 -> 42.5 MB | 30 min | PROTO | `dev/camera/reports/c4.md:232-235` |

No measurement exists of either Beamer app's CPU or memory while idle, or while streaming camera.

## 5. Network, RTT, input path

| Item | Value | Method / date | Source |
|---|---|---|---|
| Topology | Mac 192.168.1.138, PC `lebig` 192.168.1.134, both on home Wi-Fi, same AP; Tailscale 100.71.12.47 direct path also tested | 2026-11-07..11 | `dev/camera/reports/c4.md:4-5`, `dev/camera/reports/c3.md:5`, `dev/audio/proto/transport/README.md:153` |
| PC Wi-Fi adapter | MediaTek RZ616, "Power Saving: Auto", "U-APSD: off" (left unchanged) | `Get-NetAdapterAdvancedProperty` | `dev/audio/proto/transport/README.md:271-272`, `:325`, `dev/audio/STATUS.md:24` |
| Channel | "5 GHz ch 52, 160 MHz, -52 dBm" (uncongested); "DFS channel 52 at 160 MHz (radar events can stall the channel)" | Phase 1 W3 | `dev/audio/proto/transport/README.md:334-335`, `dev/audio/STATUS.md:24` |
| Mac Wi-Fi | on Wi-Fi; awdl0 active but not a contributor; en0 FQ-CoDel 0 drops | W3, C3 | `dev/audio/STATUS.md:206`, `dev/audio/proto/transport/README.md:329-332`, `dev/camera/reports/c3.md:95` |
| RTT, LAN (pings every 100 ms, 60 s runs) | min 3.0 / p50 5.1-5.7 / p95 6.6-8.9 / max 16-48 ms | T6, 2026-11-07 | `dev/audio/proto/transport/README.md:152` |
| RTT, Tailscale | ~3 / 5.6-5.8 / 8.3 / 16-22 ms | T6 | `dev/audio/proto/transport/README.md:153` |
| RTT during video runs (pings 5/s, 5-10 min) | min 2.5-3.4 / p50 5.2-7.3 / p99 31.1-38.8 ms | C3, 2026-11-11 | `dev/camera/reports/c3.md:67-77` |
| RTT, E2E harness ssh pings | WP10 min 4.1 ms; WP16 "p99 ... 19-21 ms" (as WP10/11) | 2026-11-08 | `dev/audio/reports/wp10.md:3`, `dev/audio/reports/wp16.md:21` |
| RTT, camera harness ssh pings (latest recorded) | min 3.63-5.28 / p50 6.02-6.34 / p99 36.19-68.33 ms; ~300-315 pings per run | 2026-11-12 19:53-20:00 | `dev/camera/e2e/runs/cam_latency_1112_195346/result.json`, `cam_latency_1112_200027/result.json`, `cam_start_1112_195859/result.json`, `cam_helper_kill_1112_195720/result.json` (key `clock`) |
| Wi-Fi stalls | RTT max 1.46 s once; 48-119 ms a few times per 15 min; one 0.66 s stall; 2-3.4 s episodes of 45-130 ms once per 5 min; C3: one 0.5 s stall (RTT 497 ms), one 3.6 s hold episode (packets 50-148 ms late), no 1.5 s stall in ~95 min | Phase 1 | `dev/audio/proto/transport/README.md:157`, `dev/audio/STATUS.md:207`, `dev/camera/reports/c3.md:84-87` |
| Mac->PC jitter vs PC transmitting (raw UDP, `jprobe.py`, 60 s) | spikes >10 ms/min: PC receive-only 20, 60, 59, 46; PC every 10 ms 7, 3, 2; 16 B/10 ms to router only: 1. PC->Mac p97 1.4-1.6 ms | W3 2026-11-07 | `dev/audio/proto/transport/README.md:312-319`, `:323-324` |
| Blind re-check | Mac->PC spikes/min ~39 (PC silent), ~24 (every 100 ms), ~13 (every 10 ms); PC->Mac 0-2 | V3, 15 x 65 s | `dev/audio/STATUS.md:122` |
| Media jitter p97 | at PC 5.4-6.7 ms vs at Mac 2.1-3.1 ms (LAN) | T6 | `dev/audio/proto/transport/README.md:152`, `:159` |
| Video frame network delay | p50 2.5-3.3, p99 22-34, max 59-156 ms (2.5-5 Mbit/s) | C3 | `dev/camera/reports/c3.md:13` |
| Audio-like one-way delay on the shared socket | p50 3.2 / p95 9.2 / p99 28.7 / p99.9 45 ms (max 137); 1 lost of 180,072 | C4 30-min soak | `dev/camera/reports/c4.md:223-224` |
| Mac sender capture -> sendto | 0.58 ms p50, 0.85 ms p99 | W3 | `dev/audio/proto/transport/README.md:298-299` |
| Clock drift Mac vs PC | +17.9 ppm (T6, 15 min); +17.3..+20.2 ppm (WP11 pings); 10.5 ppm (C4 soak), -6.5..+0.9 other runs; 19-25 ppm (C3); 12.07-18.24 ppm (2026-11-12 harness) | various | `dev/audio/proto/transport/README.md:227`, `dev/audio/reports/wp11.md:5`, `dev/camera/reports/c4.md:213-215`, `dev/camera/reports/c3.md:34`, camera `result.json` `clock.drift_ppm` |
| UDP blocked -> link fallback | product: `link` after 2.0 s, back to `up` 1.6 s after unblock (WP16); 1.3-2.3 s (WP10) | E2E | `dev/audio/reports/wp16.md:16`, `dev/audio/reports/wp10.md:13` |
| Input-path latency / `link timing` values | **none recorded**. Only mention: "fresh link timing lines both peers" (no values) | WP8 deploy check | `dev/audio/reports/wp8.md:29` |

## Gaps (what the ticket wants that nothing measured)

1. **Audio, final build**: no E2E at the delivered `7b83e71` (WP17 mono up-mix, WP18 AEC changes) nor after the
   merge into `main`; Mac mic -> PC shared (~110 ms expected) never measured.
2. **Audio, mouth-to-ear**: every product number stops at a digital tap/loopback and starts at a test source.
   No acoustic in/out (ADC, DAC, G733 radio, speakers) on the product; the real Mac mic was never forwarded in a
   measured run (permission pending at the time). No p99 for any audio E2E path.
3. **Camera**: no glass-to-glass at all; no product capture -> app latency, first-picture time, fps, soak,
   1080p or lip-sync result (CW11 blocked at 0 frames). The real camera's own delay is an assumption (25-70 ms).
4. **CPU / memory**: no idle numbers for either Beamer app or the audio helper on either machine; no Beamer app
   CPU/memory while the camera streams; real-camera helper CPU only from an agent shell (~9 %), never in Beamer.app.
5. **Input path**: no input latency, no `link timing` log values, no ping RTT of Beamer's own link (`ping_rtt`)
   recorded anywhere in `dev/`.
6. **Network**: Wi-Fi only (no wired, no 2.4 GHz); PC power saving never tested Off; no measurement on asusrog.
