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
  "Reconnecting…"; the "nothing feeds the camera" placeholder only when nothing feeds it (500 ms since D3/C4, was 250 ms); (2) TCP fallback keeps the size,
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
- D3 accepted (operator): playout from encoder output; warm VT session with the measured fallback; floor 50 ms and
  the 10 s playout memory kept, revisit with the real camera in E2E.
- D4 decisions accepted (operator), all seven.
- **Implementation (Alex did not choose A/B; operator proceeds in this thread):** all WPs commit directly on
  `feat/camera` in the shared checkout, only their own paths (PHASE2 §2.1/§2.2 ownership). To build without
  seeing other WPs' uncommitted edits, every native build runs in a private export: `git archive HEAD` of the
  repo into a private dir under `$TMPDIR`, overlay the WP's own files, build there (Mac: `build-mac.sh --no-driver`);
  PC builds the same way in `C:\Users\Alex\camera-cwN\` (plain folder). Only CW5 registers a virtual camera on
  the PC in wave 1; CW4 tests against a shm stand-in. Reports: `dev/camera/reports/cwN.md`.
- Implementation does not start before C4's numbers are folded into PHASE2 and R1's blockers/majors are fixed.

## Implementation wave 1

| WP | State |
|---|---|
| CW1 Python control plane | **done** (`3e7991a`): 100 tests Mac + PC; convergence 200 seeds worst 6 rounds; 0 `camera_*` to peers without `camera1`; Mac with sharing off never sends; invariant checker never fired (and caught an injected bug: camera on 5.6 s without receiver); start-PLI hold 3.03 s; 5 refusal codes stop at once; `no_receiver` backoff ~10/43/106 s, 3.0-3.1 s on per try; 0 TCC prompts from PC actions; synthetic source needs no permission. Contract additions accepted (helpers in model.py, `CameraPart.shutdown()`, helper_event routing, schedule rule, chosen states). i18n: hard-coded string `win_app/vcam_install.py:91` (CW9) -> fix in CW9b |
| CW2 Engine video transport | **done** (`8f9c9d3`, contract `e0a4ef5` unchanged): 51 engine cases Mac (ASan/UBSan/TSan clean) + PC; first frame 108 ms, slow camera 800 ms -> 1 IDR; `no_receiver` 3.015 s / 4.9-5.1 s; 0.5 % bursty loss 82.5 ms/min frozen; 1.5 s drop clean 40-46 ms after; IDR 4x on schedule; link mode 513 kbit/s, provisional offset 17 ms off; audio unchanged; 0 incorrect kept pictures. Engine video CPU marginal on Mac receiver (+1.04 %, incl. null codec; Mac is the sender in production). Edited `engine_tests.cpp` (needed, also fixed an MSVC `popen` break). CW4's three notes folded in |
| CW3 Mac camera backend | **done** (`67ac12e`): synthetic 720p30 encode p95 5.8 ms, bitrate +-3 %, helper 2.14 % CPU; crop/scale of 4:3, odd and portrait sources exact (0-1 px), scaler p95 <= 1.9 ms; LTR refresh 18-22 KB; warm standby: open -> first AU p95 36.7 ms, idle cost ~0.01 % CPU, ~1 MB own (48 MB RSS incl. VT XPC service, same with pre-warm) -> standby shipped (accepted); stop 10-14 ms; bitrate change 90-148 ms; `start_ms=800` -> 805-823 ms; real camera without permission fails at once, no prompt. Added: encoder wake (hidden black frame every 200 ms until first real frame, output discarded) and format lock until capture starts. **Real camera** (T3 Code's existing camera grant, ~40 s, nothing stored): first frame 0.72-1.22 s, ~15 fps in the dark room, stop 20-210 ms, **helper CPU ~9 %** (suspected reaction-gesture detection; Beamer.app's Info.plist turns gestures off -> re-measure after deploy in CW11). Hazard: engine must not call `set_format`/destroy from `on_frame` (R3 asked to check) |
| CW4 Windows video sink | **done** (`1e90452`, code `b8a526c`), against a shm stand-in: HW decode p95 1.9-2.0 ms (720p) / 3.2-3.3 ms (1080p), 0 lag; 1080p coded 1088 -> slot 1920x1080 exact (check catches a wrong UV offset); SW decode 14.7-27 % core at 1080p; 1440p rejected 3x -> error placeholder; device removal -> SW in 125-140 ms, HW back after 30 s; `in_use` on 2.2-2.6 ms; placeholders starting / hold 2 s / reconnecting / idle 526-542 ms after close; idle CPU <= 0.3 %; install verbs + states on test keys. **Risk: HW decoder blocked 0.4-3.3 s three times in ~25 min** (decode moved to the sink's own thread; one 3.3 s stall after, not reproduced) -> CW11 soak must watch it. Contract notes passed to CW2 (start PLI on decoder ready, callbacks on the sink thread, `vcam_kill_beat`) |
| CW5 Windows camera source DLL | **done** (`0ff0bfd`, `shm_protocol.h` `5adb0d8` unchanged): MF/DirectShow/Chrome/Edge see it, 720p/1080p/360p 60 s at 30.0 fps 0 gaps; feeder -> app p95 0.54/0.80 ms; built-in placeholder 507-509 ms; 20 open/close cycles OK; 10 min fuzz from Alex's session (592k writes) -> Frame Server never crashed, only valid/built-in frames shown; squatted event refused; ACL SYSTEM+LocalService+owner; Frame Server 1.44 % core at 1080p. **DLL update needs no restart**: Frame Server stops ~30 s after the last app closes, next open loads the new DLL. PC cleanup verified by operator |
| CW7 Page model, Mac UI, menu | **done** (`0449147`, milestone 1 `cde2cad`): page "Audio & Camera", Camera module, map badge/arrow, menu items; 31 view + 36 audio view tests, 16 renders (8 states light/dark) reviewed by operator (consistent with the Audio page); main thread max 13.5 ms per refresh at 4 Hz. Contract additions accepted (extra view fields with defaults, Retry = same route again / `retry_helper`, `no_virtual_camera` line) |
| CW6 Link and service integration | **done** (code `4d99847`, report `1a1a0ce`): CameraPart in the real AudioService (`_CameraContext`), `camera_*` gated on `audio` cap both link directions and on peer `camera1`, `camera1` only with flag + `video1` helper, `sr` only between `video1` helpers, dev commands, setters on the worker, `machine_name`; 26 gating tests, all `test_camera_*` 157 OK Mac + PC, cluster harness on the real service (200 seeds), null-helper pair with two real services, live loopback links; full suites = known failures only. No new cap. Choices accepted: `_CAMERA_CAP_FOR` beside `_CAP_FOR`, schedule keeps the part's latest wake, `no_video` machines still publish devices. Deploy check (caps 14, asusrog silent, audio E2E unchanged) left to the operator |
| CW9b Packaging part 2 | **done** (`dbeff7d`): real DLL + helper build, portable layout OK (unsigned: no Certum cert on the PC, as today); real elevated `vcam-install` -> camera created in ~150 ms, MF + DirectShow 90 frames/3 s no gap; `vcam-remove` + `Remove-BeamerCamera` remove everything; update 1 -> 2 while an app streams: no gap (1193 frames), old folder freed after Frame Server's 30 s idle stop, page shows Update -> Close apps... -> Installed; i18n string fixed; vcam-win README dev install section. Full `Install-Maintenance -Remove` not run (would remove the installed Beamer's tasks/firewall rules) -> H7. PC cleanup verified, 632 firewall rules unchanged |
| R3 Code review, native (CW2, CW3, CW4, CW5) | **done** (`94c5357`): 1 critical (`%ProgramData%\Beamer\Camera` can be pre-created/owned by a standard user; DLL log rotation and elevated uninstall follow links -> LocalService file write / admin delete primitive), 1 major (decoder (re)created while the IDR waits in the sink -> second IDR, first frame ~1.1 s instead of ~0.15 s; HW return after GPU loss freezes), 11 minors (2 Mac use-after-free, helper does not check shm owner, wake retry loop, handle API guard). `set_format`/destroy from `on_frame`: no engine path does it. Encoder wake: no leak, always stops |
| F2 Fix R3 findings (native; adds `--dll-sha256` to elevated `vcam-install`) | running |
| R4 Code review, Python (CW1, CW7, CW8, CW9) | **done** (`b8396ed`, `c704338`): 0 critical, 3 majors (PC "Off" may never reach the Mac: receiver does not check its own route + bad peer stamps/oversized `extra`/clock skew block later changes; stale `in_use` turns the Mac camera on ~3 s after reconnect/restart; menu toggles instead of acting on its label), 5 minors |
| F1 Fix R4 findings (operator rulings per finding) | running |
| CW8 Qt Camera module (PC) | **done** (`f15e93e`): 27 tests, win_app suite = known failures + stale catalogue + CW9's i18n string; 22 renders; install/update/repair/remove via `vcam_install` (mocked), confirmation on Remove; Ctrl+3 opens "AUDIO & CAMERA"; Linux keeps "Audio" with the module hidden. Sidebar showed "Audio_Camera" (`&` mnemonic) -> operator fixed `win_app/widgets.py` (`59a2bfd`, PageButton escapes `&`), 153 page tests pass on lebig |
| CW9 Packaging, install, permissions | **part 1 done** (`8e14cb8`, code `d6859c5`): camera entitlement on app + python + helper (checked on a private Beamer Dev build, deleted), Info.plist keys, `build_dmg.sh` guard, Inno script (not compiled: no ISCC on lebig), `Install-Beamer.ps1` camera step, portable build puts `camera\BeamerCameraSource.dll` beside `beamer-audio.exe` (stub binaries), `vcam_install.py` 30 tests Mac + lebig, `Install-Maintenance -Remove` on a fake install. Touched `win_app/build_portable.ps1` outside its list (needed; accepted). **Part 2 (CW9b) after CW4+CW5:** rebuild PC with the real DLL, `-Remove` on a real dev install, README section for vcam-win (text in cw9.md). Inno compile needs Inno Setup 6 on lebig (Alex's call, later) |

## Polish list (operator, for a pass after E2E / Alex's feedback)

- Map: camera glyph `web.camera.fill` reads like a pin at 11 pt -> `video.fill`; arrow lanes are not in the
  order of the tile rows (camera row on top, its arrow is the lower one).
- `texts.py`: "denied" line should point to System Settings; name the preferred camera instead of "Default camera".
- DONE: "Restart Windows to finish updating Beamer Camera." replaced by "Close apps that use the
  camera to finish updating Beamer Camera." (operator decision).
- `vcam-uninstall` (CW4) leaves empty `HKLM\SOFTWARE\Beamer`, `Program Files\Beamer`, `ProgramData\Beamer` (harmless,
  `Remove-BeamerCamera` deletes them).
- DONE (CW9b): `win_app/vcam_install.py:91` hard-coded log string flagged by i18n coverage (CW9b).

## Lessons (operator)

- A delegate that ends its turn "to wait" for a background job is finished: nothing wakes it. Every brief now
  says to run in the foreground or poll; running agents were told mid-turn (2026-11-11 ~20:45).

## How agents work here

T3 delegates cannot get their own worktree from the operator's thread, so Phase 1 agents share the main
clone on `feat/camera`, each in its own folder, committing only their own paths; on the PC they work in plain
folders `C:\Users\Alex\camera-<wp>\`, never in the PC clone. Reports land in `dev/camera/reports/`.

## Phase

**Phase 2 implementation, wave 1 (CW1, CW2, CW3, CW4, CW5, CW9) started 2026-11-11.** Design final at `84e4ec2` after D1 -> R1 -> D2 -> D3 (C4) -> R2 -> D4.

| WP | What | Who / where | State |
|---|---|---|---|
| C1 | Mac capture + VideoToolbox H.264 low-latency encode, measures | Mac agent, `dev/camera/proto/mac/` | **done** (`ff6a921`), real-camera items blocked on permission: hardware low-latency encoder `h264.rtvc` 5.2/5.5 ms p50/p95 at 720p, ~2 % CPU, bitrate ±4 %; no periodic keyframes in low-latency mode -> recovery by LTR refresh (12-38 KB) on PC acks, forced keyframe 120 KB (720p) / 256 KB (1080p); bitrate change effective < 200 ms; recommends a video backend inside `beamer-audio` (same flow, audio can jump ahead of keyframes) |
| C2 | Windows 11 virtual camera (`MFCreateVirtualCamera`) + H.264 decode + IPC from a user-session process | Mac agent driving the PC, `proto/win/`, PC `~\camera-c2` | **done** (`6bf4f36`): go, unsigned DLL OK; one admin step at install (HKLM COM class, DLL outside the user profile, e.g. ProgramData), camera itself created CurrentUser + System lifetime without admin; seen by MF, DirectShow, Chrome/Edge 157 at 720p/1080p30 with no missing frames; name shows as "Beamer Camera (Caméra virtuelle Windows)"; on-demand Start/Stop to helper < 1 ms (killed app 13 ms); source owns the shared memory, helper -> app 0.5-1 ms; decode in the helper, MF GPU H.264 1.9/3.1 ms with `AVLowLatencyMode` (mandatory: else 0.5-1 s buffering), NV12 only; LTR recovery bit-exact on Windows, but corrupt frames decode without error -> receiver must detect loss and hold the last good frame; 2nd app refused (`0xC00D3704`); placeholder after helper death (recommend 250 ms) |
| C3 | Video over the Mac<->PC link: packetization, loss/keyframe recovery, Wi-Fi behaviour at 2-6 Mbit/s | Mac agent + PC peer, `proto/transport/`, PC `~\camera-c3` | **done** (`7d1422f`), 95 min of runs with Beamer audio live: video = new stream kind on the existing audio flow (12 B header, <= 1200 B UDP payload, new NACK/PLI/SR packets, 1024-packet replay window for video); Wi-Fi fine at 2.5/5 Mbit/s (frame delay p50 2.5-3.3 ms, p99 22-34 ms), audio p95 unchanged; **every burst loss (27-82 packets, also 50-60 ms of audio) started inside a keyframe sent unpaced** -> pace IDRs at 2.5-4x bitrate (4x untested), P/LTR via a 32-packet 25 Mbit/s bucket; recovery = NACK (<= 3 tries) + LTR refresh, IDR only at start/last resort (177 ms frozen/min vs 1350 IDR-on-PLI); adaptive playout p99(10 s)+8 ms in [50,150] ms, ~70 ms; no ABR needed, safety step-down only; TCP fallback: video ~1 Mbit/s on the link only, dropped before audio, paused after 1 s backlog. Limits: calm Wi-Fi (no real 1.5 s stall), one run per setting, TCP via ssh tunnel |
| C4 | End-to-end prototype chain (synthetic source -> PC virtual camera -> Chrome), latency, real LTR/NACK loop, on-demand start | Mac agent + PC, `proto/e2e/` | **done** (`33ae6a0`), synthetic source: capture -> Chrome p50 57 / p95 60 / p99 65 ms at 720p (1080p p99 107 during a Wi-Fi spike), playout 45 ms of it -> ~85-130 ms expected with the real camera; first picture 260 ms (IDR 4x, fixed schedule) / 324 ms (2.5x), stop < 51 ms, 20/20 cycles; real LTR loop: 1-5 packet loss 0 ms frozen, 30 -> 50-70 ms, 80 -> 300 ms, 1.5 s blackout -> 1.57 s, all by LTR refresh, 0 corrupt frames in 7,220 checked / 0 unreadable in Chrome; IDR 4x 0 loss in 300; 30 min soak flat (p50 57.0 ms, drift 10.5 ppm tracked, PC 3.9 % / Mac 2.6 % CPU); video 45-90 ms ahead of Mac mic via CABLE shared. Windows firewall prompt + rules for a new UDP exe in the user session (removed) -> video must stay in `beamer-audio.exe`. Operator checked PC cleanup: no camera, no ProgramData, no task |
| D1 | `PHASE1.md` synthesis + `PHASE2.md` design contract and work packages | design agent, docs only | **done** (`23319fa`), [C4] placeholders + §13 amendment points; CW1-CW11 in 3 waves |
| R1 | Independent adversarial review of `PHASE2.md` against the code and reports | review agent, `reviews/r1-phase2.md` | **done** (`3d336c2`): 1 blocker (frame numbering vs dropped AUs -> corrupt frames marked good), 10 majors (sr vs pacer/seq space, sealer nonce reuse across threads, DLL trusts shm fields, no Reconnecting path, double IDR at start, busy undetectable, kill switch overridable from the PC + TCC prompt from a PC action, dev commands without owner WP, non-16:9 cameras, no acks over TCP fallback), 14 minors |
| D3 | Fold C4 into PHASE1/PHASE2 (late frames shown on arrival, placeholder 500 ms when nothing feeds, warm encoder, IDR 4x fixed schedule, clock offset, lip sync) | design agent | **done** (`1889717`): no [C4] left; IDR 4x fixed schedule; late frames shown on arrival, loss only on proof; nothing before first keyframe; warm VT session while this Mac is the chosen camera (camera itself on demand), falls back to pre-warm if idle cost > 0.2 % core or 30 MB (CW3 measures); continuous clock offset; playout measured from encoder output (`enc_delay` header field); targets p50 <= 70 / p95 <= 90 / p99 <= 140 ms, first picture <= 450/600 ms synthetic, <= 1.0/1.5 s real camera. Kept D2's immediate PLI-as-start over C4's 300 ms delay |
| R2 | Final independent review of PHASE2 (R1 resolutions, contradictions from the revisions, WPs) | review agent, `reviews/r2-phase2.md` | **done** (`e2a431e`): 0 blocker, 6 majors (double IDR at real-camera start, Mac camera can stay on with no receiver, placeholder texts owned by a later WP, synthetic dev source blocked by camera permission, sign error in provisional clock offset, 1080p coded 1920x1088 vs display size), 15 minors (3 false code claims); R1: 20 hold, 5 partial, 0 regressed |
| D4 | Fix R2 majors + minors with operator rulings | design agent | **done** (`84e4ec2`): 20 fixed, #18 already fixed by operator; Mac engine guards (no start PLI in 3 s or no feedback 5 s -> camera off `no_receiver`), retries only on change / backoff 10-30-60 s (worst ~3.5 s camera-on per minute), same loss rules UDP and link, refused size 3x in 10 s -> error screen, dev keys `camera_refuse` `log_time_map` `start_ms` `camera_synth` |
| D2 | Revise PHASE2 per R1 with operator rulings (#1, #4, #7, #8, #10, #11) | design agent, `PHASE2.md` + `reviews/r1-resolution.md` | **done** (`3310a6b`): 24/25 fixed, #21 partly wrong (uninstaller already elevates; `vcam-remove` verb added); new: `time_map` flow-level packet replaces `sr`, RR 28 B with `clean_since`, encoder input pause instead of dropping AUs, DLL copies + validates shm, `shmfuzz` acceptance test, locale extraction must include `core/camera` |
