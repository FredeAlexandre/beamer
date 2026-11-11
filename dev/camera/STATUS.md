# Camera across Beamer: status board

Operator's working memory for the camera mission (Mac camera -> PC apps). Updated at every significant
step. Branch: `feat/camera` (from `feat/audio` @ `12f18d2`, so it reuses the audio engine, link and pages).
Same method as audio (`dev/audio/`): Phase 1 prototypes and measures, Phase 2 design + integration work
packages, Phase 3 E2E/QA, then a test guide for Alex.

## Goal (Alex, 2026-11-11)

The Mac's camera is usable on the PC as if it were plugged into it: a "Beamer Camera" in PC apps (Teams,
Zoom, Discord, browsers, Camera app), built the same way as the audio feature he uses daily.

## Operator defaults (Alex did not choose; revisit with his feedback)

- Camera: the Mac's built-in "MacBook Air Camera" (the only one present); any AVFoundation camera must work.
- Quality: 720p30 by default, 1080p30 as a setting. H.264 (VideoToolbox on the Mac).
- PC side: Windows 11 native virtual camera (`MFCreateVirtualCamera`, PC is build 26200), no third-party driver.
- On demand: the Mac camera is on only while a PC app has "Beamer Camera" open.
- Out of scope for now: PC -> Mac (needs an Apple-signed Camera Extension; the PC has no camera anyway), Linux.

## En attente d'Alex

1. **Camera permission for the measurements on the real camera (C1, optional, ~3 min).** C1's request went out
   in the name of T3 Code (the app agents run under); a prompt may be on the Mac's screen. Allowing it lets an
   agent run `dev/camera/proto/mac/experiments/camera_suite.sh` (cold start, capture latency, sharing with
   another app); revoke afterwards with `tccutil reset Camera com.t3tools.t3code`. Alternative: skip, and
   measure once Beamer.app itself asks for Camera in Phase 2. Then, with someone present: cover the lens
   (`experiments/low_light.sh`), toggle Video Effects in Control Center during a capture.
2. **PC, not now (Phase 3 test guide):** the Windows Camera app, Teams and Discord ask camera permission on
   first use, so seeing "Beamer Camera" there needs a person; also the "Allow multiple apps to use camera"
   switch, and the camera still listed after a logoff/reboot. C2's prototype is fully removed (operator
   checked: no camera device, no `C:\ProgramData\BeamerCamC2`, no task).

## Phase 2 design decisions (operator, 2026-11-11)

- D1's four deviations accepted: (1) during a Wi-Fi gap on a live stream, hold the last frame up to 2 s, then
  "Reconnecting…"; the 250 ms placeholder only when nothing feeds the camera; (2) TCP fallback keeps the size,
  1 Mbit/s 15 fps; (3) video sending starts before `camera_opened`, gated on the PC's first PLI; (4) the helper
  keeps the name `beamer-audio` (the PC firewall grant is on that name).
