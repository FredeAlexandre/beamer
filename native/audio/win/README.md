# Windows backend (WASAPI) for `beamer-audio.exe`

PHASE2 §2.4 `Backend` on Windows. `main.cpp` builds it and hands it to the engine's `run_helper`.

| File | What |
|---|---|
| `backend_win.h` | internal declarations; includes `../engine/backend.h` once |
| `backend_win.cpp` | `WinBackend`: worker thread (notifications debounced 300 ms and de-duplicated against the last snapshot, default tracking, demand, `stream_lost`), one IO thread per stream (MMCSS "Pro Audio"), default following, recovery from `AUDCLNT_E_DEVICE_INVALIDATED` (retried for 3 s, then `stream_lost`), features, mic permission |
| `devices_win.cpp` | inventory and classification: transport from the devnode chain (first bus node going up: `PCI\`/`ACPI\` = physical, `ROOT\`/`SWD\` = virtual), form (`PKEY_AudioEndpoint_FormFactor` refined by names), VB-CABLE roles, blocklist |
| `blocklist.inc` | Elgato Wave Link virtual endpoints (names, adapter, ancestor); the physical Wave:3 stays allowed |
| `loopback_win.cpp` | WASAPI clients: shared capture/render/endpoint loopback, process loopback (exclude tree), exclusive render, keep-alive, channel fold-down |
| `mute_journal_win.cpp` | endpoint mute with journal `<state-dir>/audio-mute-journal.json` + guard process (`beamer-audio _guard --pid P`) |
| `sessions_win.cpp` | demand: Active capture sessions of other processes on "CABLE Output" |
| `backend_check.cpp` | test harness that drives the interface directly (see its header for the commands) |
| `build.ps1` | called by `../build-win.cmd` |

Behaviour worth knowing:
- **System sound** = process loopback excluding the tree of `exclude_pids[0]` (one tree only on Windows), at the
  default output's mix channel count, folded down here. It is not tied to an endpoint. `CaptureSpec.device` names
  the endpoint to **mute** (`""` = default output, followed). Process loopback keeps a steady 48 kHz clock when
  idle, so it needs no keep-alive. **Reference** (AEC) = endpoint loopback + keep-alive.
- **Mute**: journal on disk before `SetMute`; mute, master and per-channel volumes are put back exactly at stop,
  by the guard within ~30 ms of a kill, by `restore_journal()` at the next start (entries of dead owners only),
  and after a device change when an endpoint unplugged while muted comes back.
- **Virtual mic**: render into "CABLE Input", shared, at a 128-frame engine period (IAudioClient3; VB-CABLE allows
  128..480) with 3 periods (8 ms) queued: 59-72 ms click latency to an app's capture of CABLE Output, 0 holes in
  4x60 s + 300 s (WP12; the old 480 x 3 measured 102-117 ms, 128 x 2 gave holes). When another stream already runs
  CABLE Input's engine at another period, the open falls back to the default period (logged). After a preemption
  (another app's exclusive open) the stream waits 250 ms before reopening, so that app's format retries succeed.
  `--vmic-exclusive` (helper command line; reported as feature `vmic_exclusive`) renders exclusively, 16-bit, 2 ms
  period: 36-45 ms, but takes CABLE Input away from other apps, so it is opt-in per machine.
- **Endpoint render** keeps 1.5 periods (15 ms) queued (WP12: 0 holes on the G733 in 4x60 s + 300 s). USB
  endpoints here (G733, DualSense) and capture (mics, process loopback) have a fixed 480-frame period: 10 ms packets.
  Mic and endpoint render still ask for 128 frames, which only matters on a driver that reports a period range.
- **Defaults** that are blocked or VB-CABLE are reported as `""`. Opening a blocked endpoint, or using CABLE Output
  or CABLE In 16ch as a source, fails with `blocked_device`.
- Errors in `*err` are `"<§3.3 code>: <message>"`.

Build: `native\audio\build-win.cmd` (add `--check-only` for the harness alone, `--clean` to drop the object cache).
