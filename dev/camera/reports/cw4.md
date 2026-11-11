# CW4: Windows video sink in the helper (report)

Date 2026-11-11. Branch `feat/camera`. Code commit `b8a526c`. PC `lebig` (Windows 11 25H2, RX 7900 XT), builds
in a private export `C:\Users\Alex\camera-cw4\` (removed at the end), helper-side runs in Alex's session (session
1) through a temporary scheduled task (H8 held: Alex was signed in).

## What was built

| File | What |
|---|---|
| `native/audio/win/video_backend_win.h` | internal declarations; `make_win_video_backend`, `run_vcam_verb_if_requested`, `WinVideoOptions` (incl. test seams) |
| `native/audio/win/video_backend_win.cpp` | `VideoBackend` + `VideoSinkHandle`: decoder worker (MFT creation off the presenter), **decoding on a sink-owned thread**, 12-picture NV12 pool, IDR backlog while a decoder is being made, 3-errors reopen, device loss -> software -> hardware after 30 s (seamless swap at the next IDR) |
| `native/audio/win/videodec_win.cpp` | MF H.264 decoder: `MF_LOW_LATENCY` + `CODECAPI_AVLowLatencyMode` (creation fails without it), D3D11 + own staging readback, software path, display vs coded size, `display_size_ok`, `copy_nv12_display`, `au_has_idr` |
| `native/audio/win/vcam_win.cpp` | install state (HKLM), kernel objects by InstallGuid, Demand event (DACL SY/LS/OwnerSid, ownership check), shared-memory client (header check, beat, live/hold/waiting, seqlocked slots), demand (`in_use` on at once / off 300 ms, `want` vs `kOffered`, `loaded_version`), camera create/Start (`MFCreateVirtualCamera` resolved at run time), presence, `vcam` event JSON |
| `native/audio/win/placeholder_win.cpp` | Direct2D/DirectWrite placeholder (tokens dark palette, camera mark, title 24 px / line 16 px at 720p) -> NV12 BT.709 |
| `native/audio/win/app_usage_win.cpp` | consent-store reader (`webcam\<family>`, `NonPackaged\<path>`), name only when exactly one app is in use; packaged display names through `SHLoadIndirectString` |
| `native/audio/win/vcam_install_win.cpp` | verbs `vcam-install`, `vcam-uninstall`, `vcam-remove`, `vcam-status` (exit 0/2/3/4, one JSON line) |
| `native/audio/win/video_check.cpp` | CW4's harness: `unit`, `setup`/`clear` (test install keys), `standin` (section owner like the DLL + control block), `play`, `scenario demand|status|states|reject|devloss|idlecpu`, `installflow`, `present` |
| `native/audio/win/main.cpp` (hunk) | vcam verbs before `run_helper`; video backend unless `--no-video`; `--video-decoder=` kept from the engine; `run_helper(Backend&, VideoBackend*, ...)` when `BEAMER_HELPER_VIDEO` |
| `native/audio/win/build.ps1` (hunk) | video sources + libs (Media Foundation **delay-loaded**), `video_check.exe`, `BEAMER_HELPER_VIDEO` from `engine/helper.cpp` (in the object tag), calls `vcam-win\build.ps1` |

Test method. Only CW5 may register the DLL or create the camera in wave 1, so nothing here touched the real CLSID,
`HKLM\SOFTWARE\Beamer\Camera`, Program Files or the camera. The backend has a registry seam (`reg_base`): test
install keys under `HKLM\SOFTWARE\BeamerTest\CW4*` give a test InstallGuid, so all kernel objects were
`Global\BeamerCamera.{C4C4C4C4-...}.*`. `video_check standin` (session 0, elevated: it can create `Global\`
sections) owns the section and frame event exactly as the DLL does and applies Start/Stop/want commands. In the
helper-side process (session 1, normal token) a reader thread decides at 30 Hz what the DLL would deliver (beat
window, mode, latest, seqlock, size checks of §4.6.3), and an engine stand-in feeds Annex-B AUs at 30 fps, waits for
each `on_decoded` while ticking `hold()` every 33 ms like CW2's presenter, presents clean pictures at once, and
answers `decoder_*` / `size_rejected` with an IDR (restart at the file's first AU). Camera creation was disabled
(`create_camera=false`, attempts counted).

Streams: C1's VideoToolbox recordings `synth_720p30_2500k_30s.h264`, `synth_1080p30_5000k_8s.h264` (coded
1920x1088), and two made with ffmpeg/libx264 on the PC: `bands1080.h264` (1920x1080 = coded 1088 + crop; 27
horizontal bands of 40 rows with distinct Y/Cb/Cr, the last band Y 210, a moving box in the middle) and
`big1440.h264` (2560x1440).

## Acceptance (CW4)

| Criterion | Result | Command / numbers |
|---|---|---|
| Decode p95 <= 4 ms (720p) / <= 5 ms (1080p) hardware, 0 frames of decoder lag | **pass** | `play` in session 1. 720p HW: p50 1.69-1.80, **p95 1.89-2.00 ms** (4 runs, 30 s). 1080p HW (C1, coded 1088): p50 2.89-3.02, **p95 3.18-3.30 ms** (6 runs, 60-90 s). `decoder_lag_frames` 0 and MF errors 0 in every run (every AU's picture comes out of its own ProcessInput). Decoder (D3D11 + MFT) ready 33-100 ms after `open_sink`; the first IDR is kept meanwhile (`backlogged` 1, `discarded` 0). One caveat: intermittent hardware stalls, see "Observed" |
| 1080p (coded 1920x1088): no `size_rejected`, no `size` PLI over 60 s, slot 1920x1080, correct colours and bottom rows, HW and SW | **pass** (vs the stand-in; `camprobe` through the real DLL is CW11's) | Output type `1920x1080 coded h 1088` (HW and SW). 60-90 s runs: `size_rejected_events` 0, every slot 1920x1080 (`slot_wrong_size` 0). Band check of the slot as the DLL copies it, once a second: **bad Y 0, bad UV 0, bottom rows (1072-1079) bad 0** over 59 + 88 checks HW and 59 SW. **Negative control** (`--negative`, UV read at the display height = the forbidden bug): HW 72,900 / SW 32,400 bad UV samples, so the check does catch a wrong offset. Nothing of rows 1080-1087 is copied (`unit`: poisoned rows never reach the output) |
| Forced software works, CPU < 30 % of a core at 1080p | **pass** (close to the limit) | `--sw`: 1080p C1 stream p95 5.40-5.68 ms, **14.7 %, 26.6 %, 27.0 %** of a core (whole harness process, incl. the 30 Hz reader and slot copies); bands 1080p 20.4-27.5 %; 720p 10.4-15.3 %. In the helper, `--dev --video-decoder=sw` |
| SPS with display size 2560x1440 rejected 3 times, no slot written, then the stream fails with the `error` placeholder | **pass** | `scenario reject`: `size_rejected` events 3 (one per IDR at that size, engine stand-in answers each with an IDR), `decoded_ok` 0, **no live slot ever written** (reader never saw a live picture), after the third the engine stand-in calls `show_placeholder("error")`: shown `ph:error` |
| Simulated device removal: decoder re-created on the worker, presenter keeps holding, `camprobe` never sees the built-in image | **pass** (4 of 4 runs on the final code) | `scenario devloss` (hook makes the next HW decode return `DXGI_ERROR_DEVICE_REMOVED`): `device_lost` event, software decoder made on the worker, **first software picture 125-140 ms after the loss**, reader saw **0 built-in deliveries**. An earlier run (decoding still on the presenter thread) saw one 3.2 s HW stall and 18 built-in deliveries, which led to the decoder-thread change (below) |
| Simulated device removal -> software decode within 1 s and back to hardware after 30 s | **pass** | software picture 125-140 ms; `decoder_hw` (standby) at 30.0 s, **switch to hardware at the next IDR 30.11-30.13 s** after the loss, no freeze (software decodes until the hardware decoder has its IDR) |
| `vcam in_use` <= 50 ms after an app opens the camera, off 300 ms after it closes | **pass** vs the stand-in (Frame Server -> CW11) | `scenario demand` (stand-in Start -> listener `vcam_changed`, cross-process QPC, 10 cycles): **on p50 2.2-2.6 ms, max 12-13 ms** (the max includes the harness's 2 ms polling); **off p50 328 ms (322-344)**; a 100 ms stop/start blip produces no `in_use:false`. `want` 1920x1080 kept, 800x600 -> 1280x720, 640x360 kept |
| `app` reported for Chrome and `camprobe` | **partly**: logic verified on a fake consent store; live needs the registered camera (CW11) | `unit` on `HKCU\Software\BeamerTest\CW4\webcam`: one desktop app -> "Bloc-notes" (FileDescription), a stopped entry ignored, two in use -> no name, a packaged app -> **"Calculatrice Windows"** (resolved `ms-resource:AppStoreName`). Chrome would come out as its FileDescription "Google Chrome"; camprobe (no version info) as `camprobe.exe` |
| Placeholder on screen <= 100 ms after a `vcam` idle change | **pass** | `scenario states`: 6 changes (`waiting` <-> `sharing_off`), new slot-3 picture delivered **4.7-6.0 ms** after `vcam_placeholders`. Render: 1080p 10-11 ms, 720p ~9 ms, first render of a process 110-130 ms (D2D/DirectWrite/WIC start-up, done at `ready` with the idle placeholder) |
| `starting` at `open_sink`, hold then `reconnecting` at 2 s | **pass** | shown at open `ph:starting`; first live picture 59-223 ms after open; stall with `hold()` ticks: shown `hold`, **no placeholder and no built-in image before 2 s**; the engine's `show_placeholder("reconnecting")` on screen 8-26 ms later |
| Sink closed -> last picture held 500 ms then the idle placeholder; a new sink presenting within 500 ms shows no placeholder | **pass** | idle placeholder delivered **526-542 ms** after the sink closed (500 ms + one 33 ms delivery), no built-in image; close -> new sink 250 ms later -> live: **no placeholder and no built-in image** in between |
| Wedged presenter (test hook) -> DLL built-in image <= 600 ms (500 ms threshold) | **pass** | the engine stand-in stops calling the sink: built-in decided **518-529 ms** after its last call |
| Network events reset after each wait (C4 surprise 6) | **pass** (n/a to sockets: the sink has none) | the vcam loop waits only on auto-reset events (Demand, wake); `scenario idlecpu`: **0.0-0.3 % of a core** with the section open (in use and not), no spinning |
| A pre-created foreign `.Demand` event is refused | **pass** | stand-in `--squat` creates `.Demand` first (owner Administrators, open DACL): `vcam` `error: "squatted"`, and `in_use` still works by polling (on in ~100 ms) |
| `vcam-install` / `vcam-uninstall` work elevated (ssh) and exit 3 unelevated, `vcam-remove` works unelevated | **pass** with test seams (real registration is CW5's in this wave) | elevated with `--test-reg/--test-dir`: install rc 0 (DLL found by the lookup rule `<exe>\camera\`; copied to `...\Beamer\Camera\1\`; COM class `InprocServer32` + `ThreadingModel=Both`; camera key InstallGuid/OwnerSid/DllVersion/DllPath; **InstallGuid kept** on reinstall; older version folder `0\` deleted; ProgramData folder ACL **SYSTEM, LocalService, Administrators full, Users RX, not inherited**); uninstall rc 0 (keys and folders gone, FrameServer not touched). Missing `--owner-sid` / bad SID / unknown option rc 2. **Unelevated (session 1): install rc 3, uninstall rc 3, remove rc 0, status rc 0.** The real `Remove()` was not called (it would remove CW5's camera) |
| Install states `missing/ok/outdated/broken` and `loaded_version` detected | **pass** | `unit` on test keys: missing; COM class only -> broken; full -> ok; DllVersion 0 -> broken; COM path != DllPath -> broken; DLL file missing -> broken; bad OwnerSid -> broken. Header layout version 99 -> **outdated**. `loaded_version` = header `dll_version` (1, and 2 with `--dll-version 2`). `outdated` from `kDllVersion > DllVersion` cannot be exercised while `kDllVersion` is 1 |
| `vcam {}` after a `vcam-install` creates the camera without a helper restart | **pass** for the trigger (creation itself is CW5/CW11's) | `installflow`: a running backend reads `missing`; the elevated install runs; the next `vcam()` reads `ok` and **starts a creation attempt** (counted; `MFCreateVirtualCamera` disabled in tests) |
| Audio does not regress | **pass** | `backend_check list` / `periods` unchanged; `beamer-audio.exe version` ok; the helper links with CW2's engine (`BEAMER_HELPER_VIDEO=1`); Media Foundation is a delay-load import only (`dumpbin`: static imports gain d3d11, d2d1, DWrite, shlwapi, version, all present on every Windows), so the helper still starts where mfplat.dll is absent (Windows N) |

Live presence check: with CW5's install on lebig, `video_check present` (read-only) reports `install: ok` and
`present: true` in both sessions.

## Observed: intermittent hardware decoder stalls

Three times in ~25 min of hardware decoding, single MFT calls blocked **0.4-3.3 s** (e.g. one 1080p run:
555, 3275, 2886, 262, 754 ms within ~8 s; other runs clean). Software decoding never stalled. While `decode()` ran
on the presenter thread, such a stall stopped the heartbeat and the DLL would show its built-in image for the
rest of the stall. **Decoding now runs on a sink-owned thread**: `decode()` only queues the AU, so
`present`/`hold`/`show_placeholder` (the heartbeat) never wait on the GPU, and a stall becomes a held picture. CW2's
`video_recv.h` already expects `on_decoded` / `on_sink_event` from "the backend's decoder thread".

One run on the final code still logged a 3.3 s + 2.9 s stall **with** 15 built-in deliveries (0.5 s), so the
harness's presenter thread was also held up then. I could not explain or reproduce it: the next 5 min of 1080p
hardware decoding, now with presenter-gap and reader-gap instrumentation and a separate gap monitor process, were
clean (presenter gaps <= 54 ms, machine gaps <= 33 ms). Other load on the PC at the time: Epic Games Launcher and
CW2's null helpers. CW11's soak should watch for built-in deliveries and for `slow_calls` / gaps in the helper's
behaviour.

## Contract issues

1. **`decoder_hw` / `decoder_sw` and PLI start (§4.6.1, §4.5.6).** The design says a new decoder makes the stream
   send PLI start. CW2's `VideoRecvCore::on_sink_event` only records the kind; it asks for an IDR on
   `device_lost` and after 3 `on_decoded(false)` within 10 s. That works here: (a) at stream start the sink keeps the
   IDR (and up to 1 s behind it) while its decoder is being made and decodes it when ready, so no IDR is lost to
   creation time; (b) after a device loss the engine asks at `device_lost`; (c) after the sink's own 3-error reopen
   the engine's own 3-error rule asks too. Suggest CW2 also sends PLI start on `decoder_hw/sw` (cheap: start IDRs
   are coalesced), so a reopen never depends on the two counters agreeing.
2. **Threads of the sink callbacks.** §3.1 lists "decode calls" on the video thread. Decoding now runs on the sink's
   own thread, so `on_decoded` / `on_sink_event` arrive from there (CW2 already posts them to a mailbox). The
   heartbeat contract is unchanged: one of `present` / `hold` / `show_placeholder` on every display tick
   (`video_recv.h`), and each of them is a beat.
3. **`vcam_kill_beat` (§3.2 dev).** `VideoBackend` has no hook for it. A wedged presenter is just an engine that
   stops calling the sink, which is how the harness tested it; the engine can implement the dev key that way.
4. **`--dev decoder=sw` (§4.6.1)** is not defined on the command line. Implemented as the backend-only
   `--video-decoder=sw|hw|auto`, honoured with `--dev` and kept away from the engine by `main.cpp`.
5. **Presence.** The camera's devnode (`SWD\VCAMDEVAPI\<hash>`) is named "Windows Virtual Camera Device"; "Beamer
   Camera" is the friendly name of its `KSCATEGORY_VIDEO_CAMERA` interface, which is what `present` checks.
6. **Demand ownership (§4.6.3).** "Owner must be its own user SID" refuses the helper's own event after a restart
   when the helper is elevated (an elevated token's objects are owned by Administrators). Accepted owners: the
   token's user or its default owner.
7. **`vcam()` and creation.** `vcam()` re-reads the install state and may start a creation; automatic attempts are
   rate-limited to one per 2 s per install so a failing creation and its `vcam` event cannot loop. CW2's
   `vcam_refresh` also calls `vcam_enable(true)` when ok, not present and not creating, which forces an attempt.
   `want` is `{0,0,0}` while not in use (CW2 emits null).
8. **Coded offset on the GPU path.** The staging texture's UV plane follows the texture's own height (`>=` the
   coded height); that is the coded offset of that buffer. System-memory buffers use `pitch * coded_h`.
9. Not required by the contract: the helper does not check the owner of the section or frame event (only
   creators do, §4.6.3). `vcam-status` opens the objects (it creates `.Demand` if no helper holds it).

## Needs Alex

Nothing (H8 held during the run).

## Left for CW11 (needs the registered DLL/camera, CW5-only in this wave)

C1's recordings through `recv` -> real DLL -> `camprobe` (decode, 1080p colours and bottom rows in `camprobe`),
`app` for Chrome and `camprobe` live, `in_use` latency through Frame Server, `vcam {}` creating the real camera,
real `vcam-remove` (`Remove()`), the helper end to end with CW2's engine (`beamer-audio.exe` with the video backend
was built and linked, not run against CW5's live objects).

## Notes for CW9 / CW11

- `native\audio\build-win.cmd` now also builds `video_check.exe` and, through `vcam-win\build.ps1` (CW5),
  `build\win\camera\BeamerCameraSource.dll` and the tools; a failing DLL build fails the helper build.
- Lookup rule for `vcam-install` without `--dll`: `<helper dir>\camera\BeamerCameraSource.dll`, then
  `<helper dir>\BeamerCameraSource.dll`. `--test-reg` / `--test-dir` are hidden test seams (never pass them in the
  product). On a loaded DLL with the same version number, install renames it aside and deletes it at reboot.
- `vcam-uninstall` stops `FrameServer` (and `FrameServerMonitor`) only when a file is locked, then deletes what is
  left at reboot.
- Running the harness: build, `video_check setup --reg SOFTWARE\BeamerTest\CW4 --owner-sid <SID>` (elevated),
  `video_check standin --reg ...` (elevated, session 0), then in the user's session `video_check play FILE --reg ...
  [--sw] [--bands --expect 1920x1080]` or `scenario demand|states|reject|devloss|idlecpu|status --reg ... [--file F]`;
  `video_check clear --reg ...` at the end. `video_check unit` (elevated for the install-state part).
- Helper options: `--dev --video-decoder=sw` forces software; `--no-video` builds no video backend.

## PC cleanup

`C:\Users\Alex\camera-cw4\` deleted (sources, build, test media, test install folders); test keys
`HKLM\SOFTWARE\BeamerTest\*` and `HKCU\Software\BeamerTest` deleted; no `CW4*` scheduled task left; no firewall
rule for any CW4 path (nothing opened a socket); no process left. The real install, the Program Files / ProgramData
Beamer folders, the installed Beamer and the PC clone were not touched.
