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

## How agents work here

T3 delegates cannot get their own worktree from the operator's thread, so Phase 1 agents share the main
clone on `feat/camera`, each in its own folder, committing only their own paths; on the PC they work in plain
folders `C:\Users\Alex\camera-<wp>\`, never in the PC clone. Reports land in `dev/camera/reports/`.

## Phase

**Phase 1: prototypes, started 2026-11-11.**

| WP | What | Who / where | State |
|---|---|---|---|
| C1 | Mac capture + VideoToolbox H.264 low-latency encode, measures | Mac agent, `dev/camera/proto/mac/` | **done** (`ff6a921`), real-camera items blocked on permission: hardware low-latency encoder `h264.rtvc` 5.2/5.5 ms p50/p95 at 720p, ~2 % CPU, bitrate ±4 %; no periodic keyframes in low-latency mode -> recovery by LTR refresh (12-38 KB) on PC acks, forced keyframe 120 KB (720p) / 256 KB (1080p); bitrate change effective < 200 ms; recommends a video backend inside `beamer-audio` (same flow, audio can jump ahead of keyframes) |
| C2 | Windows 11 virtual camera (`MFCreateVirtualCamera`) + H.264 decode + IPC from a user-session process | Mac agent driving the PC, `proto/win/`, PC `~\camera-c2` | started 16:05 |
| C3 | Video over the Mac<->PC link: packetization, loss/keyframe recovery, Wi-Fi behaviour at 2-6 Mbit/s | Mac agent + PC peer, `proto/transport/`, PC `~\camera-c3` | started 16:05 |
| C4 | End-to-end prototype chain, glass-to-glass latency (after C1-C3) | | not started |
