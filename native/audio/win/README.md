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
- **Virtual mic**: render into "CABLE Input". Shared mode queues 3 periods: 2 periods gave 1 hole per 60 s in
  3 runs out of 4; 3 periods gave 0 in 3x60 s + 300 s. Click latency to an app's capture of CABLE Output: 103-111 ms.
  `--vmic-exclusive` (helper command line) renders exclusively, 16-bit, 2 ms period: 43-45 ms, 0 holes in
  4x60 s but 1 in a 300 s soak. Off by default because it takes CABLE Input away from other apps.
- **Defaults** that are blocked or VB-CABLE are reported as `""`. Opening a blocked endpoint, or using CABLE Output
  or CABLE In 16ch as a source, fails with `blocked_device`.
- Errors in `*err` are `"<§3.3 code>: <message>"`.

Build: `native\audio\build-win.cmd` (add `--check-only` for the harness alone, `--clean` to drop the object cache).
