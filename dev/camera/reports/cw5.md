# CW5: Windows camera source DLL (`BeamerCameraSource.dll`)

Date 2026-11-11. Branch `feat/camera`. Work package PHASE2 §12 CW5 (§4.6.2-4.6.4, §9.2, §10.4). PC `lebig`
(Windows 11 Pro 25H2, build 26200, MSVC Build Tools, SDK 10.0.26100), built and tested in a plain folder
`C:\Users\Alex\camera-cw5\` from `git archive` exports, never from a checkout. The installed Beamer was not
touched. Alex's session (console, session 1) was signed in throughout (H8 held); every "in session" run below
went through a temporary scheduled task (`tools/insession.ps1`), unelevated, as the helper runs.

## Files (all under `native/audio/vcam-win/`)

| File | What |
|---|---|
| `shm_protocol.h` | Contract, verbatim from §4.6.2. **First commit `5adb0d8`; unchanged since.** |
| `source.cpp` (1,109 lines with comments), `source.def` | The DLL (activate -> source -> one shared NV12 stream, objects, demand, validation, delivery). |
| `builtin_image.h`, `placeholder_builtin.inc` | Built-in image: a 1920x1080 PNG (47 KB, Beamer ground, the Beamer mark, "Beamer Camera" / "Waiting for Beamer" in Hanken Grotesk, OFL) decoded with WIC, scaled (Fant) and converted to NV12 (BT.601 limited) once per size; the moving dot. |
| `build.ps1` | DLL -> `<out>\camera\BeamerCameraSource.dll`, tools -> `<out>\*.exe` (default out `native\audio\build\win`); static CRT, refuses MSVC runtime imports; runs inside vcvars or finds MSVC itself. DLL imports: MFPlat, ole32, ADVAPI32, KERNEL32 (277 KB). |
| `README.md` | Behaviour, update rule, test recipes; CW9's dev-install section left as a stub for CW9. |
| `tools/camprobe.cpp` | Consumer: `list`, `read` (frame classifier: `helper` C2 stamp, `slot3`, `c1` code strip with `--strip c1`, `builtin`, `uniform`, `other`; `--events`, `--dump`, `--per-second`), `yuvcheck` (DirectShow via ffmpeg). |
| `tools/feeder.cpp` | Helper stand-in: `run` (Demand with owner check, open on demand / 1 s poll, beat from the presenting loop, live or waiting pictures, `--beat-stop-at`, `--waiting-at`, `--follow`), `vcam create/remove`, `inspect` (owners + DACL SDDL + header), `squat [--open]`, `loadtest`, `cpu`. |
| `tools/shmfuzz.cpp` | Hostile writer (below). |
| `tools/tool_common.h` | Names, clocks, C2 stamp, C1 code-strip reader, seqlocked slot writer. |
| `tools/make_builtin.py` | Mac generator of `placeholder_builtin.inc` (PyObjC, app venv). |
| `tools/devinstall.ps1`, `devuninstall.ps1` | Elevated dev stand-ins for `vcam-install` / `vcam-uninstall` (§9.2 paths, keys, ProgramData ACL). |
| `tools/insession.ps1` | Run a command in the signed-in user's session from ssh. |
| `tools/accept/*.ps1` | The exact drivers behind every number in this report. |

Commits: `5adb0d8` (contract), `dffd9a0` (DLL + tools), `9001006` (README, feeder), and the final commit with
this report and `tools/accept/`.

## Acceptance

All runs: DLL installed with `tools/devinstall.ps1` (CW4's verb not yet available), camera created unelevated
in Alex's session with `feeder vcam create` (System lifetime, CurrentUser access), feeder and consumers in the
session unless noted. Latency = consumer's wall clock at `ReadSample` return minus the time stamped into the
picture by the feeder just before it wrote the slot.

| Criterion | Result | Command / numbers |
|---|---|---|
| MF, DirectShow, headless Chrome/Edge see "Beamer Camera" | **Pass** | `camprobe list`: MF and DirectShow both list "Beamer Camera (Caméra virtuelle Windows)" (Windows' suffix, as C2). DirectShow `ffmpeg -f dshow` -> `camprobe yuvcheck`: 720p 300 frames 30.10 fps 0 gaps, 1080p 300 frames 30.16 fps 0 gaps (latency p50 34.6 / 36.0 ms: the DirectShow bridge's frame, as C2's 34.5). Chrome 157 / Edge 157 headless `getUserMedia` (`dev/camera/proto/win/web/gum.html`): 720p and 1080p, 301 frames, 30 fps, 0 gaps, 0 unstamped, open 73-79 ms, latency p95 2.9-3.1 ms (`accept/t_consumers.ps1`). |
| 720p / 1080p / 360p NV12 @30 for 60 s, 0 gaps | **Pass** | `accept/t_sizes.ps1`: 1280x720 1799 frames 30.01 fps **0 gaps**; 1920x1080 1802 frames 30.02 fps **0 gaps**; 640x360 1802 frames 30.02 fps **0 gaps** (1-2 "dups" each = one repeat when a feeder frame came > 50 ms late; 1 built-in frame each = the first frame before the helper's first picture). |
| `feeder` -> app p95 <= 1.0 ms (720p) / 1.5 ms (1080p) | **Pass** | 720p p50 0.41 / **p95 0.54 ms**; 1080p p50 0.69 / **p95 0.80 ms**; 360p p50 0.23 / p95 0.30 ms (max 63-89 ms: single scheduling outliers, 1 per run). |
| Built-in placeholder 500 ± 50 ms after the heartbeat stops | **Pass** | `accept/t_thresh.ps1`, `feeder --beat-stop-at 4` (beat and writes stop, process alive): first built-in frame **509.2, 508.8, 509.0, 508.7, 507.2 ms** after the last beat (5/5). |
| `mode` waiting -> slot 3 within one frame | **Pass** | `feeder --waiting-at 4`: first slot-3 frame **1.15, 1.37, 1.16, 1.08, 1.25 ms** after the mode switch (5/5; one frame = 33 ms). |
| Repeated Stop/Shutdown idempotent (state_seq increments, no crash) | **Pass** | `accept/t_storm.ps1`: 20 open/close cycles (separate processes, 0.5 s each): 20/20 ok; the feeder saw 77 publications (20 `streaming:true`, 57 `streaming:false`: every Stop and every Shutdown of Frame Server's 2-3 re-created sources publishes), `state_seq` 2 -> 106 strictly increasing, last state `streaming:false`, Frame Server PID unchanged. In-process (`feeder loadtest`): Start, Stop, Stop, Shutdown, Shutdown -> `S_OK, S_OK, S_OK, S_OK, MF_E_SHUTDOWN`. |
| `shmfuzz` from Alex's unelevated session, every helper field, 1 kHz, 10 min, `camprobe` reading: Frame Server never crashes/restarts, every frame built-in / previous good / valid slot, `rejected` rises | **Pass** | `accept/t_fuzz.ps1`: 600 s, **591,931 ticks (987 Hz)**, 354,889 fuzz ticks and 7,114 legitimate frames in 477 alternating phases (1.5 s fuzz / 1 s legit). Frame Server PID 18116 before and after, 0 PID changes in 10 s polls, service Running, **no Application 1000-1002 nor System 7031/7034/7024 events**. `camprobe` 19,944 frames: **builtin 11,711, helper (stamped picture, fresh or previous) 7,113, uniform 1,120 (accepted fuzzed slots: value 0x50 = a live slot, 0x60 = slot 3), other 0**. **`rejected` +253,158.** Frame Server CPU during the fuzz 1.33 % of a core (frame event set every tick: the 2 ms listen throttle holds). The log stayed at <= 1 line/s ("+N lines suppressed"). |
| DLL loaded in a user process creates no kernel object and no log | **Pass** | `feeder loadtest` (Frame Server stopped first): types exactly `kOffered`, `objects_while_loaded: false`, log 13,778 -> 13,778 bytes. `feeder vcam create` from the session (MFCreateVirtualCamera loads the DLL in the caller to validate it): the 3 new log lines all carry the Frame Server PID (24024), none the caller's. |
| A foreign pre-created `.Frame` event is refused | **Pass** | `feeder squat` (user's default DACL, LocalService not granted): source `frame event create failed (err 5)`, refuses, **148/148 frames built-in**; `feeder squat --open` (everyone full access, so the source gets `ERROR_ALREADY_EXISTS`): owner check -> `squatted frame event (err 183)`, refuses, **118/118 built-in**, section not left behind. After the squatter exits: live again (118 helper frames). |
| Objects' DACL = SYSTEM, LocalService, owner SID | **Pass** | `feeder inspect` while streaming: `.Shm` `O:LS D:P(A;;…;;;SY)(A;;…;;;LS)(A;;…;;;S-1-5-21-…-1001)`, `.Frame` `O:LS D:P(A;;0x1f0003;;;SY)(A;;0x1f0003;;;LS)(A;;0x1f0003;;;S-1-5-21-…-1001)` (protected, no inherited ACEs); `.Demand` (feeder) owner = Alex, same three ACEs. |
| Frame Server CPU <= 1.5 % of a core at 1080p | **Pass (close)** | `feeder cpu <FrameServer pid> 30` during the 1080p run: **1.44 % (cycles) / 1.15 % (times)**; C2 measured 0.93-1.34 % with the same copy path. The DLL copies each picture twice (slot -> private buffer -> sample) where C2 copied once: the price of rule 4. |
| Unregister + delete leaves no `SWD\VCAMDEVAPI` node | **Pass** | `feeder vcam remove` + `devuninstall.ps1`: no `SWD\VCAMDEVAPI*` node in any state, MF lists no camera (PC cleanup below). |
| DLL update measured | **Measured** | No Windows restart needed: Frame Server exits 30.3-30.7 s after the last app closes and the next open loads the re-pointed DLL; old DLL only while a camera stays in use (section below). |
| Media types an app sees are exactly `kOffered` | **Pass** | Every `camprobe read`: `["NV12 1280x720@30/1","NV12 1920x1080@30/1","NV12 640x360@30/1"]`, in that order (MF source reader). DirectShow adds YUY2 by itself in its bridge (C2), not offered by the source. |

Not run in this WP (belong to CW4 / §10.4 with the real helper): C1's H.264 recordings through `recv`, the 1080p
round trip (coded 1920x1088). CW5's tools are ready for it, but `camprobe --strip c1` (C1 code-strip reader, per `proto/mac/README.md`) was not exercised on a real C1 stream here: first use is CW4/CW11's.

## DLL update: does it need a restart? (§9.2 measurement)

Method (`accept/t_update.ps1`, `accept/t_idleexit.ps1`): v1 installed in `%ProgramFiles%\Beamer\Camera\1\`,
used by an app; then `devinstall.ps1 -Version 2` copies the DLL to `...\Camera\2\` and re-points the CLSID
(`InprocServer32`, `DllPath`, `DllVersion`), as `vcam-install` does for a new version. Which file runs is read
from Frame Server's loaded modules and from the DLL's own `loaded from <path>` log line (both binaries have
the same `kDllVersion`, so `Header.dll_version` cannot tell them apart in this test; with a real version bump
`loaded_version` vs `DllVersion` shows the same thing).

| When | Frame Server | DLL running |
|---|---|---|
| App used v1, app closed (5 s later) | pid 15728 running | `...\Camera\1\` |
| Right after the re-point | pid 15728 | `...\Camera\1\` |
| An app opens the camera again within seconds | pid 15728 (still the same process) | **`...\Camera\1\` (old)** |
| App closed, camera idle: 1, 2, ... 15 min | **not running** at every 1-minute check | - |
| Next app open after the idle period | new pid 5304 | **`...\Camera\2\` (new)** |
| `Restart-Service FrameServer` over ssh, then an app open | new pid 6556 | `...\Camera\2\` |
| After a Windows restart | not measured (H9) | expected new (Frame Server starts fresh) |

**How long Frame Server stays after the last app closes** (`t_idleexit.ps1`, 5 runs): it stops itself
**30.3-30.7 s** after the app closed the camera, with or without a helper (feeder) holding the shared section
open (3 runs without, 2 with; the helper's handle keeps the section object, not the service). The DLL is then
unloaded and its file unlocked.

**Answer: an update does not need a Windows restart on this PC.** The new DLL runs from the first camera open
that comes **more than ~30 s after every app has stopped using the camera** (Frame Server is demand-start and
exits when idle). It keeps the old DLL only while some app keeps a Frame Server camera open (any camera, not
just Beamer Camera) or reopens one within 30 s. Consequences for §9.2 / the UI wording (operator decision, not
changed here): "Restart Windows to finish updating Beamer Camera." is wrong in the common case; suggested
wording when `loaded_version != DllVersion`: "Close apps that use the camera to finish updating Beamer Camera."
(the mismatch resolves itself ~30 s after they close; a dev agent can also `Restart-Service FrameServer`).
`vcam-uninstall` and the stale-folder cleanup will usually find the old DLL unlocked; stopping FrameServer is
needed only while an app has a camera open. The reboot row stays for H9.

Note for CW4 that follows from this: when Frame Server exits, the helper's open handle keeps the old section
alive and the next Frame Server instance re-opens it (owner LocalService, accepted). With a future layout
change (`kVersion` bump) the helper should drop its mapping when `source_pid` no longer names a live process
and reopen, so the new DLL creates a fresh section.

## How the DLL is built (for review)

- **Process check**: `TokenUser == LocalService` decides everything; otherwise inert (no objects, no log, no
  registry read). The check runs at source activation.
- **Objects** are created by the first source object of the process and closed with the last (Frame Server
  re-creates and shuts down its source 2-3 times on every close, so between app sessions they normally do not
  exist; seen in the log as `ipc: closed` / `section created`). When the helper still holds them open the next
  source object gets `ERROR_ALREADY_EXISTS`, checks the owner (LocalService) and logs `reopened`.
- **Delivery**: a fresh helper picture goes out as soon as a request is pending (woken by the frame event, at
  most once per 2 ms); placeholders and repeats come from a 30 fps **high-resolution waitable timer** (Frame
  Server runs at the default 15.6 ms timer resolution: plain wait timeouts gave 21.6 fps in the first build).
  A live picture is repeated if no newer one arrives within 1.5 frame periods (`repeats`).
- **Validation** exactly as §4.6.3 rules 1-5; an unchanged slot (same index, `seq`, `written_qpc`) is not
  re-copied (its previous outcome is reused), so a stuck invalid slot costs one rejection, not 30/s.
- Counters `delivered`, `placeholders` (built-in + slot 3), `repeats`, `rejected` are cumulative for the
  section's life (they survive source re-creation while the helper holds the section).

## Contract issues

No change to `shm_protocol.h`. Choices where the contract is silent or loose (all consistent with it):

1. **`InstallGuid` format** is not specified. The DLL uses the registry string **verbatim** in the object
   names after checking it is a GUID (36 characters, optionally braced). The dev install writes
   `{UPPERCASE-GUID}`; CW4's `vcam-install` and the helper must use the same string verbatim (recommend the
   same braced uppercase form).
2. **`header_bytes`** value is not specified: set to `sizeof(Header)` = 448.
3. **"Logs at most 1 line/s"**: implemented as a token bucket of 3 lines refilled at 1 line/s (long-run <= 1/s,
   with a short burst so Start/Stop lines are not lost), suppressed lines counted on the next line.
4. **`beat_qpc` in the future** (> now + 1 s): the contract says "else stale"; the DLL shows the built-in
   image and also counts it in `rejected` (an old beat is the normal "helper gone" case and is not counted).
5. **Torn seqlock**: "previous good picture" is used only if a live or slot-3 picture is currently on screen;
   otherwise the built-in image (so an hours-old picture never reappears).
6. **Change detection** (implementation, but it binds the helper): the helper must increment `seq` (never reset
   it, also across helper restarts) and set `written_qpc` on every slot write.
7. The **section usually does not exist between app sessions** (Frame Server shuts down all source objects on
   close). §4.6.2 already makes the helper open it on Demand or by 1 s polling; noted because a helper that
   expects it to persist would wait.

## Needs Alex

- **H9**: reboot lebig and sign in, then the "after a reboot" row of the DLL-update table (expected: new DLL,
  since Frame Server starts fresh). Not run (no reboot allowed).
- **H4**: Beamer Camera still listed after a logoff/reboot (System lifetime). Not run.
- (H2, not CW5's: Camera app / Teams / Discord, as C2's Needs Alex 1.)

## Notes for other WPs

**CW4 (helper):**
- Names: `Global\BeamerCamera.<InstallGuid verbatim>.{Shm,Frame,Demand}`; open the section with
  `FILE_MAP_WRITE` (it gives read too), the frame event with `EVENT_MODIFY_STATE`. The section is gone between
  app sessions: open on the Demand event, poll every 1 s otherwise, and drop your mapping when `source_pid`
  changes or the magic is wrong.
- Demand: the DLL opens `.Demand` with `EVENT_MODIFY_STATE` on every publication; expect several
  `streaming:false` publications per close (Frame Server re-creates the source 2-3 times) and treat them as one.
- Slot writes: `seq` odd, pixels, `w/h/stride`, `written_qpc`, `picture`, `seq` even; never reset `seq`; then
  `latest`, `mode`, `beat_qpc`, `SetEvent(.Frame)`. Present at ~30 fps: if no newer picture arrives within 50 ms
  the DLL repeats the last one (counted in `repeats`).
- Header counters are cumulative, not per stream. `dll_version` is the running DLL's `kDllVersion` (=
  `loaded_version`); with identical `kDllVersion` an update is invisible there (see the update section).
- Building against `shm_protocol.h` with `/W4` gives C4324 (alignas padding); `build.ps1` uses `/wd4324`.
- Measured with the feeder: helper write 0.63 ms (720p) / 1.27-1.30 ms (1080p) including pattern generation;
  Demand signal to the helper 0.01-0.02 ms.

**CW9 (packaging):** `build.ps1` output `native\audio\build\win\camera\BeamerCameraSource.dll` (277 KB) and the
three tools beside the helper. `tools/devinstall.ps1` / `devuninstall.ps1` mirror §9.2 (Program Files version
folder, CLSID with `ThreadingModel=Both`, `HKLM\SOFTWARE\Beamer\Camera` values, ProgramData ACL
`SY:F, BA:F, LS:M, Users:RX`, no inheritance). The README's "Dev install and removal" section is yours. See the
update section for when the old DLL is locked.

**CW11 (E2E):** `camprobe read --strip c1 --dump FILE` writes `t_us class frame ts100us` per frame (C1 code
strip), `--events` prints class changes (`builtin`, `slot3`, `helper`, `c1`), so `cam_helper_kill` can read
"built-in image after N ms" directly. DirectShow consumers add ~34 ms (one frame) as in C2.

## PC cleanup

Removed at the end (`accept/t_cleanup.ps1`, over ssh) and verified:

- Camera removed unelevated by its creator (`feeder vcam remove` in Alex's session: `Remove()` ok); then
  `tools/devuninstall.ps1` (elevated).
- **No camera device**: no `SWD\VCAMDEVAPI*` node and no "Beamer Camera" device in any state (`Get-PnpDevice`,
  present or not); `camprobe list` shows MF: none, DirectShow: only "OBS Virtual Camera" (pre-existing).
- **No COM class**: `HKLM` and `HKCU\Software\Classes\CLSID\{19FCB9D1-…}` absent; `HKLM\SOFTWARE\Beamer` absent
  (it existed only for this test).
- **No files**: `%ProgramFiles%\Beamer` (versions 1 and 2) and `%ProgramData%\Beamer` (log) absent (both
  existed only for this test); no `C:\ProgramData\BeamerCam*`.
- **No scheduled task** `CW5-*`; no `feeder`/`camprobe`/`shmfuzz` process.
- FrameServer back to its initial state: Stopped (it exits ~30 s after the last use), start type Manual.
  I stopped/restarted it several times over ssh during the tests (allowed for dev agents by §9.2).
- Beamer untouched: its tasks `Beamer-lebig-Alex` (Running) and `Beamer-GameInputRecovery` (Ready) as before;
  no firewall or system setting changed.

What stays: the work folder `C:\Users\Alex\camera-cw5\` (sources export, binaries, the acceptance scripts,
`run\` outputs incl. `fuzz_frames.txt`, ~10 MB). Remove with `Remove-Item -Recurse C:\Users\Alex\camera-cw5`
when Phase 2 is closed.

Harness note: the first 60 s size run was invalid (stopping a scheduled task does not kill the tool it
started, so the 720p feeder overlapped the 1080p run; the DLL stayed correct: 23 torn reads rejected, 0
"other" frames). The drivers now kill the tools by path; all numbers above are from clean re-runs.
