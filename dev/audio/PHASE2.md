# Audio across Beamer: Phase 2 design (integration into the app)

Date 2026-11-08. Branch `feat/audio`. Inputs: `PHASE1.md` (verdicts, recommended architecture),
`STATUS.md` (Alex's decisions: AEC in Beamer plus a headset warning; on the PC, apps pick
"CABLE Output" once and Beamer never switches Windows default devices), `notes/beamer-link.md`
(link, caps, threads) and the prototypes in `proto/`. This document is the contract for the
implementation agents. Section numbers (§) are referenced by the work packages in §13.

## 0. Decisions at a glance

| # | Question | Decision |
|---|---|---|
| 1 | Layout | Python control plane in a new package `core/audio/`; native code in a new top-level `native/audio/` (portable `engine/`, `mac/`, `win/`, `mac-driver/`, `third_party/`); thin per-app adapters `mac_app/audio_mac.py`, `win_app/audio_win.py`; pages in new files. Existing big files get small, named hunks only (§2.2). |
| 2 | Native sharing | **One portable C++17 engine** (packets, RFC 8439 AEAD on vendored Monocypher primitives, vendored static libopus 1.6.1, jitter buffer + drift resampler ported from `proto/transport/media.py`, levels, UDP flows with receiver reports, the stdio control loop) shared by **one helper executable per OS**. Mac backend in **Objective-C++** (deviation from PHASE1's "Swift", §2.3); Windows backend in C++/WASAPI. No CMake: plain `clang++`/`cl` scripts over one shared source list. |
| 3 | Helper control | JSON lines: commands on stdin, events on stdout, logs on stderr. Helper exits on stdin EOF. Spec §3. |
| 4 | Wire | One new cap `audio` (14th of 16). Messages `audio_hello`, `audio_devices`, `audio_route`, `audio_flow`, `audio_open`, `audio_opened`, `audio_close`, `audio_frame`, `audio_watch`, `audio_stats`, `audio_test`. Device lists and the route are flooded newer-wins like the shared layout; everything about streams is direct-link only. Spec §5. |
| 5 | Routing | Cluster-wide route document: one **active mic** (machine + device, or off) and per machine an **output** (play here, or send to machine T's device, muted here). The active mic feeds the Beamer virtual mic of **every machine where an app is recording it, and only while it is** (on demand). No pointer-focus rule. Per-entry last-writer-wins stamps. Persisted in `audio.json`. Spec §6. |
| 6 | UDP | One UDP flow per directly linked pair, carrying all streams both ways. The side that must not listen (Mac) offers; the other answers on 24823 (24824-24830, then any). Both send first (hole punch), so no firewall rule anywhere. TCP fallback over the link after 2 s of UDP silence. Spec §4. |
| 7 | AEC | Vendored speexdsp echo canceller (MDF + residual echo suppression) in the engine, on the machine that captures the active mic, reference = loopback/tap of that machine's output device; on when that mic is forwarded and the output is not a headset. Headset warning computed in the UI model. Spec §7. |
| 8 | UI | "Audio" page after Crossing on both apps: active-mic selector, notices, a read-only machine map drawn like the Crossing map (tiles, route arrows, meters), a detail panel for the selected machine. Pure page model `core/audio/view.py` shared by both UIs. Spec §9. |
| 9 | Packaging | Mac: helper at `Beamer.app/Contents/MacOS/beamer-audio` (universal, hardened runtime, own entitlements), driver bundle at `Contents/Resources/audio/BeamerAudio.driver`; Info.plist usage strings; entitlement `com.apple.security.device.audio-input`. Windows: `beamer-audio.exe` next to `Beamer.exe`. VB-CABLE never bundled. Spec §10. |
| 10 | Tests | `unittest` for `core/audio` with an in-process fake helper and an N-machine cluster harness; a C++ test binary for the engine on both OSes; a null device backend so the real helper runs in CI-like tests; a two-machine E2E harness driven through dev-only command files. Spec §11. |

Deviations from PHASE1, each with its reason: Mac helper in ObjC++ not Swift (§2.3); route stamps
per entry instead of whole-document (§6.2); offline machines derived from the `links` graph instead
of expiry (§5.3); the listening port is a default carried in messages, not a contract (§4.2); AEC is
a vendored canceller instead of OS voice processing (§7); PHASE1/beamer-link's optional
`Beamer Media (UDP-In)` rule is **not** added (binding constraint: no new firewall rules).

## 1. Terms

- **Machine id**: `protocol.id_text(...)`, the base64 text of the 16-byte machine id, as `by` in `settings_sync`.
- **Direct peer**: a machine with a live link (either direction) whose caps and ours both contain `audio`.
- **Helper**: the native child process (`beamer-audio`). **Engine**: the portable C++ code in it.
- **Flow**: one UDP 5-tuple between two direct peers. **Stream**: one encoded direction on a flow
  (`mic` or `system`), identified by a random u32 chosen by its sender.
- **Virtual mic**: Mac "Beamer Microphone" (`com.beamer.audio.mic`, fed through the hidden
  `com.beamer.audio.micfeed`); PC VB-CABLE (Beamer renders into "CABLE Input", apps record "CABLE Output").
- **Device key**: Mac Core Audio UID; Windows MMDevice endpoint id. **DeviceRef** = `{key, name}`.

## 2. Code layout, ownership, native build

### 2.1 Tree

```
core/audio/__init__.py      exports AudioService, AudioHost, model types
core/audio/wire.py          audio_* message types, builders, validators, size limits (pure)
core/audio/model.py         dataclasses (§8.3), stamps, merges, reachability (pure)
core/audio/planner.py       plan(...) -> Plan (pure, §6.4)
core/audio/store.py         audio.json load/save, atomic (tmp + os.replace)
core/audio/helper.py        HelperProcess: locate, spawn, JSON-lines IO, watchdog, restart
core/audio/service.py       AudioService: worker thread, links, flows, streams, dev command files
core/audio/fake_helper.py   FakeHelper (in-process) and `python -m core.audio.fake_helper` (stdio)
core/audio/view.py          pure page model for both UIs (§9.1)
core/tests/test_audio_*.py, core/tests/audio_cluster_harness.py, core/tests/audio_fixtures.py

native/audio/engine/        portable C++17 (§2.4), sources.txt (its .cpp list), build-tests.sh/.cmd
native/audio/tests/         engine_tests.cpp, vectors/ (generated by gen_vectors.py with PyNaCl)
native/audio/third_party/   opus-1.6.1/ monocypher-4.0.2/ speexdsp-1.2.1/ (AEC subset) nlohmann-json-3.11.3/
                            README.md (version, upstream URL, sha256, licence per library)
native/audio/mac/           main.mm, backend_mac.{h,mm}, devices_mac.mm, tap_mac.mm, vmic_mac.mm
native/audio/win/           main.cpp, backend_win.{h,cpp}, devices_win.cpp, loopback_win.cpp, mute_journal_win.cpp, sessions_win.cpp
native/audio/mac-driver/    git mv of dev/audio/proto/mac-driver (src, Info.plist, build.sh, test/, helper/) + install-driver.sh
native/audio/build-mac.sh   -> native/audio/build/mac/beamer-audio (universal) + BeamerAudio.driver
native/audio/build-win.cmd  -> native/audio/build/win/beamer-audio.exe              (+ native/audio/README.md)

mac_app/audio_mac.py        AudioHost for the Mac, service construction, main-thread hops
mac_app/audio_page.py       AppKit Audio page, AudioMap, LevelMeter
mac_app/audio_driver.py     driver presence/version, install via admin prompt (§10.1)
win_app/audio_win.py        AudioHost for Windows/Linux, service construction, Qt signal hops
win_app/audio_page_win.py   Qt Audio page, AudioMap, LevelMeter
dev/audio/e2e/              two-machine harness (§11.4)
```

`native/audio/build/` is git-ignored. Every new source file carries `SPDX-License-Identifier: GPL-3.0-only`.

### 2.2 Hunks in existing files (the only shared surfaces)

| File | Hunk | Owner (§13) |
|---|---|---|
| `core/link.py` | `_CAP_FOR` += every `audio_*` type -> `"audio"` | WP5 |
| `core/receiver.py` | `_Link.post`: drop `audio_*` when `"audio"` not in caps; `_dispatch`: `elif isinstance(kind, str) and kind.startswith("audio_")` -> `self._callback("audio", link.peer_id, message)`; `"audio"` in the callbacks dict | WP5 |
| `mac_app/bridge.py` | `CAPABILITIES` += `"audio"` when `audio_mac.available()`; `_link_message` elif for `audio_*`; link up/down -> service | WP5 |
| `win_app/sender.py`, `win_app/kvm_bridge_win.py` (caps list ~l.2026, responder wiring) | same as Mac; Linux (`OWN_PLATFORM == "linux"`) never advertises `audio` | WP5 |
| `core/feature_flags.py` | `AUDIO = True` (fork; `False` when offered upstream) | WP5 |
| `WIRE.md` | new section "Audio (capability `audio`, fork)" = §4-§5 of this doc | WP5 |
| `mac_app/setup.py` | `py_modules`: WP5 inserts `"audio_mac",` as the **first** entry; WP6 inserts `"audio_page",` immediately **before** `"pages",`; WP8 appends `"audio_driver",` **after** `"windows_input",` (non-adjacent hunks); `includes` += the `core.audio` modules (WP5); plist keys (WP8) | WP5/WP6/WP8 |
| `mac_app/pages.py`, `mac_app/kvm_bridge_app.py` (builders map + `_audio_page`) | page registration | WP6 |
| `win_app/pages_win.py`, `win_app/kvm_bridge_win.py` (builders map ~l.880 + `_audio_page`) | page registration | WP7 |
| `mac_app/build_dmg.sh`, `entitlements.plist`, `Install-Beamer-Mac.command`, `win_app/build_win_app.ps1`, `Install-Beamer.ps1`, `Beamer-Setup.iss`, `build_portable.spec` | packaging | WP8 |

### 2.3 Why one C++ engine, and ObjC++ on the Mac

- The tuned logic (JB, drift, catch-up, RR, replay window) must exist once; two ports would drift
  apart. The engine runs on the playback IO thread (PHASE1: "driven from the playback device's
  callback"), so it must be native, lock-free and allocation-free on that path.
- **ObjC++ instead of Swift** for the Mac backend: everything it calls is C (Core Audio HAL, AudioToolbox)
  except `CATapDescription` (an ObjC class, fine in ObjC++). One compiler (`clang++`) builds engine and
  backend into one universal binary; no Swift/C++ bridging and no Swift on the IO thread (the Swift
  prototype already had to move its real-time code into `rt.c` for that reason). The Swift prototype is the
  reference for behaviour, not code to keep.
- **Build tooling**: the Mac has Command Line Tools only (clang 21, no CMake) and the PC has MSVC
  (+ bundled CMake). Both build from the same `engine/sources.txt` with a shell script / `.cmd`; libopus is
  compiled from its own `celt_sources.mk`, `silk_sources.mk`, `silk_sources_float.mk`,
  `opus_sources.mk`, `opus_sources_float.mk` lists (parsed by the scripts), float build,
  `-DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF`, no intrinsics/RTCD at first (measure CPU; enable later).
  Mac: compile per arch (`-arch arm64`, `-arch x86_64`, `-mmacosx-version-min=13.0`), `lipo`; objects
  cached under `native/audio/build/obj/<arch>/` and rebuilt only when the source is newer. Windows:
  `cl /std:c++20 /O2 /MT /W4 /utf-8`, x64, static CRT, link `ws2_32 ole32 avrt propsys`.
- **Licences** (repo is GPL-3.0, `LICENSE`): Opus BSD-3-Clause, Monocypher BSD-2-Clause/CC0,
  speexdsp BSD-3-Clause, nlohmann/json MIT: all GPL-3.0-compatible; keep each licence file in
  `third_party/<lib>/` and list them in `third_party/README.md`. Opus 1.6.1 tarball sha256
  `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1` (as in `proto/transport`).
  Resampler: port of the prototype's own windowed sinc (no third-party code).

### 2.4 Engine interface (`native/audio/engine/backend.h`, verbatim contract for WP2/WP3/WP4)

```cpp
namespace beamer::audio {
constexpr int kRate = 48000;                      // the engine works in 48 kHz float32 only
enum class Form { Unknown, Microphone, Speakers, Headphones, Headset };
enum class Role { Normal, VirtualMic, VirtualMicFeed };  // blocked devices are never listed
struct DeviceInfo {
  std::string key, name, manufacturer, transport;  // transport: "builtin","usb","bluetooth","virtual","hdmi","other"
  bool input = false, output = false;
  int channels = 0; double nominal_rate = 0; Form form = Form::Unknown; Role role = Role::Normal;
};
struct DeviceSnapshot {
  std::vector<DeviceInfo> devices;
  std::string default_input, default_output;      // keys, "" = none
  std::string vmic_kind;                          // "beamer" | "vbcable" | ""
  bool vmic_present = false, vmic_in_use = false; // in_use = some other process records the virtual mic
};
struct CaptureSink {   // called on the backend's IO thread: no locks, no allocation, no syscalls
  virtual void on_capture(const float* interleaved, int frames, int channels, uint64_t host_ns) = 0;
  virtual ~CaptureSink() = default;
};
struct RenderSource {  // called on the backend's IO thread; must fill exactly `frames`
  virtual void on_render(float* interleaved, int frames, int channels, uint64_t host_ns) = 0;
  virtual ~RenderSource() = default;
};
enum class CaptureKind { Mic, System, Reference };  // Reference = AEC reference of an output device
struct CaptureSpec { CaptureKind kind; std::string device;  // "" = default (followed on change)
                     int channels; bool mute_local = false; std::vector<int64_t> exclude_pids; };
struct RenderSpec  { std::string device;  // "" = default output (followed), or the key
                     bool virtual_mic = false; int channels; };
struct StreamHandle { virtual ~StreamHandle() = default; virtual std::string device_in_use() const = 0; };
struct BackendListener {
  virtual void devices_changed() = 0;                      // debounced 300 ms by the backend
  virtual void stream_lost(StreamHandle*, const std::string& why) = 0;  // device gone/invalidated
  virtual ~BackendListener() = default;
};
class Backend {
 public:
  virtual ~Backend() = default;
  virtual void set_listener(BackendListener*) = 0;
  virtual DeviceSnapshot snapshot() = 0;
  virtual std::unique_ptr<StreamHandle> open_capture(const CaptureSpec&, CaptureSink*, std::string* err) = 0;
  virtual std::unique_ptr<StreamHandle> open_render(const RenderSpec&, RenderSource*, std::string* err) = 0;
  virtual int restore_journal() = 0;                       // Windows endpoint mutes; Mac returns 0
  virtual std::vector<std::string> features() = 0;         // §3.3 feature words
  virtual std::string permission(const std::string& what) = 0;  // "mic" -> granted|denied|undetermined|unknown
};
std::unique_ptr<Backend> make_null_backend(const std::string& options);  // engine-provided, for tests
int run_helper(Backend& backend, int argc, char** argv);                 // engine-provided main loop
}
```

Backends convert to/from 48 kHz float32 interleaved (Mac: AUHAL/AudioConverter; Windows:
`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`), map channels (mono = average, own 7.1 fold-down on Windows as in
`proto/win`), and own all OS quirks: Mac tap-only aggregate without auto-start, feed UID re-translation
after a coreaudiod restart (`kAudioHardwarePropertyServiceRestarted`), Windows keep-alive render for
loopback, `AUDCLNT_E_DEVICE_INVALIDATED` -> `stream_lost`. `mac/main.mm` and `win/main.cpp` are three
lines: build the backend, `return run_helper(*backend, argc, argv);`.

**Blocked devices** (never listed, opened, muted or changed): any device whose name or manufacturer
contains `Elgato` **and** whose transport is virtual/software, plus exact names collected by WP4's first
task on the PC (Wave Link virtual endpoints) into `win/blocklist.inc` (Mac: `mac/blocklist.inc`); the
physical "Wave:3 Mic In" stays allowed. Also never listed as sources/outputs: the virtual mic itself and its
feed, "CABLE Output", "CABLE In 16 Ch"; "CABLE Input" is used only as the virtual mic sink.

## 3. Helper control protocol (stdio)

### 3.1 Process

- Command line: `beamer-audio --control --parent-pid <pid> --state-dir <dir> --exclude-pid <pid>
  [--dev] [--log-level debug|info|warn]`. Without `--control` it offers diagnostics only:
  `beamer-audio list`, `beamer-audio restore`, `beamer-audio version`. `--backend null:<options>`
  replaces the OS backend by the null backend (§11.3) in any helper (`beamer-audio-null` defaults to it).
- `<dir>` = the folder of `settings.json` (Mac `~/Library/Application Support/Beamer`, Windows
  `%LOCALAPPDATA%\Beamer`). Windows journal: `<dir>/audio-mute-journal.json`.
- `--exclude-pid` = Beamer's own Python process (`os.getpid()`): on Windows the process-loopback
  exclusion takes that tree (it contains the helper; with the PyInstaller one-file build the Python
  process is the bootloader's child, so pass `os.getpid()`, never the parent). On the Mac the tap
  excludes the helper itself and that pid (Core Audio has no trees).
- Startup order: replay the journal (`restore_journal`), then emit `ready`. The helper exits 0 on stdin
  EOF or `quit`, after stopping streams and restoring mutes; it exits within 1 s if `--parent-pid` dies
  (Windows: wait on the process handle; Mac: `kqueue` `EVFILT_PROC NOTE_EXIT`).
- stdin/stdout are UTF-8 JSON, one object per line (`\n`), each line < 64 KiB. Commands have `"cmd"`,
  events have `"ev"`. A command may carry `"req": <int>`; the helper answers it with exactly one
  `{"ev":"ok","req":N}` or `{"ev":"error","req":N,"code":"...","msg":"..."}`. Unknown commands -> error
  `unknown_cmd`; unknown fields are ignored. stdout is written by one writer under a mutex, flushed per line.
- stderr: free text log lines, `level: message`. Beamer logs them under `beamer.audio.helper`, at most
  50 lines/s.
- Threads: main (dispatcher) + a stdin reader thread, one network thread (`poll`/`WSAPoll` on all flows,
  sleeping until the earliest flow timer: <= 10 ms, 2 ms while a flow is in `link` state; it also sends
  hello/ping/rr and batches `link_out`), one encoder thread per send stream, the backend's IO threads, one
  50 ms timer thread (levels, stats, `in_use` off-delay). IO threads touch only SPSC rings and atomics.

### 3.2 Commands (Beamer -> helper)

| cmd | fields | effect |
|---|---|---|
| `devices` | - | emit `devices` now |
| `flow_open` | `flow` (str, 8 hex), `peer` (machine id), `role` `"offer"`/`"answer"`, `port` (int, 0 = any; answer side passes 24823 and the helper tries 24823..24830 then 0), `host` (the peer's IPv4 from the link, optional), `remote` `{"host","port"}` or null, `key` (b64, 32 B), `rr_ms` (10), `udp` (bool, default true) | open the UDP socket and emit `flow_bound` (its local port) and `flow_state probing`. With `port` 0 and `host` set (the side that must not listen: the Mac) the socket is **not bound**: it is `connect()`ed towards `host:24823` and gets an ephemeral port on the routing interface, as `proto/transport/peer.py` did (Phase 1 V3: no macOS firewall dialog); `flow_remote` re-connects it. With `port` > 0 it binds that port on the interface that routes to `host`/`remote.host`. With neither (`remote` null, no `host`) it binds `0.0.0.0:0`: tests on 127.0.0.1 only. When `remote` has a port, `connect()` and start probing (hello every 200 ms until a valid packet arrives, then every 1 s). `udp: false`: no socket, the flow is in `link` state at once (loopback / no host, §4.2) |
| `flow_remote` | `flow`, `host`, `port` | set the remote (offer side, after the answer), `connect()`, start probing |
| `flow_close` | `flow` | send `bye`, close; streams on it stop |
| `link_in` | `flow`, `p` (list of b64 sealed packets) | packets that came over the TCP link (§4.5) |
| `send` | `stream` (u32), `flow`, `kind` `"mic"`/`"system"`, `source` (`{"device": key|null}` for mic, `{"system": true}`; with `--dev` also `{"test": spec}`, spec as `TestSignal`: `"A"`/`"B"[:<pilot amp>]`, `"sine:<hz>[:<amp>]"`, `"noise[:<amp>]"`, `"silence"`, `"rise:<hz>[:<dB/s>]"`), `codec` (§4.4), `key` (b64), `mute_local` (bool, system only), `aec` (`{"reference": key|null}` or null; mic only, a system stream ignores it; with `--dev` the object may also carry `lead_ms` (-40..80: the reference sample fed with mic sample t is the one captured at t + lead_ms, default -1), `max_wait_ms` (0..200, default 30: how long a mic frame waits for its reference) and `exclude_pids` (list of ints: processes left out of the reference capture, for live tests)) | capture -> [echo canceller] -> [howl guard, system only] -> encode -> seal -> flow |
| `recv` | `stream`, `flow`, `kind`, `codec`, `key`, `sink` (`{"device": key|null}` or `{"virtual_mic": true}`) | flow -> open -> JB -> decode -> resample -> render |
| `loop` | `stream`, `source` `{"device": key|null}`, `sink` `{"virtual_mic": true}` | local mic -> virtual mic, through the drift controller, no codec, no AEC |
| `stop` | `stream` | stop that stream; restore any local mute it holds |
| `meter` | `on` (bool), `period_ms` (50..500) | `levels` events on/off |
| `test_input` | `device` (key), `seconds` (1..15) | open that input for metering only |
| `ping` | `req` | `pong` with the same `req` (no `ok`) |
| `quit` | - | orderly exit |
| `dev` | only with `--dev` (else `unknown_cmd`): `flow` (optional, default all flows), `drop_udp` (bool: drop all UDP in and out), `loss_pct` (0..100: drop media at send), `reset_stats` (restart the `run_*` statistics), `null` `{"vmic_in_use": bool, "remove": key, "add": key}` (null backend only) | test switches for E2E and acceptance runs |

### 3.3 Events (helper -> Beamer)

| ev | fields |
|---|---|
| `ready` | `proto` (1), `version`, `platform` (`"macos"`/`"windows"`/`"linux"`), `features` (list of: `system_capture`, `local_mute_tap`, `local_mute_endpoint`, `mic_demand`, `aec`, `virtual_mic`), `restored` (endpoints restored from the journal) |
| `devices` | `inputs`, `outputs` (lists of `{key,name,transport,channels,form}`; `form` in `microphone`,`speakers`,`headphones`,`headset`,`unknown`), `default_input`, `default_output` (key or null), `virtual_mic` `{kind,present,in_use}`. Emitted after `ready`, on every backend change, and when `in_use` changes (on at once; off after 3 s without a recorder) |
| `flow_bound` | `flow`, `port` |
| `flow_state` | `flow`, `state` `"probing"`/`"up"`/`"link"`/`"closed"`, `rtt_ms` (float or null) |
| `link_out` | `flow`, `p` (list of b64 sealed packets, ≤ 4): only while the flow is in state `link` |
| `stream_state` | `stream`, `state` `"starting"`/`"running"`/`"stopped"`/`"failed"`, `device` (key in use), `error` (code, on failed/stopped by error) |
| `levels` | `t` (ms), `in` `{key:[rms_db,peak_db]}`, `out` `{key:[...]}`, `streams` `{id:[...]}`: dBFS, 0.1 dB, floor -100; only devices Beamer has open |
| `stats` | every 1 s while streams exist: `{"ev":"stats","t":ms,"cpu_pct":<helper process, % of a core>,"streams":{"<id>":{...}}}`; counters are **per interval** (since the previous `stats`). recv: `dir,kind,path` (`udp`/`link`, of the last packet), `jb_ms` (mean level), `jb_p99_ms`, `jb_max_ms`, `target_ms`, `late`, `expand`, `drop` (incl. flushed), `flushed`, `loss_pct`, `fec`, `plc`, `underrun`, `dec_err`, `ppm`, `ppm_i`, `rtt_ms`, `kbps`, `rx`, `rx_link`, `ring_full`, `playing`, `run_jb_p99_ms` / `run_target_ms` (level p99 and mean target since the stream started or `dev reset_stats`). send: `dir,kind,path`, `kbps`, `bitrate`, `jb_ms` (the receiver's level from its rr), `rtt_ms`, `frames`, `enc_us` (mean encode time), `overflow`, `dev_dropped`, `rr_rx`, `enc_err`. loop: `jb_ms`, `target_ms`, `ppm`, `underrun`, `flushed`, `overflow` Send streams with `aec` also carry an `aec` object: `{"on":false,"error":code}` when the reference did not open (the mic runs without echo cancellation; `error` `test_source` for a dev test source, which never gets a canceller), else `on` true, `path` (0..1, how much of the echo estimate is removed: 0 until an echo path has shown, so a reference the mic does not hear is never mixed into it; §7), `ref` (reference device in use), `atten_db` (how much the canceller lowered the mic over the interval: echo and whatever else it removed), `mic_db` and `ref_db` (mean levels it got from the mic and the reference, dBFS), `echo_ms` (the echo delay the filter found, from host timestamps, minus `lead_ms`; -1 unknown), `err_ms` and `ppm` (the reference/mic drift loop: position error and applied correction), `resync` (re-alignments after a gap), `ref_short`, `no_ref` (frames with a short or no reference), `waits` (mic frames that waited for their reference), `us` (mean processing time per 10 ms frame) and `cpu_pct` (canceller CPU, % of a core); `resync`, `ref_short`, `no_ref`, `waits` are cumulative since the stream started |
| `permission` | once after `ready`: `mic` and `system_audio`: `granted`/`denied`/`undetermined`/`unknown`; later `{"mic":"suspect"}` when a mic capture (send or loop) delivered only digital zero for 5 s. Never `suspect` for system sound: a silent machine's tap is digital zero too. Mac `system_audio` (no API reads it): `undetermined` while the first tap waits for the prompt, `granted` once a tap (the helper's probe or a stream) has run and called back, digital silence included, else `unknown`; never `denied` (macOS gives no definite error: a tap that may not record still runs, silent), so `granted` means "taps run here". The Mac re-reads an `undetermined` mic permission every 2 s (each read is synchronous XPC to tccd) and at once when its prompt is answered. Emitted again on each change |
| `ok`, `error`, `pong` | see §3.1. Error codes: `bad_args`, `no_device`, `blocked_device`, `no_virtual_mic`, `device_busy`, `permission`, `unsupported`, `bind_failed`, `unknown_flow`, `unknown_stream`, `internal` |
| `howl` | `stream` (a `system` send), `active` (bool), `freq_hz` (the tone, Hz), `duck_db` (-20 while active, else 0). The howl guard (`native/audio/engine/howl.{h,cpp}`) of every system send stream emits it on each change. Acoustic feedback through two machines: M's sound plays on T's speakers, a microphone on M hears them and something on M (Elgato Wave Link's monitor, Windows "Listen to this device") plays it into M's own sound, which the capture sends again; Windows excludes one process tree only, so the loop cannot be cut at capture. Detection, on the mono mix just before the encoder: 1024-point Hann FFT every 512 samples; a tone (peak +-2 bins, from 800 Hz to 16 kHz: below, voices' first formants made it fire on room speech, WP16/WP18) that holds >= 40 % of the 140 Hz-16 kHz band, has no partial at f/2, 2f or 3f and no other spectral line within 15 dB, and whose level rises at a stable frequency (+-1 bin) by >= 6 dB over 0.26, 0.77 or 2.3 s with all four quarter means non-decreasing and both halves carrying >= 35 % of the rise (so an attack, a step then flat, never counts), ending >= -45 dBFS. Then the stream is ducked by 20 dB (20 ms ramp), held 2 s (doubled per repeat within a minute, at most 30 s) and released over 1 s once the tone has decayed by 12 dB (or gone) for 0.3 s; a tone that never decays under the duck is the source's own sound: released after 4 s, its frequency ignored for 30 s. Samples pass bit for bit while no duck is in force. Engine tests: a loop simulation (gain 1.06-4 at 1.2 or 7.1 kHz, 100-180 ms, with and without a music bed) and music that must pass untouched (chords, a chord swell, sine sweeps, held and swelling flute-like notes, a switched-on tone, a voice-like signal) and voices that must pass untouched (drifting pitch, vibrato, formants from 380 Hz to 2.2 kHz, swelling syllables and a sung note; `BA_HOWL_CLIPS=<dir>` adds recorded sound clips). Known limit: a pure tone fading in slowly looks like feedback and gets the 4 s duck once. |

### 3.4 Example session (Mac sends its mic to the PC; abbreviated)

```
<- {"ev":"ready","proto":1,"version":"0.1.0","platform":"macos","features":["system_capture","local_mute_tap","mic_demand","virtual_mic","aec"],"restored":0}
<- {"ev":"devices","inputs":[{"key":"BuiltInMicrophoneDevice","name":"MacBook Air Microphone","transport":"builtin","channels":1,"form":"microphone"}],"outputs":[{"key":"BuiltInSpeakerDevice","name":"MacBook Air Speakers","transport":"builtin","channels":2,"form":"speakers"}],"default_input":"BuiltInMicrophoneDevice","default_output":"BuiltInSpeakerDevice","virtual_mic":{"kind":"beamer","present":true,"in_use":false}}
-> {"cmd":"flow_open","req":1,"flow":"9f2c01aa","peer":"q2Vx...==","role":"offer","port":0,"host":"192.168.1.134","remote":null,"key":"3q2+7w...=","rr_ms":10}
<- {"ev":"flow_bound","flow":"9f2c01aa","port":52114}
<- {"ev":"flow_state","flow":"9f2c01aa","state":"probing","rtt_ms":null}
<- {"ev":"ok","req":1}
   (Beamer sends audio_flow offer {port:52114}; the PC answers {port:24823})
-> {"cmd":"flow_remote","flow":"9f2c01aa","host":"192.168.1.134","port":24823}
<- {"ev":"flow_state","flow":"9f2c01aa","state":"up","rtt_ms":5.2}
-> {"cmd":"send","req":2,"stream":2882400018,"flow":"9f2c01aa","kind":"mic","source":{"device":"BuiltInMicrophoneDevice"},"codec":{"name":"opus","app":"voip","channels":1,"bitrate":32000,"frame_ms":10,"fec":true},"key":"Zm9v...=","mute_local":false,"aec":{"reference":null}}
<- {"ev":"ok","req":2}
<- {"ev":"stream_state","stream":2882400018,"state":"running","device":"BuiltInMicrophoneDevice"}
```

## 4. Media plane

### 4.1 Packet (from `proto/transport/media.py`, version 1)

24-byte header in clear, authenticated as AEAD associated data:
`version u8 (1) | ptype u8 | flags u8 | codec u8 | stream u32 | seq u64 | ts u32 | nsamp u16 | channels u8 | reserved u8`
(big endian), then ChaCha20-Poly1305 (RFC 8439, 12-byte nonce) ciphertext + 16-byte tag.
- `ptype`: 1 media, 2 hello, 3 ping, 4 pong, 5 rr (receiver report), 6 bye. `flags`: bit0 fec,
  bit1 marker (stream start), bit2 silence. `codec`: 1 opus (0 pcm reserved for debug).
- `stream` = 0 for flow control packets (hello, ping, pong, bye; sealed with the **flow key**);
  otherwise the stream id (media sealed with the **stream key**; rr sealed with the stream key by the receiver).
- Nonce = `dir_stream u32 || seq u64`, where `dir_stream` = stream id with bit 31 = 0 for the stream's
  sender and 1 for its receiver (rr); for stream 0, bit 31 = 0 for the flow's offerer, 1 for the answerer.
  Stream ids are non-zero and drawn with bit 31 clear. `seq` starts at 1 per (key, direction), +1 per packet, never reused.
- Replay window 128 packets per (key, direction); older, duplicate, or failing tag -> dropped silently
  (counted). Media are ordered by `ts` (48 kHz samples from the sender's stream start), not `seq`.
- rr payload (6 bytes): newest `ts` received (u32) + JB level in 0.1 ms (u16). Sent by the network
  thread every `rr_ms` while a stream receives media on this flow and this side sent nothing on the flow
  for `rr_ms` (keeps a power-saving Wi-Fi radio awake, W3). Never over the link.
- ping/pong every 1 s per flow (NTP-style 4 timestamps): RTT for the UI, clock offset for diagnostics.
- AEAD is implemented in `engine/crypto.cpp` on Monocypher's `crypto_chacha20_ietf` + `crypto_poly1305_*`
  primitives exactly as RFC 8439 §2.8 (not Monocypher's incremental AEAD context, whose ratchet is not a
  per-packet construction). Tests: RFC 8439 §2.8.2 vector + vectors generated with PyNaCl
  (`crypto_aead_chacha20poly1305_ietf_encrypt`, what the prototype used).

### 4.2 Flows: who connects to whom

- One flow per pair of direct peers, open while the link is up (2 small packets/s when idle: stream start
  is then immediate and the UI always knows whether UDP works). Closed on link down.
- `audio_hello.udp.listen` says whether a machine may receive unsolicited UDP: `false` on macOS (an
  unsigned listener there pops a firewall dialog), `true` on Windows/Linux.
- **Offerer** = the side with `listen=false`; if both are equal, the smaller machine id (bytes). The offerer
  draws the flow key, opens its socket (`flow_open` with `host` = the answerer's IP and `port` 0: no bind,
  connected towards `host:24823`, ephemeral port; or `port` 24823 if it listens), sends
  `audio_flow {phase:"offer"}` with that port. The
  answerer binds 24823 (24824..24830, then 0), `connect()`s to `offerer_host:offer.port` and starts sending
  hello at once (its first datagram opens its own stateful firewall: the "punch"), then answers with its
  port. The offerer `connect()`s and probes. Both sockets are connected sockets that send first, which is
  what Phase 1 validated without any rule (T6, V3). 24823 is a default carried in the messages, so falling
  back to an ephemeral port (if WP4 finds that an explicit bind prompts the Windows firewall) changes no
  protocol.
- Host for UDP = the peer entry's `host` (link address). Loopback (the Mac's SSH tunnel fallback,
  127.0.0.1:24822) or no host -> no UDP: the flow starts in `link` state.
- The answerer can ask for a flow with `audio_flow {phase:"want"}` (e.g. after its helper restarted).

### 4.3 Streams on a flow

Any number per flow, both directions: mic and system sound of either side, multiplexed by stream id.
Per machine pair at most one `mic` stream and one `system` stream per direction (§6.4).

### 4.4 Codec and buffer parameters (from T6/W3; `codec` object on the wire and in `send`/`recv`)

| kind | codec object |
|---|---|
| mic | `{"name":"opus","app":"voip","channels":1,"bitrate":32000,"frame_ms":10,"fec":true}` + encoder: `PACKET_LOSS_PERC 10`, DTX off, complexity **5** |
| system | `{"name":"opus","app":"audio","channels":2,"bitrate":96000,"frame_ms":10,"fec":false}`, complexity 9 |

Optional `"complexity"` (0..10) overrides the encoder complexity; default 5 for `app` `voip`, 9 otherwise.
The mic default was lowered from 9 (WP2, operator decision): the encoder CPU halves (in-helper encode
118-160 us/frame at 5 on the M5), masked log-spectral distance on voiced speech 2.59 dB at 5 vs 2.35 dB
at 9 (2.42 dB at 6).

Jitter buffer (port `JitterBuffer` from `proto/transport/media.py`, current defaults): pull model from the
render callback; target = render period + frame/2 + p97(transit, 10 s window, spikes > 150 ms excluded)
+ 2 ms, clamped **10..120 ms**, rises at once, decays over ~2 s; playback-clock jitter provision only when
the backend reports irregular callbacks; FEC from the next packet else PLC; underrun = PLC without advancing
the timeline, re-buffer after 3 frames; silent-frame drop when > max(5 ms, 0.75 frame) above target;
catch-up by resampling up to +2000 ppm when > 4 ms above target; non-silent drop only after 2 s above
max(30 ms, frame); flush to target when above target + max(40 ms, target); dropped/flushed frames are
**decoded and discarded**; peak hold 10 s after 2 spikes > max(15 ms, p97 + 5 ms). Drift: level EWMA 1 s
minus target -> 40 ppm/ms + integral 0.1 ppm/(ms.s) clamped ±200 ppm, total ±500 ppm; resampler =
16-tap Kaiser-windowed sinc, 512 phases. The local loop (§3.2 `loop`) uses the same controller on its ring.

Engine deviations from `media.py` (WP2; both off = the reference, verified identical by
`native/audio/tests/jb_crosscheck.py`, 0.000 ms level/target difference over 225,001 pulls):
- `merge_expand` (on): a frame concealed by expansion (nothing newer had arrived) that then proves lost
  (a newer frame came, it never did) is not concealed a second time: the expansion was its concealment.
  In `media.py` each such loss added a frame of latency that catch-up takes ~5 s to remove; with bursty
  loss the buffer sat 10-20 ms above target (now within 5 ms). Late frames keep the reference behaviour.
- `jitter_recalc_ms` 20: the p97 transit statistic is recomputed at most every 20 ms instead of after every
  packet (it was most of the render thread's CPU).

### 4.5 TCP fallback

A flow whose side has received no valid UDP packet for 2 s while the link is up goes to state `link`: the
helper emits `link_out` (sealed packets, batched 2 per 20 ms, rr never) and Beamer posts them as
`audio_frame`; incoming `audio_frame` -> `link_in`. Packets from both paths are accepted at any time (the
replay window removes duplicates). In `link` state the sender caps Opus at 32 kb/s for mic and 48 kb/s for
system sound and queues its media for `link_out` **and also keeps sending them over UDP** (the receiver
drops the duplicates), plus hello every 1 s and rr: UDP recovery is then seen at once and, after 1 s of
received UDP, the flow returns to `up` (measured 1.0 s; fallback 2.0 s). A side that receives `link_in`
packets (the peer cannot hear its UDP) also goes to `link` until 250 ms after the last one, so a one-way
UDP blockage falls back in both directions. Beamer
drops `link_out` packets (counted in the stream stats, heard as PLC) while the link's outbox holds > 64
messages (`AudioHost.outbox_depth`), so audio can never starve input acks (`receiver.py:1003-1020`).

## 5. Wire protocol on the Beamer link

### 5.1 Rules

- Cap `audio` is added to `hello`/`welcome` caps (13 -> 14 of `MAX_CAPS` 16) only when
  `feature_flags.AUDIO` is on and the helper binary exists. Wire version byte stays `0x06`.
- `audio_*` messages go only to peers whose caps contain `audio` (`_CAP_FOR` on initiator links, `_Link.post`
  on responder links) and are only handled when both ends advertise it. Old peers never receive them and
  would ignore them anyway. No `seq` (never input types). Each message < 16 KiB except `audio_devices` /
  `audio_route` (< 32 KiB by the limits below).
- Link choice: the WIRE.md "which link" rule (the link this machine opened when up, else the other).
- Handlers on link threads only enqueue to the AudioService (§8.1).
- Versioning: `audio_hello.v` = 1 and `min_v` = 1. A peer whose `min_v` > our `v` is treated as not audio
  capable. New behaviour is announced by words in `features`, never by new caps. Unknown fields ignored;
  a malformed `audio_*` message is ignored (never ends the link).
- Validation limits: ≤ 32 inputs and ≤ 32 outputs per machine, `key` ≤ 200 chars, `name` ≤ 128,
  ≤ 33 machines in a route, ≤ 64 `forms` entries (when full, the oldest `form:null` ones go first),
  keys b64 of exactly 32 bytes, stamps as `settings_sync` (int, ≤ 2^53-2,
  ≤ now + 1 day).

### 5.2 Messages

| type | data | scope / when |
|---|---|---|
| `audio_hello` | `{"v":1,"min_v":1,"platform":"macos","helper":"ok"|"starting"|"failed","udp":{"listen":false},"codecs":["opus"],"features":[...]}` | direct; on link up, and when `helper` changes |
| `audio_devices` | `{"machine":id,"rev":int,"name":str,"platform":str,"status":"ok"|"starting"|"failed","helper":"0.1.0","inputs":[Device],"outputs":[Device],"default_input":key|null,"default_output":key|null,"virtual_mic":{"kind":"beamer"|"vbcable"|null,"present":bool,"in_use":bool},"features":[...],"links":[id,...]}`, Device = `{"key","name","transport","channels","form"}` (form already overridden, §6.1) | flooded (§5.3) |
| `audio_route` | §6.2 | flooded, per-entry merge |
| `audio_flow` | `{"flow":"9f2c01aa","phase":"offer","port":52114,"key":b64}`; `{"flow","phase":"answer","port":24823}`; `{"phase":"want"}`; `{"flow","phase":"close"}` | direct |
| `audio_open` | `{"stream":u32,"flow":str,"kind":"mic"|"system","codec":{...},"key":b64,"sink":{"virtual_mic":true}|{"device":DeviceRef|null},"for":{"set_at":int,"by":id}}` | direct, sender -> receiver |
| `audio_opened` | `{"stream":u32,"ok":bool,"error":"no_device"|"no_virtual_mic"|"conflict"|"helper"|"unsupported"|null}` | direct, receiver -> sender |
| `audio_close` | `{"stream":u32,"reason":"route"|"demand"|"device_gone"|"helper"|"shutdown"|"error"}` | direct, either side |
| `audio_frame` | `{"flow":str,"p":[b64,...]}` (≤ 4) | direct, fallback only |
| `audio_watch` | `{"on":bool}` | direct; the sender's Audio page is visible |
| `audio_stats` | `{"t":ms,"levels":{"in:<key>":[rms,peak],"out:<key>":[...]},"streams":{"<id>":{"path":"udp","jb_ms":23.0,"loss_pct":0.1,"rtt_ms":5.1}},"howls":[{"to":id,"freq_hz":7100.0,"active":true}]}`; `howls` (≤ 8, left out when empty): the sender's system sound to `to` was ducked by its howl guard (§3.3 `howl`), `active` false during the notice hold (120 s) after the release | direct, 4 Hz, only to peers watching |
| `audio_test` | `{"device":key,"seconds":10}` | direct; meter one idle input of the receiver (its stats carry the level) |

### 5.3 Cluster propagation (reuses the `layout` flood rule of `settings_sync`)

- **Device lists**: each machine is the only author of its own `audio_devices`. `rev` = max(now in ms,
  last rev + 1), the last rev persisted in `audio.json` so it never goes back. A receiver stores a list if
  `rev` is higher than the one held for that `machine` (or it came directly from that machine), persists it,
  and forwards it unchanged to every other direct peer. On link up each side sends its own list and every
  list it holds. A machine republishes its list when devices, defaults, `in_use`, status, form overrides or
  its direct-peer set (`links`) change.
- **Online**: machine X is online for Y when X is reachable from Y in the graph whose edges are the `links`
  of Y's own direct peers and of the held lists of online machines (BFS from Y; edges only count when both
  ends list each other). When a link drops, both ends republish with a smaller `links`, the flood updates
  everyone, and X turns offline without timers. Offline machines stay visible (greyed) from the cache.
- **Route**: §6.2; on link up each side sends its whole route; on receipt, entry-wise merge; if anything
  changed, forward the merged route to every other direct peer; if the sender lacked an entry we hold newer,
  answer it with ours (as `settings_sync.merge`'s `answer`).
- Third parties: a Linux machine on official 1.5.x linked only to the PC never advertises `audio`; it is
  simply not in the audio view. A Mac not paired with a machine still sees its devices through the flood,
  and can set routes involving it; only the two machines of a stream must be direct peers.

### 5.4 Link loss and reconnect

Link down with P: every stream and the flow with P stop at once (the sender stops capturing: a Windows
endpoint mute or Mac tap ends, so a machine whose sound went to P plays locally again within ~1 s); P is
removed from `links` (republish). Link up: `audio_hello` both ways, then devices and route, then the offerer
opens the flow, then each side's planner opens what it should send. Helper restart: every stream of this
machine is closed with `audio_close {reason:"helper"}` to its peers, status `starting` is published, and
once `ready` arrives status `ok` is published and everything is re-planned.

## 6. Routing model

### 6.1 What Alex sets (cluster-wide, persisted on every machine)

1. **Active microphone**: `(machine, device)`; `device` null = that machine's default input (followed);
   `machine` null = off.
2. **Output of each machine M**: `to` = null (play on M, nothing done) or machine T ≠ M, and `device` on T
   (null = T's default output, followed). When `to` is set, M's sound is captured (all processes except
   Beamer and the helper) and **muted on M** while it streams.
3. **Form override** per device (`headset` / `speakers` / null = what the OS says), feeds the echo warning
   and the AEC switch.

Local only (not on the wire): `aec` mode `auto`|`off` (per machine), last page selection.

### 6.2 Route document (`audio_route` data, and `route` in `audio.json`)

```json
{"mic":    {"machine":"<id>|null","device":{"key":"BuiltInMicrophoneDevice","name":"MacBook Air Microphone"},"set_at":1793000000,"by":"<id>"},
 "outputs":{"<M id>":{"to":"<T id>|null","device":{"key":"...","name":"Logitech G733"}|null,"set_at":1793000100,"by":"<id>"}},
 "forms":  {"<machine id> <device key>":{"form":"headset|speakers|null","set_at":1793000200,"by":"<id>"}}}
```

Each entry is stamped on its own: newer = higher `set_at`, tie -> larger `by` as bytes (`settings_sync._newer`);
a change made here takes `settings_sync.next_stamp(held, now)`. Merge is entry-wise, so concurrent edits of
different machines' outputs on two UIs both survive. Entries are never deleted (an output goes back to
`to:null`, the mic to `machine:null`). Initial state: mic off, every output local.

### 6.3 "The machine he works on": on-demand mic, no focus rule

The active mic feeds the virtual mic of **every** audio machine; it actually streams to machine T only
while an app on T records T's virtual mic (`virtual_mic.in_use`):
- Mac: listener on `kAudioDevicePropertyDeviceIsRunningSomewhere` of `com.beamer.audio.mic` (the driver
  counts IO per device, `BeamerAudio.c` `ioCount`, so the feed does not count). Windows: audio sessions on
  "CABLE Output" (`IAudioSessionManager2` + `IAudioSessionNotification` + per-session
  `OnStateChanged`), Active state, any process.
- Why: pointer focus would cut the mic off a call whenever the pointer visits another screen; an explicit
  "target" setting is one more thing to set and to forget; always-on streaming would keep the source's
  microphone (and its privacy indicator) on permanently. On demand needs no setting, follows whatever app
  is actually in a call, and costs ~150 ms of silence at call start (detection + `audio_open` + capture
  start + JB fill). If several machines record, each gets a stream.
- If a machine lacks `mic_demand` in `features`, its demand counts as always true while a mic is active.
- When the active mic is on T itself and T's virtual mic is in use, T runs a local `loop` (so apps can keep
  "Beamer Microphone"/"CABLE Output" selected whichever mic is active).
- Demand off is held 3 s by the helper and 5 s more by the planner before a stream closes.

### 6.4 Planner (`core/audio/planner.py`, pure)

```python
def plan(own: str, machines: Mapping[str, MachineAudio], route: Route, direct: frozenset[str],
         online: frozenset[str], aec_mode: str, now: float, demand_hold: Mapping[str, float]) -> Plan

@dataclass(frozen=True)
class SendSpec:  kind: str; peer: str; source: str | None; sink: Mapping; mute_local: bool
                 aec: bool; aec_reference: str | None    # reference output key, None = default output
@dataclass(frozen=True)
class Plan:
    sends: Mapping[tuple[str, str], SendSpec]   # (kind, peer) -> spec, only streams this machine sends
    loop: LoopSpec | None
    mic_status: RouteStatus
    output_status: Mapping[str, RouteStatus]    # every known machine
    wake_at: float | None                       # next time the plan may change by itself (demand hold)
```

Rules, evaluated identically on every machine (each acts only on `sends`/`loop` of its own):
1. **Mic** (S = `route.mic.machine`): resolve the device on S (key, else unique name match, else
   missing -> `waiting_device`). For each online T ≠ S with demand (or hold): if T ∈ S's direct peers and T's
   `virtual_mic.present` -> send `("mic", T)` from S with `sink {"virtual_mic":true}`; else status lists T as
   `unreachable` / `no_virtual_mic`. S with its own demand -> `loop`. Each mic `SendSpec` gets `aec=True` and
   `aec_reference` = S's default output key when `aec_mode == "auto"`, S's helper lists `aec` in its `features` and S's output form (override first) is
   not `headphones`/`headset`, and S's default output plays something: not when S's own sound streams to
   another machine (muted on S) while no other machine's sound streams to that output (`speakers_idle`)
   (helper `aec {"reference": key|null}`, else `aec: null`). Never on the loop: apps
   on S cancel their own playback.
2. **Output of M** (`route.outputs[M]`, `to` = T): requires M and T online and direct peers, M has
   `system_capture`, T's device resolvable (null -> T's `default_output` exists). **Conflict**: T has
   `local_mute_endpoint` (Windows), T's own output is redirected away, and the resolved device on T is T's
   default output (that endpoint is muted) -> `conflict`, no stream. Otherwise M sends `("system", T)` with
   `mute_local=True` and `sink {"device": ref}`. Any unmet condition -> M plays locally, status says why
   (`waiting_device`, `waiting_machine`, `unreachable`, `conflict`).
3. Streams are keyed `(kind, peer)`: the service diffs the new plan against running streams: new key ->
   `audio_open`, removed key -> `audio_close {reason:"route"|"demand"}`, changed spec -> close then open.
4. Receivers are passive: they accept an `audio_open` from a direct peer when the sink resolves, is not
   blocked and is not in conflict (same rule as 2), else `audio_opened {ok:false, error}`; the sender marks
   that stream failed and retries only when the receiver's device list or the route changes.
5. A device that disappears: the owner's list loses it -> every planner re-evaluates -> streams to/from it
   close, the status becomes `waiting_device` (name kept from the ref); the source plays locally meanwhile.
   When it comes back (same key, or a unique name match: a USB device moved to another port), the plan
   reopens the streams. Nothing has to be set again.

`RouteStatus = {state, reason, params}`; states: `off`, `local`, `ready` (mic chosen, no app recording),
`streaming`, `waiting_device`, `waiting_machine`, `unreachable`, `no_virtual_mic`, `conflict`, `error`.

## 7. Echo cancellation and the headset warning

- **Where**: on the machine S that captures the active mic, inside the engine, between capture and the
  encoder of forwarded mic streams (not the local loop). **What**: speexdsp 1.2.1 `mdf.c` (frame 480, filter 4800 = 100 ms)
  + `preprocess.c` residual echo suppression, float build, vendored files only
  (`mdf.c preprocess.c fftwrap.c kiss_fft.c kiss_fftr.c filterbank.c` + headers), behind an
  `EchoCanceller` interface so WebRTC AEC3 can replace it later.
- **Reference**: a `CaptureKind::Reference` capture of S's output device: Mac = a non-muting process tap
  of that device's stream (all processes, Beamer's own playback included); Windows = endpoint loopback of
  that render endpoint. Aligned to the mic by host timestamps (coarse delay = capture time difference, the
  remaining acoustic delay ~44 ms on the MacBook is inside the filter), drift-corrected by the same
  resampler/controller when mic and output are on different clocks.
- **When**: `aec` set by the planner (§6.4 rule 1): mic forwarded to another machine, S's output not a
  headset and playing something, mode `auto` (Alex's rule: a forwarded mic and local speakers both
  active). Never on a dev test source (no room reaches it). The engine also checks that there is an
  echo path at all (WP18): without one (speakers muted or out of the mic's reach) speexdsp's MDF never
  leaves its fast initial adaptation, its filter wanders with the near end and subtracting its estimate
  put the reference into the forwarded mic (WP16: -49 dBFS, 674 markers), while the residual echo
  suppressor, fed the raw reference until MDF adapts, ducked the mic by up to 12.8 dB. So the output is
  `notch(mic) - w * y` (y = MDF's echo estimate) and the RES runs only while `w` > 0.5; `w` rises to 1
  once >= 12 of the last 100 frames had MDF remove >= 8 dB of the mic (held 15 s; 0 false openings in
  40 simulated minutes of uncorrelated mic and reference), times a ramp of `<mic,y>/<y,y>` (~1 for a
  real echo, double talk included), and drops to 0 when the estimate keeps making the mic >= 1 dB louder
  (30 of 100 frames: the path went away). Cost: about 2 s of uncancelled echo when a stream starts. This covers Alex's echo case (the PC's sound sent to the Mac speakers while the Mac
  mic feeds the PC call) and any local sound of S.
- **Why not OS voice processing**: macOS VoiceProcessingIO drives the default devices, adds AGC/noise
  suppression, ducks other audio and is documented to cancel its own output only; the Windows Voice
  Capture DSP/communications AEC depends on the endpoint's APOs or runs at 16 kHz. A canceller with an
  explicit reference behaves the same on both machines and is testable offline.
- **Headset detection**: `form` from the backend: Mac transport Bluetooth/USB + name/data source
  (`kAudioDevicePropertyDataSource` `'hdpn'` on the built-in jack) -> headphones/headset; Windows
  `PKEY_AudioEndpoint_FormFactor` (Headphones = 3, Headset = 5, Speakers = 1). Override per device in the
  route `forms` (UI toggle "This is a headset").
- **Warnings** (computed by `core/audio/view.py`, §9.1): `echo_aec` (S forwards its mic and its output is
  speakers: "Beamer cancels the echo of <S>'s speakers. A headset on <S> sounds better.");
  `echo_cross` (T records the mic and plays its own sound on speakers: "If <T>'s speakers can be heard by
  <mic>, people in a call on <T> hear themselves. Use a headset, or send <T>'s sound to <S>, where Beamer
  cancels it.").

## 8. Python control plane

### 8.1 AudioService (`core/audio/service.py`)

```python
class AudioHost(Protocol):          # implemented by mac_app/audio_mac.py and win_app/audio_win.py
    def own_id(self) -> str: ...
    def own_name(self) -> str: ...
    def platform(self) -> str: ...                     # "macos" | "windows" | "linux"
    def post(self, peer: str, message: dict) -> bool: ...   # non-blocking, WIRE.md "which link" rule
    def outbox_depth(self, peer: str) -> int: ...      # queued messages on the link used for `peer`
    def peer_host(self, peer: str) -> str | None: ...  # IPv4 from the peer entry; None/loopback = no UDP
    def peer_name(self, peer: str) -> str: ...

class AudioService:
    def __init__(self, host: AudioHost, state_dir: Path, helper_cmd: list[str] | None,
                 *, clock=time.monotonic, wall=time.time, helper_factory=None, logger=None): ...
    def start(self) -> None
    def stop(self, timeout: float = 2.0) -> None
    # link threads (enqueue only, never block, never raise)
    def link_up(self, peer: str, caps: frozenset[str]) -> None
    def link_down(self, peer: str) -> None
    def link_message(self, peer: str, message: dict) -> None
    # UI / any thread
    def snapshot(self) -> AudioSnapshot
    def subscribe(self, fn: Callable[[], None]) -> Callable[[], None]          # called on the worker thread
    def subscribe_levels(self, fn: Callable[[Mapping[str, tuple[float, float]]], None]) -> Callable[[], None]
    def set_mic(self, machine: str | None, device: DeviceRef | None) -> None
    def set_output(self, machine: str, to: str | None, device: DeviceRef | None) -> None
    def set_form(self, machine: str, device_key: str, form: str | None) -> None
    def set_aec(self, mode: str) -> None                                       # "auto" | "off"
    def set_vmic_exclusive(self, on: bool) -> None   # Windows, local: helper restarted with --vmic-exclusive
    def watch(self, on: bool) -> None                                          # page visible
    def test_input(self, machine: str, device_key: str) -> None
    def retry_helper(self) -> None
```

- One worker thread `Beamer-audio` consumes a `queue.Queue` of events (link, helper, UI, timers); all state
  lives there. `snapshot()` returns the last immutable `AudioSnapshot` (swapped atomically). `subscribe`
  callbacks run on the worker; the apps hop to their UI thread (`AppHelper.callAfter` / a Qt `Signal`).
  Levels are coalesced to ≤ 20 Hz; keys `"<machine>/in/<key>"`, `"<machine>/out/<key>"`.
- Flow ids: 8 hex chars from `secrets`; stream ids: `secrets.randbits(31)` redrawn while 0 or in use; keys: `secrets.token_bytes(32)`.
- `watch(True)` -> `meter on` locally and `audio_watch {on:true}` to direct peers; their `audio_stats`
  feed the levels map. `watch(False)` stops both.
- **Dev command files** (only when `<state_dir>/audio-dev.enabled` exists; adds `--dev` to the helper; the file
  may hold `{"helper": [argv...]}` to run another helper, e.g. `beamer-audio-null`, without env vars):
  every 1 s read and delete `<state_dir>/audio-dev-cmd.json` (`{"set_mic":...}`, `{"set_output":...}`,
  `{"test_source":{"kind":"mic","signal":"A"}}`, `{"set_aec":"off"}`, `{"set_vmic_exclusive":true}`) and write `<state_dir>/audio-dev-status.json` (snapshot +
  last `stats` + `test_source`, the dev source in force: a harness that sets it on one machine waits for it
  there before setting a route that opens the stream, else the stream opens on the real device and opens
  again on the test source). Used by the E2E harness (§11.4); no socket, no listener.

### 8.2 HelperProcess (`core/audio/helper.py`)

- Binary: `audio-dev.enabled` `helper`, else env `BEAMER_AUDIO_HELPER`, else frozen `Path(sys.executable).parent / "beamer-audio[.exe]"`
  (py2app: `Contents/MacOS/`; PyInstaller one-file: the folder of `Beamer.exe`), else
  `<repo>/native/audio/build/{mac,win}/beamer-audio[.exe]`. `available()` = feature flag on and the file
  exists (the Mac/PC adapters use it to decide the cap).
- `subprocess.Popen(cmd, stdin=PIPE, stdout=PIPE, stderr=PIPE, bufsize=0)`; Windows
  `creationflags=0x08000000` (CREATE_NO_WINDOW). Reader threads for stdout (JSON -> worker queue) and stderr
  (log). Writes under a lock, one line each.
- Watchdog: `ping` every 5 s; no `pong` in 10 s, or EOF on stdout -> kill, mark `starting`, restart after
  1, 2, 4, 8, 16, 30 s; 5 failures within 2 min -> `failed` (UI shows Retry). On every Beamer start the
  helper replays the Windows journal before `ready`, so an endpoint muted by a crashed helper is restored.

### 8.3 Snapshot (`core/audio/model.py`)

```python
# one-line shorthand: each is a @dataclass(frozen=True); Mapping fields default to empty, never shared
@dataclass(frozen=True) class DeviceRef: key: str; name: str
@dataclass(frozen=True) class Device: key: str; name: str; kind: str; transport: str; channels: int; form: str; default: bool
@dataclass(frozen=True) class VirtualMic: kind: str | None; present: bool; in_use: bool
@dataclass(frozen=True) class MachineAudio:
    id: str; name: str; platform: str; rev: int; status: str        # ok | starting | failed | no_helper
    inputs: tuple[Device, ...]; outputs: tuple[Device, ...]
    default_input: str | None; default_output: str | None
    virtual_mic: VirtualMic; features: frozenset[str]; links: frozenset[str]; helper: str
@dataclass(frozen=True) class Stamp: set_at: int; by: str
@dataclass(frozen=True) class MicRoute: machine: str | None; device: DeviceRef | None; stamp: Stamp
@dataclass(frozen=True) class OutputRoute: to: str | None; device: DeviceRef | None; stamp: Stamp
@dataclass(frozen=True) class FormEntry: form: str | None; stamp: Stamp
@dataclass(frozen=True) class Route: mic: MicRoute; outputs: Mapping[str, OutputRoute]; forms: Mapping[tuple[str, str], FormEntry]
@dataclass(frozen=True) class RouteStatus: state: str; reason: str | None = None; params: Mapping[str, str] = {}
@dataclass(frozen=True) class StreamView: stream: int; kind: str; source: str; target: str; state: str; path: str; jb_ms: float | None; loss_pct: float | None; rtt_ms: float | None
@dataclass(frozen=True) class AudioSnapshot:
    own: str; machines: Mapping[str, MachineAudio]; online: frozenset[str]; direct: frozenset[str]
    route: Route; mic_status: RouteStatus; output_status: Mapping[str, RouteStatus]
    streams: tuple[StreamView, ...]; helper: str; permission: Mapping[str, str]; aec_mode: str; aec_active: bool
    howls: tuple[HowlView, ...] = ()   # HowlView: source: str; target: str; freq_hz: float; active: bool
```

Pure functions in `model.py`: `merge_route(held, incoming) -> (merged, changed, answer)`,
`accept_devices(held, incoming, direct_from) -> bool`, `online(own, direct, machines) -> frozenset`,
`resolve(ref, devices) -> Device | None` (key, else unique name).

### 8.4 Persistence: `<state_dir>/audio.json`

`{"version":1,"route":{...§6.2},"own_rev":int,"machines":{"<id>":{...last audio_devices data}},"local":{"aec":"auto","vmic_exclusive":false}}`,
written atomically on every change (debounced 500 ms). `local` is this machine's own and never sent:
`vmic_exclusive` (Windows; default off = shared render into CABLE Input, ~70 ms; on = exclusive, ~35 ms,
but no other app can play into CABLE Input meanwhile, WP12) adds `--vmic-exclusive` to the helper's
command line; changing it restarts the helper (streams drop ~1 s and reopen on `ready`; events of the
replaced helper are dropped). The helper names `vmic_exclusive` in `ready.features` only when started
with the flag (and still falls back to shared if the exclusive open is refused, logged).
`AudioSnapshot.vmic_exclusive` carries the local value. A separate file keeps `settings.json`, its schema,
migrations and `PeerBook` lock untouched. Loading applies the route at once, so routes come back after a
restart and are re-applied as soon as links and devices allow.

## 9. UI

### 9.1 Pure page model (`core/audio/view.py`, shared by both apps)

```python
def page_model(snap: AudioSnapshot, tiles: Mapping[str, tuple[float, float, float, float]],
               selected: str | None, own_label: str) -> AudioPage
```
`tiles` = the shared Crossing layout (`settings["layout"]` tiles `{x,y,w,h}` per machine, as the Crossing
map uses; machines without a tile are appended in a row). `AudioPage` holds: `mic_choices` (grouped by
machine: `(machine, DeviceRef|None, label, enabled)` + "Off"), `mic_line` (status sentence), `tiles`
(per machine: frame, name, platform, online, `has_active_mic`, `output_line` e.g. "Sound plays here" /
"Sound -> lebig", meter keys), `arrows` (`(from, to, "mic"|"sound", state)`), the selected machine's
`output_choices` (here + each direct peer's outputs and "default output"), `inputs`/`outputs` rows (name,
default mark, form, meter key, test enabled), `notices` (code, sentence, action), all strings through
`core.i18n` (`N_`/`tr`), so `core/tests/test_i18n_coverage.py` keeps passing.
Notices: `driver_missing` (Mac, action "Install Beamer Microphone"), `vbcable_missing` (PC: "To use another
machine's mic here, install VB-CABLE (vb-audio.com), then choose 'CABLE Output' as the microphone in your
apps."), `pick_virtual_mic` (how apps pick it, per platform), `permission_mic`/`permission_system`
(action opens System Settings), `helper_failed` (action Retry), `echo_aec` (only where S's helper has `aec`
and, for this machine, `aec_mode` is `auto`; else `echo_no_aec`: "<S>'s microphone can hear its speakers, and Beamer
does not cancel that echo there. Use a headset on <S>.", and `echo_cross` drops its "send the sound to S" advice),
`howl` (one per sound stream in `snapshot.howls`, ours or a watched peer's: "Beamer heard feedback between <T>'s
speakers and a microphone on <M>, and turned down <M>'s sound on <T>. Use headphones on <T>, or stop <M> from playing
that microphone through its own sound."; when M lists an Elgato/Wave Link device the advice is "or turn off Wave
Link's monitor to the system output on <M>"), `echo_cross`, `conflict`,
`peer_no_audio` (a linked machine runs a Beamer without audio).
`MachinePanel.vmic_exclusive_*`: on this machine's own panel only, when it is Windows with VB-CABLE present,
a switch "Low-latency virtual microphone" + "About 35 ms instead of 70 ms, but no other app can play into
CABLE Input meanwhile." (+ "This version of the audio helper cannot do it." when it is on, the helper is
ready and its features lack `vmic_exclusive`); `apply_vmic_exclusive(service, panel, on)`. Never on a Mac,
never for a peer: each machine sets its own.

### 9.2 Layout (both apps, same order and words)

Page "Audio" after "Crossing" (Mac SF Symbol `speaker.wave.2`; purpose: "Every machine's microphones and
speakers. Choose the microphone your apps hear and where each machine's sound plays.").
1. **Microphone** module: selector "Microphone your apps hear" + level meter + status line + one-line hint
   ("Apps use 'Beamer Microphone' on a Mac and 'CABLE Output' on a PC").
2. **Notices** strip, right under the Microphone module (hidden when empty), so a missing driver, a denied
   permission or an echo warning is seen without scrolling.
3. **Machines** module: `AudioMap`, read-only, drawn like the Crossing map (same tile shapes, colours,
   fonts and theme tokens: Mac `theme`/`tokens`, Qt `win_app/theme`/`tokens`), Core Animation layers on the
   Mac (like `diagram.AtlasCanvas`), `QPainter` on Qt; each tile: name, mic glyph when it holds the active mic,
   output line, a thin level bar; arrows for active routes (sound solid, mic dashed, grey when waiting).
   Click a tile = select it (keyboard: arrows/Tab as on the Crossing map).
4. **Selected machine** module (ledger-like rows): "Sound from <M> plays on" selector; note "Muted on <M>
   while it plays elsewhere"; status; on a PC's own panel the "Low-latency virtual microphone" switch and
   its note (§9.1); Inputs rows (name, meter, Test); Outputs rows (name, meter,
   "Headset" switch).
Meters: a `LevelMeter` widget (horizontal bar, RMS fill + peak tick, -60..0 dBFS), only for devices with a
level key in the levels map. While the page is visible the app calls `service.watch(True)`.

### 9.3 Data flow and threading

Service worker -> `subscribe` callback -> UI thread -> `snapshot()` -> `page_model()` -> widgets update
(diff by key, no rebuild while a menu is open). User action -> service setter (thread-safe, enqueues).
Levels -> `subscribe_levels` -> UI thread -> meters only. Nothing in the UI blocks on the helper or the link.

## 10. Packaging and permissions

### 10.1 Mac

- `build_dmg.sh` runs `native/audio/build-mac.sh`, copies `build/mac/beamer-audio` to
  `$APP/Contents/MacOS/beamer-audio` and `build/mac/BeamerAudio.driver` + `mac-driver/install-driver.sh` to
  `$APP/Contents/Resources/audio/`, **before** `sign_app`. The universal check already covers the helper.
  `sign_app`: sign `Contents/MacOS/beamer-audio` with `--options runtime --entitlements
  mac_app/entitlements-audio.plist` (`com.apple.security.device.audio-input` only); sign the driver **as a bundle**
  (`codesign ... "$APP/Contents/Resources/audio/BeamerAudio.driver"`, no hardened-runtime requirement from
  coreaudiod) before the outer bundle; skip its inner Mach-O in the nested loop.
- `entitlements.plist` (bundle + `python`) gains `com.apple.security.device.audio-input`.
- `setup.py` plist: `NSMicrophoneUsageDescription` "Beamer sends this Mac's microphone to the machine
  you are working on when you choose it on the Audio page." and `NSAudioCaptureUsageDescription` "Beamer
  sends this Mac's sound to another machine when you choose it on the Audio page."
- TCC: the helper is spawned by Beamer, so macOS treats Beamer.app as the responsible process: the prompts
  name Beamer and use Beamer's Info.plist strings, once each (Microphone at the first mic capture,
  System Audio Recording at the first tap). The helper reports `permission` (`AVCaptureDevice
  authorizationStatusForMediaType:` for the mic; `suspect` from 5 s of digital zero for the tap).
  `Install-Beamer-Mac.command` adds `Microphone` and `AudioCapture` to the services it resets for ad-hoc builds.
  First real prompt and grant need Alex (human check H1).
- macOS < 14.2: no process taps; the helper omits `system_capture`/`local_mute_tap` (weak-linked, `@available`).
- **Driver**: dev keeps `beamer-hal` (NOPASSWD) with staging dir `~/Library/Application Support/Beamer/hal-staging`
  (build-mac.sh stages there as the prototype did). Shipped: `audio_driver.py` compares the bundled and
  installed `CFBundleVersion`; the page offers "Install Beamer Microphone" (or "Update"), which runs
  `/usr/bin/osascript -e 'do shell script "<Resources>/audio/install-driver.sh" with administrator
  privileges'` (copy to `/Library/Audio/Plug-Ins/HAL`, verify signature/bundle id as `beamer-hal` does,
  `killall coreaudiod`). It warns first that all audio stops for a second, and is only offered while no
  stream runs. Uses `beamer-hal` instead when that helper exists (dev Macs).

### 10.2 Windows

- `build_win_app.ps1` runs `native\audio\build-win.cmd`, signs `beamer-audio.exe` with
  `Sign-BeamerArtifact`; `Install-Beamer.ps1` stops `beamer-audio` processes from the install dir, copies
  the exe next to `Beamer.exe` (same retry loop), then runs `beamer-audio.exe restore` once;
  `Beamer-Setup.iss` `[Files]` += `native\audio\build\win\beamer-audio.exe` (DestDir `{app}`); the portable
  build adds it to its `COLLECT`. Not embedded in the one-file exe (it would be unpacked to a new temp path
  at every start).
- No firewall changes (`firewall_win.py`, `Install-Maintenance.ps1` untouched).
- VB-CABLE: never bundled or installed by Beamer (VB-Audio's licence; and the system change is Alex's).
  Absent -> `virtual_mic.present=false` -> notice. Beamer never changes default devices.

## 11. Testing

### 11.1 Python (`core/tests/test_audio_*.py`, `unittest`, run from the repo root on both machines)

`mac_app/.venv/bin/python -m unittest discover -s core/tests -p "test_audio*" -t .` and the same with
`win_app\.venv\Scripts\python.exe` on the PC.
- `test_audio_wire`: builders/validators, limits, malformed input ignored. `test_audio_model`: route merge (commutative, idempotent, per-entry, ties by `by`), device-list acceptance,
  `online` BFS, `resolve` (key, unique name, ambiguous name -> None).
- `test_audio_planner`: table-driven: mic on demand (0/1/2 consumers, local loop), holds, missing device and
  return, output redirect, Windows conflict, unreachable (not direct), no virtual mic, AEC on/off by form/override.
- `test_audio_service` with `FakeHelper` and a fake host: open/close sequences, flow offer/answer/roles,
  fallback posting and outbox cap, helper crash/restart (journal replay announced), dev command files.
- `test_audio_cluster` with `audio_cluster_harness.py` (N services, in-memory links with caps, link
  up/down, partitions): convergence of routes and lists, a 1.5.x peer (no `audio` cap) receives nothing,
  reconnect re-applies the route, unplug/replug, three machines with a non-direct pair.
- `test_audio_helper`: `HelperProcess` vs `python -m core.audio.fake_helper` (spawn, EOF, watchdog, backoff,
  stderr rate limit); `test_audio_helper_native`: the real helper, null backend (skipped when not built).
- `test_audio_view`: page model from fixture snapshots (`audio_fixtures.py`), notices, i18n.
- WP5: `test_audio_link_gating` (initiator `post` and responder `_Link.post` drop `audio_*` without the cap;
  caps ≤ 16; Linux list without `audio`), existing suites green, `tools/older_peer_check.py`.

### 11.2 Engine (`native/audio/tests/engine_tests.cpp`, one binary, tiny assert macros, no framework)

Mac `native/audio/engine/build-tests.sh` (also `--asan`, `--tsan`), PC `build-tests.cmd`. Cases: AEAD
(RFC 8439 vector, PyNaCl vectors, tamper/replay/out-of-window/wrong key), packet codec, Opus round trip
(mono VOIP FEC, stereo AUDIO; FEC recovers single loss), resampler SNR ≥ 80 dB at 1 kHz for ±500 ppm,
JB on synthetic traces (2 % Bernoulli loss, Gilbert bursts, 0-20 ms jitter, a 0.7 s stall, ±100 ppm drift,
10 min simulated in < 10 s): level within target ±10 ms after 30 s, no unbounded growth, flush after stall,
"decoded and discarded" invariant; RR timing; flow state machine (probe/up/link/back) over loopback UDP
with packet loss injected; control protocol golden lines (§3.4); SPSC ring under TSan; AEC (§7): synthetic
echo (-11 dB, 44 ms + room IR, noise) ERLE ≥ 20 dB after 5 s, near-end-only speech attenuated < 1 dB.

### 11.3 Null backend

`make_null_backend("mic=sine:440,ppm=+80;out=record:<path>")`: virtual devices on threads with a given
clock error, records what it renders, exposes levels and a settable `vmic_in_use`. `beamer-audio --backend
null:<opts>` (built by `build-tests.*` as `beamer-audio-null`) lets Python and E2E tests run the whole stack
on one machine, including two helpers streaming to each other over 127.0.0.1.

### 11.4 Two-machine E2E (`dev/audio/e2e/`, reuses `proto/scenarios/sig.py` and `analyze.py`)

`dev/deploy.sh` both, then `uv run --python 3.13 --with numpy dev/audio/e2e/run.py <case>`: enables
dev mode on both machines (creates `audio-dev.enabled`, restarts Beamer through `open -a` / the scheduled
task), drives routes by writing `audio-dev-cmd.json` (PC over `ssh lebig`), injects signature A/B with the
helper's dev test source, records sinks with the prototype tools (`proto/mac` `capture-mic --device "Beamer
Microphone"`, `proto/win` `capture-mic --device "CABLE Output"`, loopback/tap for sound), and analyses
latency/glitches as in Phase 1. A PC `test_source` is written first and the harness waits (≤ 15 s, else
a clear error) until the PC's `audio-dev-status.json` names it before any route is set. Only `both` needs
the PC's DualSense; without it that case alone stops with a message. Cases: `mic_mac_pc`, `mic_pc_mac`, `sound_mac_pc`, `sound_pc_mac`,
`both`, `persist` (route survives a restart of both apps, re-applied ≤ 5 s after link up), `linkloss`
(kill Beamer on the target: source sound audible again ≤ 1.5 s; Windows journal restore), `fallback`
(block UDP with the helper's dev `drop_udp` switch: stream continues via the link ≤ 2.5 s),
`oldpeer` (PC<->asusrog link unaffected: `tools/older_peer_check.py` + log). Pass marks: latency within
+10 ms of Phase 1's rows (scenario table in `PHASE1.md`), ≤ 3 glitches/min over 10 min on Wi-Fi, 0
feedback loops (signature seen once).

## 12. Risks and open points

1. **Windows firewall prompt on an explicit UDP bind** in the user session (Phase 1 ran in session 0 where
   no dialog can show). WP4 checks it first; if it prompts, use port 0 (no protocol change).
2. **TCC for a child helper under the hardened runtime**: attribution to Beamer.app is the documented
   behaviour, but whether the entitlement is checked on the helper or the app is not; both carry it. Needs
   Alex for the first prompts (H1).
3. **On-demand detection on VB-CABLE** (capture sessions) and on the driver (`IsRunningSomewhere` per
   device) are unverified; fallback = demand always true (privacy indicator stays on while a mic is active).
4. **AEC quality** with MacBook speaker non-linearity; speexdsp may leave residual echo. Escape hatch:
   `EchoCanceller` interface for AEC3; the headset warning stays.
5. **Device tap semantics** for the AEC reference (does a device tap see processes muted by another tap?).
   WP9 measures; the reference is only needed when sound actually leaves the speakers.
6. **VB-CABLE adds ~100 ms** on the PC; WP4 stretch: exclusive-mode render into CABLE Input (~55 ms).
7. Smaller: Opus without SIMD (~1-2 % of a core per stream, measure); x86_64 slices built but never run
   here (no Rosetta); a driver install/update restarts coreaudiod (all audio drops); Wi-Fi stalls stay the
   main glitch source.
8. Linux: protocol-ready (`udp.listen`, open-ended `virtual_mic.kind`, platform-neutral messages); needs a
   PipeWire backend and a virtual source later.

Human checks (Alex, batched): H1 first Mic + System Audio Recording prompts on the Mac; H2 ear check of PC
endpoint mute (G733) and real USB/Bluetooth hot-plug on both; H3 a real call (Zoom/Discord) with the Mac mic
on the PC and the PC sound on the Mac speakers, AEC on.

## 13. Work packages

Each WP works in its own worktree branch from `feat/audio`, commits with `git commit -s`, and is merged by
the operator. File ownership is exclusive except the named hunks of §2.2. PC agents (`ssh lebig 'claude
-p ...'`) write their report to `~\briefs\<wp>.report.md` (ssh timeouts kill stdout, not the agent).

**Wave 1 (start now, in parallel): WP1, WP2, WP3, WP4, WP5, WP8. Wave 2: WP6, WP7 (start once WP1 has
committed `model.py`, its first milestone). Wave 3: WP9, WP10.**

### WP1 Python control plane (Mac agent)
- Goal: `core/audio` minus `view.py`: wire, model, planner, store, helper client, service, fake helper,
  cluster harness, dev command files.
- Owns: `core/audio/{__init__,wire,model,planner,store,helper,service,fake_helper}.py`,
  `core/tests/test_audio_{wire,model,planner,service,cluster,helper,helper_native}.py`,
  `core/tests/audio_cluster_harness.py`.
- Inputs: §3 (client side), §4.2/§4.5 (flow/fallback logic), §5, §6, §8. Milestone 1 (first commit):
  `model.py` exactly as §8.3, so WP6/WP7 can start.
  Depends on: nothing (fake helper). `test_audio_helper_native` waits for WP2's null helper.
- Acceptance: all `test_audio_*` above pass on the Mac and on the PC; cluster harness: 3 machines incl. one
  without `audio` converge in < 50 simulated message rounds for 200 random edit/partition sequences
  (seeded); no `audio_*` ever posted to a peer without the cap; service never blocks a link thread (test with
  a host whose `post` asserts the calling thread is the worker).

### WP2 Portable media engine (Mac agent; builds and runs its tests on the PC over ssh)
- Goal: `native/audio/engine` + `third_party` + `tests`: crypto, packet, codec, JB, resampler, drift, levels,
  SPSC, flow (UDP, probe, RR, ping, replay, link mode), streams (send/recv/loop), control loop `run_helper`,
  null backend, `backend.h` exactly as §2.4, `build-tests.sh`/`.cmd`, `beamer-audio-null`.
- Owns: `native/audio/engine/**`, `native/audio/third_party/{opus-1.6.1,monocypher-4.0.2,nlohmann-json-3.11.3}/**`,
  `native/audio/third_party/README.md`, `native/audio/tests/**`, `native/audio/README.md`, `.gitignore` entry
  for `native/audio/build/`.
- Inputs: §2.3, §2.4, §3, §4, §11.2, §11.3; port from `proto/transport/media.py` and `peer.py`.
  Depends on: nothing. Commit `backend.h` first (unchanged from §2.4) so WP3/WP4 compile against it.
- Acceptance: `engine_tests` pass on Mac (plain, ASan/UBSan, TSan) and PC; two `beamer-audio-null`
  processes driven by a small Python script over 127.0.0.1 stream mic + system both ways for 120 s with 2 %
  loss and +80 ppm: JB bounded (level p99 < target + 15 ms), 0 decode errors, CPU < 3 % of a core per
  helper; flow falls back to `link` within 2.2 s of UDP drop and returns within 1.5 s of UDP restore;
  control protocol answers every command in §3.2 (golden test).

### WP3 Mac backend + helper build (Mac agent)
- Goal: `Backend` for macOS (devices + listeners, mic capture, global tap excluding pids with
  `mutedWhenTapped`, reference tap, render to a device or to the hidden feed by UID, `IsRunningSomewhere`
  demand, forms, permission status, coreaudiod-restart and sleep/wake recovery, blocklist), `main.mm`,
  `build-mac.sh` (universal helper + driver build/stage), and the `git mv` of the driver to
  `native/audio/mac-driver/`.
- Owns: `native/audio/mac/**`, `native/audio/build-mac.sh`, `native/audio/mac-driver/**`
  (and the removal of `dev/audio/proto/mac-driver`).
- Inputs: §2.3, §2.4, §3.3 features, §6.3, §7 (forms, reference), `proto/mac` (behaviour reference),
  `proto/mac-driver/README.md`.
  Depends on: WP2's `backend.h` (start with a local `backend_check.mm` harness until the engine lands).
- Acceptance (on this Mac): `beamer-audio list` shows built-in mic/speakers with forms, not the feed, not
  "Beamer Microphone" as a source; `send` of the built-in mic to a null peer runs with < 1 % CPU; system
  capture with mute: `proto/mac/experiments/mute_test.sh`-style check (tap level = source, mic hears noise
  floor) and unmute ≤ 1 s after SIGKILL; exclusion: a tone played by the helper itself is not in its tap
  (≤ -90 dBFS); render into the feed appears on "Beamer Microphone" (`live_check.py` pilot passes);
  `virtual_mic.in_use` flips true ≤ 200 ms after a recorder opens "Beamer Microphone" and false 3 s after
  it closes, and is not set by Beamer's own feed; after `beamer-hal restart` the feed stream recovers ≤ 3 s;
  `lipo -archs` = `x86_64 arm64`; `build-mac.sh` with Command Line Tools only.

### WP4 Windows backend + helper build (PC agent)
- Goal: `Backend` for Windows (MMDevice inventory + notifications with de-dup, mic capture, process
  loopback EXCLUDE tree of `--exclude-pid`, endpoint mute of the default render endpoint with journal and
  restore (mute, master and channel volumes), keep-alive, render to an endpoint or to CABLE Input,
  capture-session demand on CABLE Output, forms, reference loopback, device invalidation recovery,
  MMCSS), `main.cpp`, `build-win.cmd`. First task: list every endpoint with driver/enumerator and write
  `win/blocklist.inc` (Elgato/Wave Link virtual endpoints), and test whether an explicit UDP bind of 24823
  by an unknown exe started from the scheduled task (user session) shows a firewall prompt (risk 1).
- Owns: `native/audio/win/**`, `native/audio/build-win.cmd`.
- Inputs: §2.3, §2.4, §3, §6.3, §7, `proto/win` (behaviour reference and results).
  Depends on: WP2's `backend.h` (local harness until the engine lands).
- Acceptance (on lebig, user session via the scheduled task where it matters): `list` shows Wave:3,
  G733, DualSense; no Elgato virtual endpoint, no CABLE Output/16 Ch; journal: kill the helper while muting
  -> next start restores exactly (state compared before/after for all active endpoints); loopback with the
  endpoint muted keeps -13.98 dBFS in = out; a tone rendered by the helper is absent from its loopback
  (≤ -90 dBFS); demand true ≤ 300 ms after a recorder opens CABLE Output; `beamer-audio.exe` is a static x64
  exe; firewall result recorded in the report. Stretch: exclusive-mode render into CABLE Input, measured
  with `proto/transport/devloop.py`.

### WP5 Link integration on both apps (Mac agent; PC verified over ssh)
- Goal: the `audio` cap, gating, dispatch, adapters, service lifecycle; WIRE.md section.
- Owns: the §2.2 hunks in `core/link.py`, `core/receiver.py`, `mac_app/bridge.py`, `win_app/sender.py`,
  `win_app/kvm_bridge_win.py` (caps + service wiring only), `core/feature_flags.py`, `WIRE.md`,
  `mac_app/setup.py` (`"audio_mac"` first in `py_modules`, `includes`); new `mac_app/audio_mac.py`,
  `win_app/audio_win.py`, `core/tests/test_audio_link_gating.py`.
- Inputs: §2.2, §5.1, §8.1 (`AudioHost`, service methods), `notes/beamer-link.md` §2/§4/§8.
  Depends on: WP1's service API (§8.1; code against it, land after WP1).
- Acceptance: `test_audio_link_gating` + full `core/tests`, `mac_app/tests` and (PC) `win_app/tests`
  unchanged except known failures (dev/README); `dev/deploy.sh` both: both logs show `audio_hello` and
  `audio_devices` exchanged (helper = `beamer-audio-null` set in `audio-dev.enabled` if WP3/WP4 are not in); the
  PC<->asusrog link stays up and asusrog logs no new warning; Mac hello carries 14 caps.

### WP8 Packaging and permissions (Mac agent; PC over ssh)
- Goal: §10: helper and driver in the app, signing and entitlements, plist strings, TCC reset list, driver
  install flow, Windows build/install/installer/portable, `restore` at install.
- Owns: `mac_app/build_dmg.sh`, `mac_app/entitlements.plist`, new `mac_app/entitlements-audio.plist`,
  `mac_app/setup.py` (plist keys, `"audio_driver"` last in `py_modules`), `mac_app/Install-Beamer-Mac.command`, new `mac_app/audio_driver.py`,
  `native/audio/mac-driver/install-driver.sh`, `win_app/build_win_app.ps1`, `win_app/Install-Beamer.ps1`,
  `win_app/Beamer-Setup.iss`, `win_app/build_portable.spec`.
- Inputs: §10, §2.1 build outputs.
  Depends on: build-script paths (fixed here); skips the native step with a warning while `build-mac.sh` /
  `build-win.cmd` do not exist yet, so it can land in wave 1.
- Acceptance: `dev/deploy.sh` both succeeds; `codesign --verify --strict --deep /Applications/Beamer.app`;
  `codesign -d --entitlements - .../MacOS/beamer-audio` shows audio-input; `plutil -p Info.plist` shows both
  usage strings; driver bundle inside verifies; `%LOCALAPPDATA%\Beamer\beamer-audio.exe` present and
  signed when a signtool is available; Beamer's log on both shows the helper `ready`; `audio_driver.py`
  unit-tested for version compare and the chosen install path (beamer-hal vs osascript) with mocks.

### WP6 Page model + Mac Audio page (Mac agent)
- Goal: `core/audio/view.py` (§9.1) and the AppKit page (§9.2) incl. `AudioMap` and `LevelMeter`.
- Owns: `core/audio/view.py`, `core/tests/test_audio_view.py`, `core/tests/audio_fixtures.py`,
  `mac_app/audio_page.py`, `mac_app/pages.py` (PAGES entry), `mac_app/kvm_bridge_app.py` (builders map +
  `_audio_page`), `mac_app/setup.py` (`"audio_page"` before `"pages"`), `mac_app/tests/test_audio_page.py`,
  `locales/beamer.pot` (regenerated with `tools/i18n_extract.py`).
- Inputs: §7 warnings, §8.3, §9.
  Depends on: WP1 milestone 1 (`model.py`); runs on fixture snapshots until WP1/WP5 land. Milestone 1 (first
  commit): `view.py` + `audio_fixtures.py` + `test_audio_view.py`, so WP7 can start.
- Acceptance: `test_audio_view` and `test_i18n_coverage` pass; an offscreen render of the page for 4 fixture
  states (no peers, mic streaming to PC, sound redirected + echo warning, driver missing) in light and dark
  (as `mac_app/tests/design_offscreen.py` does) looks like the Crossing page (same tile style), reviewed by
  the operator; selector changes call the service setters (fake service test); no call blocks the main
  thread (> 16 ms) in a 60 s meter run at 20 Hz.

### WP7 Qt Audio page (PC agent)
- Goal: §9.2 for `win_app` (Windows and Linux builds), using `core/audio/view.py`.
- Owns: `win_app/audio_page_win.py`, `win_app/pages_win.py` (PAGES entry), `win_app/kvm_bridge_win.py`
  (builders map + `_audio_page` only), `win_app/tests/test_audio_page_win.py`.
- Inputs: §9; style from `win_app/diagram.py` (`AtlasCanvas`), `win_app/widgets.py`, `win_app/theme.py`.
  Depends on: WP1 milestone 1 and WP6 milestone 1 (`view.py` + fixtures; WP7 never edits them, it asks WP6).
- Acceptance: offscreen renders (`QT_QPA_PLATFORM=offscreen`) of the same 4 fixture states, light/dark;
  `win_app` test suite unchanged otherwise; the page opens with Ctrl+<n> and keyboard navigation reaches
  every control.

### WP9 Echo cancellation (Mac agent; Windows part on the PC after WP4)
- Goal: §7: `EchoCanceller` (speexdsp MDF + RES), reference capture alignment and drift, wiring of `aec`
  in `send`/`loop`, `CaptureKind::Reference` in both backends.
- Owns: `native/audio/engine/aec.{h,cpp}`, `native/audio/third_party/speexdsp-1.2.1/**`; hunks in
  `native/audio/mac/tap_mac.mm` and `native/audio/win/loopback_win.cpp` agreed with WP3/WP4 (after they merge).
- Inputs: §7, §11.2 AEC cases.
  Depends on: WP2, WP3, WP4 merged.
- Acceptance: engine AEC tests (§11.2); live on the Mac: PC sound sent to the Mac speakers at normal volume
  while the Mac mic feeds the PC: echo level at CABLE Output ≥ 15 dB lower with AEC than without, near-end
  speech level change < 2 dB; CPU of the AEC < 3 % of a core.

### WP10 Two-machine E2E harness and soak (Mac agent with ssh)
- Goal: §11.4.
- Owns: `dev/audio/e2e/**` (may import `dev/audio/proto/scenarios/sig.py` and `analyze.py`; never edits them).
- Inputs: §8.1 dev command files, §11.4, Phase 1 scenario tables.
  Depends on: WP1-WP5 and WP8 merged (WP9 for the AEC case).
- Acceptance: every case in §11.4 runs unattended from the Mac and writes `runs/<case>/result.json`;
  results table appended to `dev/audio/STATUS.md` by the operator; pass marks of §11.4 met or each miss
  explained with its trace.

### Dependency summary

| WP | Machine | Wave | Needs before it can finish |
|---|---|---|---|
| WP1 Python control plane | Mac | 1 | - |
| WP2 Media engine | Mac (+PC build) | 1 | - |
| WP3 Mac backend | Mac | 1 | WP2 `backend.h` (first commit); WP2 for the full helper |
| WP4 Windows backend | PC | 1 | WP2 `backend.h`; WP2 for the full helper |
| WP5 Link integration | Mac (+PC) | 1 | WP1 |
| WP8 Packaging | Mac (+PC) | 1 | WP3/WP4 build scripts for the full result |
| WP6 Page model + Mac page | Mac | 2 | WP1 milestone 1 (`model.py`); WP5 for live use |
| WP7 Qt page | PC | 2 | WP1 milestone 1, WP6 milestone 1 (`view.py`) |
| WP9 AEC | Mac (+PC) | 3 | WP2, WP3, WP4 |
| WP10 E2E + soak | Mac (+ssh) | 3 | WP1-WP5, WP8 (WP9 for the AEC case) |