- D1's open questions, operator defaults (Alex may override): page renamed "Audio & Camera" (same key, place,
  shortcut); one quality for the whole cluster, 720p30 default; no 30 fps pinning in low light until Alex's
  test; "Stop Sharing the Camera" on the Mac turns the camera route off cluster-wide (privacy kill switch owned
  by the camera's machine); the Windows multi-app camera setting is only a help link, no automation.
- D2 decisions accepted (operator): one encode session per receiving PC (same size cluster-wide, independent
  recovery); the Mac kill switch leaves the route alone and every page shows "Turned off on <Mac>" (replaces the
  earlier cluster-wide-off default); choosing its own camera on the Mac page counts as the Mac action that may
  trigger the TCC prompt; a DLL update may need a Windows restart until CW5 measures otherwise; `locales/` is
  regenerated only at merge time.
- Next: fold C4's numbers into PHASE2 (§13), then one independent re-check (R2) of the whole contract.
- Implementation does not start before C4's numbers are folded into PHASE2 and R1's blockers/majors are fixed.

## How agents work here

T3 delegates cannot get their own worktree from the operator's thread, so Phase 1 agents share the main
clone on `feat/camera`, each in its own folder, committing only their own paths; on the PC they work in plain
folders `C:\Users\Alex\camera-<wp>\`, never in the PC clone. Reports land in `dev/camera/reports/`.

## Phase

**Phase 1 done 2026-11-11 (C1-C4). Phase 2 design: D1 -> R1 review -> D2 fixes -> D3 (C4 fold) running -> R2 re-check -> implementation.**

| WP | What | Who / where | State |
|---|---|---|---|
| C1 | Mac capture + VideoToolbox H.264 low-latency encode, measures | Mac agent, `dev/camera/proto/mac/` | **done** (`ff6a921`), real-camera items blocked on permission: hardware low-latency encoder `h264.rtvc` 5.2/5.5 ms p50/p95 at 720p, ~2 % CPU, bitrate ±4 %; no periodic keyframes in low-latency mode -> recovery by LTR refresh (12-38 KB) on PC acks, forced keyframe 120 KB (720p) / 256 KB (1080p); bitrate change effective < 200 ms; recommends a video backend inside `beamer-audio` (same flow, audio can jump ahead of keyframes) |
| C2 | Windows 11 virtual camera (`MFCreateVirtualCamera`) + H.264 decode + IPC from a user-session process | Mac agent driving the PC, `proto/win/`, PC `~\camera-c2` | **done** (`6bf4f36`): go, unsigned DLL OK; one admin step at install (HKLM COM class, DLL outside the user profile, e.g. ProgramData), camera itself created CurrentUser + System lifetime without admin; seen by MF, DirectShow, Chrome/Edge 157 at 720p/1080p30 with no missing frames; name shows as "Beamer Camera (Caméra virtuelle Windows)"; on-demand Start/Stop to helper < 1 ms (killed app 13 ms); source owns the shared memory, helper -> app 0.5-1 ms; decode in the helper, MF GPU H.264 1.9/3.1 ms with `AVLowLatencyMode` (mandatory: else 0.5-1 s buffering), NV12 only; LTR recovery bit-exact on Windows, but corrupt frames decode without error -> receiver must detect loss and hold the last good frame; 2nd app refused (`0xC00D3704`); placeholder after helper death (recommend 250 ms) |
| C3 | Video over the Mac<->PC link: packetization, loss/keyframe recovery, Wi-Fi behaviour at 2-6 Mbit/s | Mac agent + PC peer, `proto/transport/`, PC `~\camera-c3` | **done** (`7d1422f`), 95 min of runs with Beamer audio live: video = new stream kind on the existing audio flow (12 B header, <= 1200 B UDP payload, new NACK/PLI/SR packets, 1024-packet replay window for video); Wi-Fi fine at 2.5/5 Mbit/s (frame delay p50 2.5-3.3 ms, p99 22-34 ms), audio p95 unchanged; **every burst loss (27-82 packets, also 50-60 ms of audio) started inside a keyframe sent unpaced** -> pace IDRs at 2.5-4x bitrate (4x untested), P/LTR via a 32-packet 25 Mbit/s bucket; recovery = NACK (<= 3 tries) + LTR refresh, IDR only at start/last resort (177 ms frozen/min vs 1350 IDR-on-PLI); adaptive playout p99(10 s)+8 ms in [50,150] ms, ~70 ms; no ABR needed, safety step-down only; TCP fallback: video ~1 Mbit/s on the link only, dropped before audio, paused after 1 s backlog. Limits: calm Wi-Fi (no real 1.5 s stall), one run per setting, TCP via ssh tunnel |
| C4 | End-to-end prototype chain (synthetic source -> PC virtual camera -> Chrome), latency, real LTR/NACK loop, on-demand start | Mac agent + PC, `proto/e2e/` | **done** (`33ae6a0`), synthetic source: capture -> Chrome p50 57 / p95 60 / p99 65 ms at 720p (1080p p99 107 during a Wi-Fi spike), playout 45 ms of it -> ~85-130 ms expected with the real camera; first picture 260 ms (IDR 4x, fixed schedule) / 324 ms (2.5x), stop < 51 ms, 20/20 cycles; real LTR loop: 1-5 packet loss 0 ms frozen, 30 -> 50-70 ms, 80 -> 300 ms, 1.5 s blackout -> 1.57 s, all by LTR refresh, 0 corrupt frames in 7,220 checked / 0 unreadable in Chrome; IDR 4x 0 loss in 300; 30 min soak flat (p50 57.0 ms, drift 10.5 ppm tracked, PC 3.9 % / Mac 2.6 % CPU); video 45-90 ms ahead of Mac mic via CABLE shared. Windows firewall prompt + rules for a new UDP exe in the user session (removed) -> video must stay in `beamer-audio.exe`. Operator checked PC cleanup: no camera, no ProgramData, no task |
| D1 | `PHASE1.md` synthesis + `PHASE2.md` design contract and work packages | design agent, docs only | **done** (`23319fa`), [C4] placeholders + §13 amendment points; CW1-CW11 in 3 waves |
| R1 | Independent adversarial review of `PHASE2.md` against the code and reports | review agent, `reviews/r1-phase2.md` | **done** (`3d336c2`): 1 blocker (frame numbering vs dropped AUs -> corrupt frames marked good), 10 majors (sr vs pacer/seq space, sealer nonce reuse across threads, DLL trusts shm fields, no Reconnecting path, double IDR at start, busy undetectable, kill switch overridable from the PC + TCC prompt from a PC action, dev commands without owner WP, non-16:9 cameras, no acks over TCP fallback), 14 minors |
| D3 | Fold C4 into PHASE1/PHASE2 (late frames shown on arrival, placeholder 500 ms when nothing feeds, warm encoder, IDR 4x fixed schedule, clock offset, lip sync) | design agent | started |
| D2 | Revise PHASE2 per R1 with operator rulings (#1, #4, #7, #8, #10, #11) | design agent, `PHASE2.md` + `reviews/r1-resolution.md` | **done** (`3310a6b`): 24/25 fixed, #21 partly wrong (uninstaller already elevates; `vcam-remove` verb added); new: `time_map` flow-level packet replaces `sr`, RR 28 B with `clean_since`, encoder input pause instead of dropping AUs, DLL copies + validates shm, `shmfuzz` acceptance test, locale extraction must include `core/camera` |
