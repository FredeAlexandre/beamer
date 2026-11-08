# WP4 report: Windows backend + helper build (lebig, 2026-11-08, session 0 over SSH)

**Done.** `native/audio/win/`: WASAPI `Backend` (inventory and notifications debounced 300 ms and de-duplicated; mic capture; process loopback excluding one tree; endpoint mute with journal and guard; endpoint-loopback reference with keep-alive; render to an endpoint or CABLE Input; demand from CABLE Output sessions; forms; default following; invalidation recovery; MMCSS), `blocklist.inc`, `main.cpp`, `backend_check.exe` harness, `README.md`; `native/audio/build-win.cmd` (+ `win/build.ps1`); `native/audio/engine/backend.h` = byte-identical copy of §2.4 (checked with `cmp`).
**Blocklist (first task).** Wave Link: Personal/Stream/Chat Mix (capture), System, Wave 3 (render), all on `TUSBAUDIO_ENUM\VID_0FD9&PID_0096` under `ROOT\ELGATO_ROOT_DEVICE`: blocked by name, adapter, ancestor, and the generic "Elgato + virtual" rule. The physical Wave:3 (`PID_0070`, USB chain) is allowed. No Elgato device was opened, muted or changed.

## Acceptance (all measured on lebig, session 0)
| Test | Result |
|---|---|
| `list`: Wave:3, G733, DualSense; no Elgato virtual, no CABLE Output/16ch | PASS (CABLE Output only as role VirtualMic, 16ch hidden) |
| journal: kill the helper while muting -> restored exactly, all 14 active endpoints compared | PASS: guard restores in 29 ms; with guard also killed, the next start's `restore_journal()` restores 1 endpoint exactly |
| loopback with the endpoint muted keeps -13.98 dBFS | PASS: process loopback -13.979, endpoint loopback -13.977 |
| helper's own tone absent from its loopback (<= -90) | PASS: -200 dBFS (digital zero) |
| demand true <= 300 ms after a recorder opens CABLE Output | PASS: 20-26 ms (off 3-24 ms after stop; raw, no hold) |
| static x64 exe | PASS for `backend_check.exe` (build checks dumpbin: x64, no CRT DLLs). `beamer-audio.exe` NOT LINKED: engine absent |
| firewall prompt on a 24823 bind (risk 1) | NOT RUN: session 0, per brief; operator tests it through Beamer |
| user session via scheduled task | NOT RUN: no scheduled tasks allowed; everything ran in session 0 |
| extra: render unity (mono/stereo), vmic tone -20.00 at CABLE Output, invalidation recovery (injected + real one caused by another process's exclusive open), `stream_lost` after 3.08 s, `device_busy` | PASS |
| stretch: exclusive render into CABLE Input | 43-45 ms against 103-111 ms shared (click to an app's capture). 0 holes in 4x60 s, 1 in a 300 s soak. Opt-in `--vmic-exclusive` |
Endpoint state after all tests = baseline (14 endpoints, 0 differ). No leftover processes.

## Deviations from PHASE2
- `main.cpp` is ~15 lines, not 3: it runs the guard (`beamer-audio _guard`), passes a UTF-8 argv (CommandLineToArgvW), and sets stdio to binary.
- `make_win_backend(WinOptions)` + `win_options_from_args()`: §2.4 has no way to pass `--state-dir` to a backend.
- Shared render into CABLE Input queues 3 periods (+10 ms): 2 periods gave 1 hole per 60 s in 3 runs out of 4.
- `devices_changed` is immediate (not debounced) when only `vmic_in_use` changes, for demand latency.
- Forms: `FormFactor` refined by names (the G733 reports "Speakers"; adapter "...Headset" -> headset).
- `channels` = device format (`PKEY_AudioEngine_DeviceFormat`, read without opening a client), not the mix (Wave:3: 1, mix 2).

## For other WPs (contract issues; backend.h left unchanged)
1. **WP2**: `backend.h` as specified has no includes, no include guard and no SPDX line. Consumers must include `<string> <vector> <memory> <cstdint>` first, exactly once. Suggest adding them plus `#pragma once`.
2. **WP2**: `*err` convention used here: `"<§3.3 code>: message"` (`no_device`, `blocked_device`, `no_virtual_mic`, `device_busy`, `permission`, `bad_args`, `unsupported`, `internal`). The engine should map the prefix.
3. **WP2**: the snapshot lists CABLE Output (VirtualMic) and CABLE Input (VirtualMicFeed); the engine must drop role != Normal from `devices`. Blocked or VB-CABLE defaults are reported as "".
4. **WP2**: `vmic_in_use` is raw; the 3 s off-hold of §3.3/§6.3 must be in the engine (not doubled in the backend).
5. **WP2**: `stream_lost` arrives on the backend worker thread; destroying the handle inside the callback is safe. All streams must be destroyed before the backend.
6. **WP2**: `on_render` frame counts vary (shared: up to 2-3 periods; exclusive: 96 every 2 ms). `host_ns` = QPC ns (capture: device QPC of the first frame, loopback ~10 ms in the future per Phase 1; render: now + padding).
7. System capture: `CaptureSpec.device` = endpoint to mute; process loopback captures every endpoint; only `exclude_pids[0]` is used (one tree on Windows). Features reported: `system_capture local_mute_endpoint mic_demand virtual_mic aec`.
8. **WP8/operator**: `build-win.cmd` writes `native/audio/build/.gitignore` (`*`) until WP2's root ignore entry lands. Engine linkage (sources.txt paths, opus `*.mk` parsing, monocypher/speexdsp) is written but untested: run `native\audio\build-win.cmd` after merging the engine.
9. Decision for Alex: make exclusive vmic render the default? It saves ~60 ms but takes CABLE Input from any other app, and showed 1 hole in 5 min against 0 for shared.

## Commits (branch pc/wp4, not pushed)
- bb3a12c Add the Windows audio backend, its test harness and build script
- b0040c9 Add opt-in exclusive render into CABLE Input and harden the Windows backend
- (this report) Write the WP4 report
