# Camera across Beamer: Phase 2 design (integration into the app)

Date 2026-11-11. Branch `feat/camera` (from `feat/audio`). Inputs: `PHASE1.md` (verdicts), the operator
decisions in `STATUS.md` and in D1's brief, `reports/c1.md`, `c2.md`, `c3.md`, the prototypes in `proto/`, and
the audio system this extends: `dev/audio/PHASE2.md` (cited as **A§n**) and its code (`native/audio`,
`core/audio`, the Audio pages). C4 (end-to-end prototype) runs in parallel: every number it owns is marked
**[C4]** and will be amended when `reports/c4.md` lands. This document is the contract for the implementation
agents; section numbers (§) are referenced by the work packages in §12.

## 0. Decisions at a glance

| # | Question | Decision |
|---|---|---|
| 1 | Process | **No new helper.** Video is a second backend of the existing `beamer-audio` helper (name kept, §2.3): Mac capture + VideoToolbox in ObjC++ (`native/audio/mac/`), Windows MF decode + virtual-camera feed in C++ (`native/audio/win/`), the transport in the portable engine (`native/audio/engine/video_*`). A new interface `video_backend.h` sits beside the unchanged `backend.h` (§2.4). |
| 2 | Windows camera | Own media source DLL `BeamerCameraSource.dll` (`native/audio/vcam-win/`), loaded by Frame Server, **owns the shared memory**; the helper creates the camera **System lifetime, CurrentUser access**, unelevated, at every start (idempotent). Decode stays in the helper (MF H.264, D3D11, `AVLowLatencyMode`, software fallback). Shared-memory protocol §4.6. |
| 3 | Media plane | Stream kind `video` on the **existing UDP flow**, packet v1 header unchanged, codec id 2 (H.264), 12 B video header, <= 1200 B UDP payload; new ptypes `nack` (7), `pli` (8), `sr` (9); 24 B video receiver report; replay window 1024 for video (§4.1-4.2). |
| 4 | Recovery | NACK (<= 3 tries, holes <= 64 packets, within the frame deadline) + **LTR refresh with real acks**; IDR only at stream start, size change or last resort, **always paced (3x bitrate)** and at most one per 2 s outside start; no FEC, no periodic intra refresh, no ABR (safety step-down only) (§4.3-4.5, §4.10). |
| 5 | Playout | Adaptive deadline p99(10 s) + 8 ms in [50, 150] ms; late frames decoded, never shown; hold the last good frame across loss; placeholder after 250 ms with nothing feeding the camera (§4.5, §4.6.4). |
| 6 | Wire | **No new cap** (still 14 of 16): camera messages ride cap `audio`, announced by the feature word `camera1` in `audio_hello`. New types `camera_devices`, `camera_route` (flooded), `camera_open`, `camera_opened`, `camera_close` (direct). Fallback packets stay in `audio_frame` (§5). |
| 7 | Routing | Cluster-wide **active camera** (machine + camera, or off) and a cluster-wide **quality** (720p / 1080p), like the active mic. It feeds the "Beamer Camera" of every machine that has one, **only while an app there has it open** (on demand). Model allows any source -> any sink; implemented: Mac source -> Windows sink (§6). |
| 8 | Python | New package `core/camera/` (wire, model, planner, store, part, view). `CameraPart` runs **inside the AudioService worker** and uses its helper and flows through a narrow `MediaContext` (§7). |
| 9 | UI | The **Audio page becomes "Audio & Camera"** (same key, place and shortcut) with a **Camera** module under the Microphone module, a camera badge and arrow on the shared machine map, and a Mac menu-bar line while a remote app uses the camera (§8). |
| 10 | Install / permissions | PC: one elevated step offered from the page ("Install Beamer Camera", UAC prompt) runs `beamer-audio.exe vcam-install`: DLL to `%ProgramFiles%\Beamer\Camera\<dll version>\`, HKLM COM class + `HKLM\SOFTWARE\Beamer\Camera`. Mac: `NSCameraUsageDescription`, `com.apple.security.device.camera` on app and helper, prompt asked **when the Mac's camera is chosen as the active camera** (§9). |
| 11 | Lip sync | Every stream (audio too) sends `sr` (capture-time mapping) from day one. Policy: on a PC receiving both the Mac mic and the Mac camera, video is held back to the mic's presentation delay, within 0..150 ms; audio is never delayed for video (§4.9). Implemented by CW10. |
| 12 | Tests | Python `unittest` with the fake helper grown a video side; engine C++ tests; a **null video backend** (fake encoder + reference-checking fake decoder) so two null helpers exercise the whole video protocol on one machine; OS acceptance per backend; a two-machine E2E harness `dev/camera/e2e/` built on `dev/audio/e2e` (§10). |

### Deviations from the operator decisions (with reasons)

1. **Placeholder vs network stalls (clarification).** "Placeholder after 250 ms of no frames" is applied to
   *nothing feeding the camera*: no stream, stream starting, helper gone or stuck. During a Wi-Fi stall of a
   running stream the helper keeps the camera fed with the **last good frame for up to 2 s** (C3: "hold the
   last frame, not black, during the stall"; stalls of 0.5-1.5 s are normal here), then switches to the
   placeholder "Reconnecting to <Mac>". Reason: a placeholder flashing in a call at every 0.3-1.5 s Wi-Fi
   stall is worse than a short freeze. §4.6.4.
2. **TCP fallback keeps the size, not "720p15"**: in link state the encoder drops to 1 Mbit/s and 15 fps at
   the current size (no size change, so no IDR over the slow path; LTR continuity kept). C3's "720p15 or
   360p30" both need an IDR (~80-120 KB) at 1 Mbit/s, i.e. ~1 s of link time. §4.10.
3. **Video send starts before `camera_opened`** (unlike audio, A§6.4 rule 4, where `send` waits for
   `audio_opened`): the camera warms up while the open round trip runs, and no media leaves the Mac before
   the receiver's start PLI, so nothing is lost to an unknown stream id. Reason: on-demand start latency.
   §4.7.
4. **Helper name kept** `beamer-audio` (C1 offered `beamer-media`): the PC's firewall allowance is for
   `beamer-audio.exe` (`dev/audio/TEST-ALEX.md` §1), and the name is in packaging, tests and Alex's notes.

No other deviation: everything else in the brief is binding and followed.

## 1. Terms

- **Audio terms** as A§1 (machine id, direct peer, helper, engine, flow, stream, device key).
- **Source machine** S: owns the physical camera (Mac). **Sink machine** T: has a virtual camera (Windows).
- **Beamer Camera**: the Windows virtual camera (`MFCreateVirtualCamera`, friendly name "Beamer Camera"; Windows
  shows "Beamer Camera (Caméra virtuelle Windows)" in French, its suffix). **Source DLL**: `BeamerCameraSource.dll`,
  the media source Frame Server loads. **vcam**: the helper's side of it (create/start, shared memory, demand).
- **Camera key**: AVFoundation `uniqueID`. **CameraRef** = `{key, name}`. "Default" = macOS
  `AVCaptureDevice.systemPreferredCamera`, followed.
- **AU**: one H.264 access unit (Annex-B, SPS/PPS before every IDR). **Frame number** `frame`: u32 set by the
  engine, +1 per AU sent on a stream. **LTR**: long-term reference (VideoToolbox `EnableLTR`).
  **Refresh**: an AU that references only an acknowledged LTR (`ForceLTRRefresh`).
- **Clean**: a frame whose whole reference chain the receiver has decoded from complete data. **Broken**:
  the receiver lost a frame and nothing clean has arrived since.
- **Demand**: an app on T has Beamer Camera open (the source DLL is streaming).
- **Playout deadline D**: time from capture to display on T (§4.5). **Hold**: re-showing the last good frame.

## 2. Code layout, ownership, native build

### 2.1 Tree (new files)

```
native/audio/engine/video_backend.h   VideoBackend contract (§2.4), verbatim; CW2 commits it first
native/audio/engine/video_packet.{h,cpp}  video header, nack/pli/sr payloads, video RR (§4.1-4.2)
native/audio/engine/video_send.{h,cpp}    VideoSendStream: fragment, seal, history, NACK/PLI, acks, rate (§4.3)
native/audio/engine/video_recv.{h,cpp}    VideoRecvStream: reassembly, loss, NACK, PLI, RR, playout, presenter (§4.5)
native/audio/engine/pacer.{h,cpp}         per-flow video token bucket (§4.4)
native/audio/engine/sync.{h,cpp}          sr bookkeeping, clock offset, lip-sync hold (§4.9; hold policy CW10)
native/audio/engine/null_video.{h,cpp}    null video backend: fake encoder + reference-checking sink (§10.3)
native/audio/tests/video_tests.cpp        engine video tests (§10.2), built into engine_tests
native/audio/tests/two_helpers_video.py   two null helpers over 127.0.0.1 with loss (CW2 acceptance)

native/audio/mac/video_backend_mac.{h,mm} VideoBackend for macOS
native/audio/mac/camera_mac.mm            AVFoundation discovery, preferred camera, permission, capture session
native/audio/mac/videoenc_mac.mm          VTCompressionSession low latency, LTR token map, refresh/IDR, rate
native/audio/mac/synth_mac.mm             dev synthetic source (C1's synth.mm + code strip), --dev only

native/audio/win/video_backend_win.{h,cpp} VideoBackend for Windows
native/audio/win/videodec_win.cpp         MF H.264 decoder: D3D11 + staging readback, software fallback
native/audio/win/vcam_win.cpp             camera create/start, install state, shared memory, demand, heartbeat
native/audio/win/placeholder_win.cpp      DirectWrite/D2D placeholder renderer -> NV12 (§4.6.4)
native/audio/win/app_usage_win.cpp        which app uses the camera (CapabilityAccessManager), best effort
native/audio/win/vcam_install_win.cpp     elevated verbs vcam-install / vcam-uninstall (§9.2)

native/audio/vcam-win/                    the source DLL (like mac-driver/: its own binary)
  shm_protocol.h                          shared-memory layout, names, CLSID (§4.6.2), verbatim; CW5 commits it first
  source.cpp, source.def, placeholder_builtin.inc (baked "waiting for Beamer" image), build.ps1, README.md
  tools/camprobe.cpp, tools/feeder.cpp    ports of proto/win camprobe (+ C1 code-strip reader) and feeder

core/camera/__init__.py                   exports CameraPart, model types
core/camera/wire.py                       camera_* builders, validators, limits (pure)
core/camera/model.py                      dataclasses (§7.2), merges, resolve (pure)
core/camera/planner.py                    plan_camera(...) (pure, §6.4)
core/camera/store.py                      camera.json (atomic)
core/camera/part.py                       CameraPart + MediaContext protocol (§7.1)
core/camera/fake_video.py                 video side of the fake helper (cameras, vcam, video streams)
core/camera/view.py                       pure Camera module model + map extras (§8.2)
core/tests/test_camera_*.py, core/tests/camera_fixtures.py

mac_app/camera_section.py                 AppKit Camera module
win_app/camera_section_win.py             Qt Camera module
win_app/vcam_install.py                   install state, UAC launch of vcam-install/-uninstall (§9.2)
dev/camera/e2e/                           two-machine harness (§10.5)
```

Every new source file carries `SPDX-License-Identifier: GPL-3.0-only`. No new third-party code: AVFoundation,
VideoToolbox, Media Foundation, D3D11, Direct2D/DirectWrite and WIC are OS frameworks.

### 2.2 Hunks in existing files (the only shared surfaces)

| File | Hunk | Owner |
|---|---|---|
| `native/audio/engine/packet.{h,cpp}` | `PType` += `kNack=7, kPli=8, kSr=9`; `PCodec` += `kCodecH264=2`; `ReplayWindow` gets a size template (`ReplayWindowN<128>` = today's, `<1024>` for video; `ReplayWindow` stays an alias) | CW2 |
| `native/audio/engine/flow.{h,cpp}` | dispatch `kNack`/`kPli` to send streams and `kSr` to recv streams (same lookup as `kMedia`/`kRR`); `attach_send/recv` accept video streams (a common `FlowStream` base or two more maps); video RR cadence (§4.2); flow-owned `Pacer`; `link_out` emits video packets in their own event with `"video":true` | CW2 |
| `native/audio/engine/streams.{h,cpp}` | audio `SendStream` sends `sr` every 1 s; audio `RecvStream` publishes the capture ts of the sample being rendered + render latency (atomics) for §4.9 | CW2 |
| `native/audio/engine/helper.{h,cpp}` | `run_helper(Backend&, VideoBackend*, argc, argv)` overload (the old signature forwards with `nullptr`); commands/events of §3; `ready.features` += video words | CW2 |
| `native/audio/engine/sources.txt` | the new engine `.cpp` files | CW2 |
| `native/audio/mac/main.mm` | build the video backend, call the new `run_helper` | CW3 |
| `native/audio/build-mac.sh` | `MAC_BACKEND` += the new `.mm`; frameworks += `CoreMedia CoreVideo VideoToolbox` | CW3 |
| `native/audio/win/main.cpp` | `vcam-install` / `vcam-uninstall` verbs before `run_helper`; build the video backend | CW4 |
| `native/audio/win/build.ps1` | `$backendSrc` += the new `.cpp`; libs += `mfplat mf mfuuid mfreadwrite d3d11 dxgi d2d1 dwrite windowscodecs advapi32 shlwapi`; call `vcam-win\build.ps1` | CW4 (DLL build script itself: CW5) |
| `core/audio/service.py` | construct `CameraPart` when the flag is on; route helper events `cameras`, `vcam`, `camera`, `permission.camera`, `stream_state`/`stats` of video stream ids, `link_out` with `video`; route `camera_*` link messages, link up/down, helper ready/restart to the part; call `part.apply()` after the audio plan; implement `MediaContext` (§7.1); `audio_hello.features` += `camera1` | CW6 |
| `core/audio/fake_helper.py` | delegate commands it does not know to `core.camera.fake_video` when present | CW1 |
| `core/tests/audio_cluster_harness.py` | optional camera parts per machine | CW1 |
| `core/audio/view.py` | `page_model(..., camera=None)`: when given a `CameraMapExtras` (§8.2), tiles carry `camera_line`/`has_active_camera`/`watching_line` and `arrows` get `kind="camera"` | CW7 |
| `core/link.py` | `_CAP_FOR` += every `camera_*` type -> `"audio"` | CW6 |
| `core/receiver.py` | `_is_audio`/dispatch: `camera_*` handled like `audio_*` (same callback) | CW6 |
| `mac_app/bridge.py`, `win_app/sender.py`, `win_app/kvm_bridge_win.py` | forward `camera_*` like `audio_*` (message elif only; caps unchanged) | CW6 |
| `core/feature_flags.py` | `CAMERA = True` (fork) | CW6 |
| `WIRE.md` | section "Camera (inside capability `audio`, fork)" = §5 | CW6 |
| `mac_app/pages.py`, `win_app/pages_win.py` | title "Audio & Camera" and purpose when `CAMERA` (key `audio` unchanged) | CW7 / CW8 |
| `mac_app/audio_page.py` | insert the Camera module after the notices strip; map draws camera badge/arrow | CW7 |
| `win_app/audio_page_win.py` | same for Qt | CW8 |
| `mac_app/kvm_bridge_app.py` | menu-bar line + "Stop sharing the camera" item (§8.5) | CW7 |
| `mac_app/setup.py` | `py_modules`: CW7 inserts `"camera_section",` immediately **before** `"audio_page",`; `includes` += `core.camera.*` (CW6); plist keys (CW9) | CW6/CW7/CW9 |
| `mac_app/entitlements.plist`, `entitlements-audio.plist`, `build_dmg.sh`, `Install-Beamer-Mac.command` | camera entitlement, TCC reset list | CW9 |
| `win_app/build_win_app.ps1`, `Install-Beamer.ps1`, `Beamer-Setup.iss`, `build_portable.spec` | ship `camera\BeamerCameraSource.dll` beside the helper | CW9 |

### 2.3 Why inside `beamer-audio`, and the one-backend split

- The video stream must share the flow's socket, keys, probing, hole punch, receiver reports and link
  fallback (C3 Q1), and audio must be scheduled ahead of video on that socket (C3 Q2): both only work in the
  process that owns the flow. A second process would need its own flow (C3: no gain, double machinery).
- TCC attribution is the same either way (Beamer.app is responsible for any child, C1 Q6); encoder crashes
  are already isolated in Apple's `VTEncoderXPCService`; on Windows a decoder crash kills only the helper
  (the source DLL then shows its placeholder, Beamer restarts the helper, A§8.2 watchdog).
- **Engine/backends split**: the engine owns everything that is policy and must be testable without a camera
  (packetization, loss tracking, NACK/PLI/ack logic, pacing, playout timing, rate ladder, lip sync). Backends
  own OS objects only: the Mac backend turns a `CameraSpec` into encoded AUs and obeys `request_ltr_refresh()`
  / `request_idr()` / `acknowledge()` / `set_bitrate()`; the Windows backend decodes the AUs it is given and
  shows the frame the engine says to show. The null video backend implements both sides in the engine
  (§10.3).
- The helper stays one binary per OS; `backend.h` (A§2.4) is not touched.

### 2.4 Video backend interface (`native/audio/engine/video_backend.h`, verbatim contract for CW2/CW3/CW4)

```cpp
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace beamer::audio {
struct VideoFormat { int w = 0, h = 0, fps = 30; };
struct CameraInfo {
  std::string key, name, transport;          // transport: "builtin","usb","continuity","virtual","other"
  std::vector<VideoFormat> formats;          // only 1280x720 and 1920x1080 at their max fps <= 30 are listed
  bool preferred = false;                    // macOS systemPreferredCamera
};
struct CameraSnapshot { std::vector<CameraInfo> cameras; std::string preferred;  // key, "" = none
                        std::string permission; };  // granted|denied|undetermined|restricted|unknown
struct EncodedFrame {                         // one Annex-B AU; valid only during on_frame
  const uint8_t* data = nullptr; size_t len = 0;
  uint64_t capture_ns = 0;                    // capture time on now_ns()'s clock (engine util.h)
  bool idr = false, refresh = false, ltr = false;  // refresh: references only an acknowledged LTR
  int w = 0, h = 0;
};
struct EncoderSink {                          // called on the backend's encoder output thread
  virtual void on_frame(const EncodedFrame&) = 0;   // copy and return; never block > 1 ms
  virtual void on_camera_event(const std::string& what, const std::string& detail) = 0;
      // what: "running","interrupted","resumed","lost","format","fps","effects","by_other"
  virtual ~EncoderSink() = default;
};
struct CameraSpec { std::string device;       // "" = system-preferred (followed)
                    VideoFormat fmt; int bitrate = 2500000; bool ltr = true;
                    std::string test;  };     // --dev only: "synth" = synthetic source, no camera
class CameraHandle {                          // one capture + one encoder session; all calls thread-safe
 public:
  virtual ~CameraHandle() = default;
  virtual void start_encoding() = 0;          // capture may run before; the first encoded AU is an IDR
  virtual void request_idr() = 0;
  virtual void request_ltr_refresh() = 0;     // falls back to an IDR inside when no LTR is acknowledged
  virtual void acknowledge(uint32_t engine_frame) = 0;   // see EncodedFrame numbering below
  virtual void set_bitrate(int bps) = 0;
  virtual void set_fps(int fps) = 0;          // 30 or 15 (15 = every other captured frame)
  virtual void set_format(const VideoFormat&) = 0;       // next AU is an IDR at the new size
  virtual std::string device_in_use() const = 0;
};
// The engine numbers AUs 0,1,2... in on_frame order per CameraHandle; acknowledge() uses that number.
struct VideoSinkListener {                    // called on the backend's thread
  virtual void on_decoded(uint32_t frame, bool ok, double decode_ms) = 0;
  virtual void on_sink_event(const std::string& what, const std::string& detail) = 0;  // "decoder_sw","device_lost"
  virtual ~VideoSinkListener() = default;
};
class VideoSinkHandle {                       // one decoder feeding the virtual camera
 public:
  virtual ~VideoSinkHandle() = default;
  virtual void decode(const uint8_t* au, size_t len, uint32_t frame, bool keep) = 0;
      // engine's video thread; keep = the picture may be shown (clean); !keep pictures are decoded and dropped
  virtual void present(uint32_t frame) = 0;   // make that kept picture the camera's current picture
  virtual void hold() = 0;                    // keep showing the current picture (stall), heartbeat continues
  virtual void drop(uint32_t frame) = 0;      // kept picture no longer needed
};
struct Placeholder { std::string state;       // "off","waiting","starting","denied","reconnecting","error"
                     std::string title, line; };
struct VcamState {
  std::string install;                        // ok|missing|outdated|broken|unsupported
  int dll_version = 0;                        // installed, 0 = none
  bool present = false, in_use = false, creating = false;
  std::string app;                            // best effort, "" = unknown
  VideoFormat want;                           // the app's media type while in_use
  std::string error;
};
struct VideoBackendListener {
  virtual void cameras_changed() = 0;         // debounced 300 ms
  virtual void vcam_changed() = 0;            // in_use on at once, off debounced 300 ms (Frame Server re-creates)
  virtual void permission_changed() = 0;
  virtual void camera_lost(CameraHandle*, const std::string& why) = 0;
  virtual ~VideoBackendListener() = default;
};
class VideoBackend {
 public:
  virtual ~VideoBackend() = default;
  virtual void set_listener(VideoBackendListener*) = 0;
  virtual std::vector<std::string> features() = 0;      // "camera_capture" (Mac), "virtual_camera" (Windows)
  virtual CameraSnapshot cameras() = 0;                 // empty on Windows
  virtual void request_permission() = 0;                // Mac: async prompt; listener on answer
  virtual std::unique_ptr<CameraHandle> open_camera(const CameraSpec&, EncoderSink*, std::string* err) = 0;
  virtual VcamState vcam() = 0;                         // Windows; Mac returns install "unsupported"
  virtual bool vcam_enable(bool on, std::string* err) = 0;   // create+Start the System/CurrentUser camera
  virtual void vcam_placeholder(const Placeholder&) = 0;
  virtual std::unique_ptr<VideoSinkHandle> open_sink(VideoSinkListener*, std::string* err) = 0;
};
std::unique_ptr<VideoBackend> make_null_video_backend(const std::string& options);
class Backend;
int run_helper(Backend& backend, VideoBackend* video, int argc, char** argv);   // video may be null
}
```

Error strings follow A§2.4 (`"<code>: message"`); codes for video: `no_camera`, `permission`, `busy`,
`not_installed`, `unsupported`, `decoder`, `internal`.

### 2.5 Build

- Mac: `build-mac.sh` unchanged in shape; the new `.mm` files are compiled per arch like the audio backend and
  linked with `-framework CoreMedia -framework CoreVideo -framework VideoToolbox` (AVFoundation is already
  linked). Deployment target stays 13.0 (`systemPreferredCamera` is 13.0+).
- Windows: `build.ps1` builds the helper as today plus `vcam-win\build.ps1` ->
  `native\audio\build\win\BeamerCameraSource.dll` (`cl /std:c++20 /O2 /MT /W4 /utf-8 /LD`, `source.def`,
  links `mfplat mfuuid ole32 advapi32 windowscodecs`), and the tools `camprobe.exe`, `feeder.exe` beside it.
- Engine tests: `build-tests.sh` / `.cmd` add `video_tests.cpp` and `null_video.cpp`; `beamer-audio-null` gets the
  null video backend by default (`--video null:<opts>`).

## 3. Helper control protocol additions (stdio, A§3)

### 3.1 Process

- Command line additions: `--video null:<options>` (any helper; replaces the OS video backend, §10.3),
  `--no-video` (no video backend: the helper behaves exactly as today). Windows verbs without `--control`:
  `beamer-audio vcam-install --dll <path> --owner-sid <SID> [--version N]`, `beamer-audio vcam-uninstall`,
  `beamer-audio vcam-status` (prints the `vcam` event once); exit 0 ok, 2 bad args, 3 not elevated, 4 failed.
- `ready.proto` stays 1 (only additions). `ready.features` gains `video1` (engine has §4), plus the backend's
  words: `camera_capture` (Mac), `virtual_camera` (Windows, independent of the install state).
- Threads added: per video send stream one **sender thread** (fragments, seals and pushes AUs through the
  pacer), per flow one **pacer thread** created with the first video send stream; per video recv stream one
  **video thread** (reassembly hand-off, decode calls, presenter timer). The network thread only parses,
  authenticates and enqueues (as for audio).

### 3.2 Commands (Beamer -> helper)

| cmd | fields | effect |
|---|---|---|
| `send` | `kind:"video"`, `stream`, `flow`, `key`, `source` `{"camera": key|null}` (null = system-preferred, followed); with `--dev` also `{"test":"synth"}`; `codec` (§4.3: `{"name":"h264","profile":"constrained_high","w":1280,"h":720,"fps":30,"bitrate":2500000,"mtu":1200,"ltr":true}`); `link` `{"bitrate":1000000,"fps":15}` (optional, the link-state settings) | opens the camera + encoder at once; **no media is sent before the receiver's first PLI** (`reason` start), whose answer is the stream's first IDR (§4.7); `stream_state starting` -> `running` at the first sent AU |
| `recv` | `kind:"video"`, `stream`, `flow`, `key`, `codec`, `sink` `{"virtual_camera": true}` | opens a sink, sends PLI `start` every 100 ms until a complete IDR arrives, then §4.5 |
| `video_config` | `stream` (a video send), any of `w`,`h`,`fps`,`bitrate` | live reconfiguration; a size change gives one paced IDR (the receiver needs no message: SPS carries the size) |
| `video_link` | `stream`, `paused` (bool) | Beamer's link outbox is full (§4.10): pause/resume encoding, capture keeps running |
| `cameras` | - | emit `cameras` now |
| `permission_request` | `what:"camera"` | Mac: `requestAccessForMediaType:AVMediaTypeVideo` (the prompt names Beamer); answered `ok` at once, the result comes as a `permission` event; elsewhere error `unsupported` |
| `vcam` | `enable` (bool, optional), `placeholder` `{state,title,line}` (optional) | Windows: create/Start the camera if installed (async; `vcam` event `creating` while it takes up to ~5 s the first time) and/or set the placeholder (§4.6.4); Mac: error `unsupported` |
| `dev` | with `--dev`, additions: `video_loss` `{"pct":0..100,"burst":mean packets}` (drop video at send: iid when burst 0, else Gilbert), `video_drop_ms` (drop all video for that long), `vcam_kill_beat` (stop the heartbeat, tests the DLL placeholder), `idr_pace_x` (2.0..6.0, default 3.0) | E2E and acceptance switches |

`stop` (A§3.2) also stops video streams; on a Mac video send it closes the encoder at once and the capture
session within 500 ms (camera light off).

### 3.3 Events (helper -> Beamer)

| ev | fields |
|---|---|
| `cameras` | `cameras` `[{key,name,transport,formats:["1280x720@30","1920x1080@30"],preferred}]`, `preferred` (key or null), `permission`. After `ready` (Mac), on every change (debounced 300 ms), on `cameras` |
| `vcam` | `install` (`ok`/`missing`/`outdated`/`broken`/`unsupported`), `dll_version`, `bundled_version`, `present`, `creating`, `in_use`, `app` (string or null), `want` (`{"w","h","fps"}` or null), `multi_app` (bool or null: Windows' per-camera toggle, when readable), `error`. After `ready` (Windows) and on every change; `in_use` on at once, off after 300 ms without a stream |
| `camera` | `stream`, `state` (`starting`/`running`/`interrupted`/`lost`/`stopped`), `device` (key in use), `format` (`"1280x720@30"`), `fps` (measured, last 1 s), `by_other` (another app on the Mac also uses the camera, when known), `effects` (list of active macOS effects: `center_stage`,`portrait`,`studio_light`,`background`,`reactions`,`edge_light`) |
| `permission` | gains `camera` (`granted`/`denied`/`undetermined`/`restricted`/`unknown`); Mac re-reads an `undetermined` state every 2 s and at once after `permission_request` completes (A§3.3 rule) |
| `stream_state` | unchanged shape; `error` codes for video: `no_camera`, `permission`, `busy`, `not_installed`, `decoder`, `internal` |
| `stats` | video send: `dir`,`kind:"video"`,`path`,`kbps`,`bitrate` (target), `fps`, `frames`, `idr`, `refresh`, `nack_rx`, `retx`, `pli_rx`, `pace_ms` (mean pacer wait), `enc_ms` (mean), `cam_fps`, `step` (rate-ladder level 0..2), `link_paused`, `rtt_ms`. Video recv: `dir`,`kind:"video"`,`path`,`kbps`,`fps_shown`,`frozen_ms` (display ticks without a new picture), `held_ms`, `late`, `lost` (after repair), `nack_tx`, `retx_ok`, `pli_tx`, `target_ms` (D), `d_p99_ms`, `dec_ms`, `decoder` (`hw`/`sw`), `sync_ms` (lip-sync hold, §4.9), `w`,`h`. Counters per interval as A§3.3 |
| `link_out` | unchanged for audio; video packets come in separate events `{"ev":"link_out","flow":..,"p":[...],"video":true}` |

### 3.4 Example session (PC app opens Beamer Camera; abbreviated)

```
PC  <- {"ev":"vcam","install":"ok","dll_version":1,"bundled_version":1,"present":true,"creating":false,"in_use":true,"app":"Microsoft Teams","want":{"w":1280,"h":720,"fps":30},"multi_app":false,"error":null}
    (PC Beamer floods camera_devices; the Mac's planner wants ("video", PC))
Mac -> {"cmd":"send","req":7,"stream":1288,"flow":"9f2c01aa","kind":"video","source":{"camera":null},"codec":{"name":"h264","profile":"constrained_high","w":1280,"h":720,"fps":30,"bitrate":2500000,"mtu":1200,"ltr":true},"key":"..."}
Mac <- {"ev":"ok","req":7}
Mac <- {"ev":"stream_state","stream":1288,"state":"starting","device":"6C707041-05AC-0011-0004-000000000001"}
    (Mac Beamer posts camera_open; PC accepts)
PC  -> {"cmd":"recv","req":12,"stream":1288,"flow":"9f2c01aa","kind":"video","codec":{...},"key":"...","sink":{"virtual_camera":true}}
PC  -> {"cmd":"vcam","placeholder":{"state":"starting","title":"Starting MacBook Air Camera","line":"on alexandres-macbook-air"}}
    (PC helper sends pli start; Mac encodes an IDR, paced)
Mac <- {"ev":"camera","stream":1288,"state":"running","device":"6C70...","format":"1280x720@30","fps":30.0,"by_other":false,"effects":[]}
Mac <- {"ev":"stream_state","stream":1288,"state":"running","device":"6C70..."}
PC  <- {"ev":"stream_state","stream":1288,"state":"running","device":"vcam"}
```

## 4. Media plane

### 4.1 Video media packet (C3 Q1, packet v1)

- Engine header (A§4.1) unchanged: `ptype=kMedia`, `codec=2` (H.264), `stream`, `seq` (per stream, +1 per
  packet including retransmitted *new* data; a retransmission re-sends the identical sealed bytes, so seq and
  nonce repeat with identical plaintext), `ts` = capture time in 90 kHz units since the stream's first
  captured frame (wraps after 13 h; compare modulo 2^32), `nsamp=0`, `channels=0`, `flags` bit1 marker on the
  first packet of the stream, `reserved=0`.
- AEAD plaintext = **12 B video header** + fragment:
  `frame u32 | frag u16 | nfrag u16 | vflags u8 | ltr u8 | rsv u16` (big endian).
  `vflags`: bit0 IDR, bit1 refresh, bit2 this AU is an LTR, bit3 retransmission (informational, the bytes are
  identical so it is set only if the first send had it: always 0; reserved), bits 4-7 0. `ltr`: 1 when the AU
  is an LTR, else 0 (VideoToolbox tokens never travel). Fragment: <= 1148 B of the AU, cut at any byte;
  `nfrag` <= 1024 (a 1080p IDR is ~225). UDP payload <= 1200 B (Tailscale MTU 1280).
- Sealed with the stream key, sender direction (A§4.1 nonce rule). Replay window per video recv stream:
  **1024** packets (`ReplayWindowN<1024>`, 16 x u64).

### 4.2 Feedback and timing packets

All sealed with the stream key; `nack`, `pli`, video `rr` in the **receiver direction** (`stream | kDirBit`, its
own seq space, like `kRR` today); `sr` in the sender direction (it shares the media seq space). `nack`/`pli`/
`rr` travel on UDP only; in link state they go through `link_out` too (they are tiny), except `nack`, which
is off in link state.

| ptype | payload | sent |
|---|---|---|
| `kNack` 7 | `n u8` (1..32), then `n` x (`seq_base u64`, `mask u16`: bit i = seq_base+1+i also missing) | at once when a gap appears (a packet arrives with seq > highest+1: reorder was 0 in every C3 run); repeated after max(2 x RTT, 15 ms), at most 3 times per seq, only while that frame's deadline has not passed and the hole is <= 64 packets |
| `kPli` 8 | `last_clean u32` (newest clean frame), `reason u8` (0 loss, 1 start, 2 decoder error, 3 stalled, 4 size), `rsv u8` | when the receiver becomes broken and NACK cannot repair (hole > 64, 3 tries, deadline passed), at stream start, on a decoder error; repeated every 300 ms while broken (start: every 100 ms until the first IDR's first packet) |
| `kSr` 9 | `ts u32`, `sender_ns u64` (now_ns of the capture of that ts) | every 1 s per stream, **audio and video**, and with the first media packet; lets the receiver map any `ts` to the sender's clock (§4.9) |
| `kRR` 5 (video) | 24 B: `newest_ts u32`, `margin u16` (min playout margin over the interval, 0.1 ms, 0 = late), `hi_seq_lo u32`, `lost u32` (cumulative after repair), `late u16` (cumulative), `acked u32` (newest clean frame), `recv_kbps u16`, `flags u8` (bit0 broken), `rsv u8` | every 50 ms per video recv stream **regardless of what this side sends** (`Flow::send_rr` keeps its 10 ms audio rule; video adds its own 50 ms timer), plus once at once after a refresh/IDR becomes clean |

Audio RR stays 6 B; a sender tells the two apart by the stream kind it knows.

### 4.3 Sender (`VideoSendStream`, engine)

- `codec` object (wire and `send`): `name` `"h264"`, `profile` `"constrained_high"` (C1: Baseline only if a
  decoder needs it; the PC's does not), `w`,`h` (1280x720 or 1920x1080), `fps` 30, `bitrate` (2,500,000 at
  720p, 5,000,000 at 1080p), `mtu` 1200, `ltr` true.
- AU in (`on_frame`, encoder thread) -> copy into a frame queue (cap 8 AUs; an overflow drops the **oldest**
  queued non-IDR AU and requests a refresh, counted) -> sender thread: fragment, seal, store each sealed
  packet in the **history** (1 s or 2048 packets, whichever is less; ~625 KB at 5 Mbit/s) and hand them to the
  flow's pacer.
- `kNack` from the network thread -> look up each seq in the history -> retransmit the stored bytes through the
  pacer **ahead of new data**; seqs no longer in history are ignored (the receiver's PLI follows).
- `kPli` -> recovery (C1 + C3):
  1. reason start/size or no acknowledged LTR -> `request_idr()`;
  2. else `request_ltr_refresh()`; a PLI whose `last_clean` is unchanged within RTT + 50 ms of the last refresh
     is a duplicate and ignored;
  3. if 3 refreshes in a row did not produce a clean ack within 1 s each -> `request_idr()`;
  4. **IDRs outside the stream start are rate-limited to one per 2 s**; a request inside that window becomes a
     refresh (or waits, if no LTR is acked).
- Acks: the RR's `acked` -> `acknowledge(frame)`. The sender keeps the **broken ranges** reported by PLIs
  (`last_clean`+1 .. the next refresh/IDR it sent) and never acknowledges an LTR inside one; the Mac backend
  maps engine frame numbers to VideoToolbox `RequireLTRAcknowledgementToken` values and passes every token of an
  LTR frame <= `acked` outside broken ranges in `kVTEncodeFrameOptionKey_AcknowledgedLTRTokens` (C1).
- Rate: `set_bitrate` from the safety ladder (§4.10) and from `video_config`.
- Before the first PLI the sender discards encoder output (it has not called `start_encoding()` yet: the camera
  and encoder session are warm, no AU is produced).

### 4.4 Pacer (per flow, `pacer.{h,cpp}`)

- Video packets only; audio media, RR, hello, ping, `sr`, `nack`, `pli` bypass it (sent at once by their
  threads, as today).
- Token bucket: capacity **32 packets**, refill **25 Mbit/s** for P frames, refreshes and retransmissions
  (effectively unpaced: they are 9-38 packets, C3). Packets of an **IDR** are released at
  `idr_pace_x` x the stream's target bitrate (default **3.0**, `dev` 2.0-6.0): 120 KB at 7.5 Mbit/s = 128 ms,
  256 KB at 15 Mbit/s = 137 ms. Frames queued behind an IDR wait (C3: ~100 ms once per stream start).
- A dedicated pacer thread waits on a condition variable until the next token (the 2 ms `NetLoop` poll is too
  coarse at 25 Mbit/s). Sends go through `Flow::send_media` (UDP; in link state see §4.10).

### 4.5 Receiver (`VideoRecvStream`, engine)

1. **Reassembly** per frame number (up to 64 frames in flight; an AU completes when all `nfrag` fragments are
   in). Gaps in `seq` -> NACK (§4.2).
2. **Clean/broken**: the stream starts broken. A complete IDR makes it clean. While clean, frames must arrive
   complete and in frame order; a frame that cannot be completed (NACK gave up, deadline passed, hole > 64)
   makes it broken -> PLI. While broken, a complete **refresh** or IDR makes it clean again (the sender only
   refreshes from LTRs it saw acknowledged, §4.3). Every complete AU is passed to the decoder in frame order
   (`decode(keep = clean)`), including AUs of a broken period, so the decoder's reference state matches the
   encoder's (C2: recovery is bit-exact from the refresh on; corrupt pictures decode silently, so `keep` is
   the only correctness signal).
3. **Playout deadline**: for each frame, `d` = completion time (receiver clock) - capture time mapped to the
   receiver clock (`sr` + the flow's clock offset from ping/pong: lowest-RTT sample of the last 10 s, as C3).
   Target `D` = p99 of `d` over the last 10 s (IDR frames excluded: paced on purpose) + 8 ms, clamped **[50,
   150] ms**, rises at once, falls by at most 15 ms per 0.5 s (C3 Q4: ~70 ms here). Present time =
   mapped capture time + D + lip-sync hold (§4.9).
4. **Presenter** (video thread): presents each kept picture at its present time; a picture completed after its
   present time + one frame period is **late**: decoded, never shown, unless it is the newest (then shown at
   once: after a stall the display jumps to the newest frame). Backlog older than D + 150 ms is decoded without
   being kept. When nothing new is presentable, the sink **holds** (heartbeat continues) for up to 2 s, then the
   helper switches the camera to the `reconnecting` placeholder (§4.6.4); it goes back to live on the next clean
   picture.
5. RR every 50 ms (§4.2), `acked` = newest clean frame **decoded ok** (`on_decoded(ok)`).
6. A decoder error (`on_decoded(ok=false)`) -> broken, PLI reason 2; 3 decoder errors in 10 s -> the sink
   reopens its decoder (software if the GPU path failed, §4.6.1) and PLI start.

### 4.6 Windows sink

#### 4.6.1 Decode (`videodec_win.cpp`)

`CLSID_CMSH264DecoderMFT`, `MF_LOW_LATENCY` + **`CODECAPI_AVLowLatencyMode` = TRUE** (mandatory, C2 Q5), NV12
out, D3D11 device manager (`MFT_MESSAGE_SET_D3D_MANAGER`) with an own staging-texture readback (C2: the fastest
path in the user session, 1.8 / 2.8 ms). Software fallback when no D3D11 video device, on
`DXGI_ERROR_DEVICE_REMOVED`/`RESET` (then retry hardware after 30 s), or with `--dev` `decoder=sw`. Size changes
via `MF_E_TRANSFORM_STREAM_CHANGE` (new output type, no message needed). Kept pictures go to a small pool (<= 6
NV12 buffers at the stream size); `present` copies the picture into the next live slot (0.07-0.13 ms, C2).

#### 4.6.2 Shared memory protocol (`native/audio/vcam-win/shm_protocol.h`, verbatim contract for CW4/CW5)

```cpp
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <cstdint>
namespace beamer::vcam {
// {19FCB9D1-E91E-45D0-B865-75D8F6D98A91}: the media source's COM class (HKLM\Software\Classes\CLSID)
constexpr wchar_t kClsidText[] = L"{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}";
constexpr wchar_t kFriendlyName[] = L"Beamer Camera";
constexpr wchar_t kRegKey[] = L"SOFTWARE\\Beamer\\Camera";      // HKLM: InstallGuid, OwnerSid, DllVersion, DllPath
// Kernel objects: L"Global\\BeamerCamera.<InstallGuid>.Shm" / ".Frame" (both created by the source)
//                 L"Global\\BeamerCamera.<InstallGuid>.Demand" (created by the helper)
// DACL on all three: SYSTEM, LocalService, OwnerSid (full); nobody else.
constexpr uint32_t kMagic = 0x4D414342;   // 'BCAM'
constexpr uint32_t kVersion = 1;          // bump only on a layout change; the helper supports [1, its max]
constexpr int kDllVersion = 1;            // bump on any DLL change that needs a reinstall
constexpr uint32_t kMaxW = 1920, kMaxH = 1080;
constexpr uint32_t kSlotBytes = 3112960;  // 1920*1080*3/2 rounded up to 4096
constexpr uint32_t kLiveSlots = 3;        // + 1 placeholder slot (index 3)
enum Mode : uint32_t { kModeNone = 0, kModeLive = 1, kModeHold = 2, kModeWaiting = 3 };
struct alignas(64) Slot {
  std::atomic<uint32_t> seq;              // seqlock: odd while the helper writes
  uint32_t w, h, stride;                  // NV12, chroma plane at data + stride*h
  uint64_t written_qpc;
  uint32_t picture;                       // helper's picture counter
  uint32_t data_offset;                   // from the start of the section, 4096-aligned
  uint32_t rsv[8];
};
struct alignas(64) Header {
  uint32_t magic, version, header_bytes, slot_count;   // slot_count = 4
  // written by the source
  std::atomic<uint32_t> state_seq;        // +1 on every change below; the source then sets the Demand event
  uint32_t streaming;                     // 1 between Start and Stop/Shutdown (repeated stops are harmless)
  uint32_t want_w, want_h, want_fps;      // the app's media type
  uint64_t state_qpc;
  uint32_t dll_version, source_pid;
  std::atomic<uint64_t> delivered, placeholders, repeats;
  // written by the helper
  uint32_t helper_pid;
  std::atomic<uint64_t> beat_qpc;         // every 100 ms while the helper has the section open
  std::atomic<uint32_t> mode;             // Mode
  std::atomic<uint32_t> latest;           // live slot with the newest picture
  uint32_t rsv[16];
  Slot slots[4];
};
}
```

Rules: the source creates section and frame event at activation (C2 Q4: only LocalService may create
`Global\` sections) and keeps them while any source object lives; the helper opens them when the Demand event
fires or by polling every 1 s, writes `helper_pid`, beats every 100 ms, writes a live picture into slot
`(latest+1) % 3` under the seqlock, publishes `latest`, sets the frame event; the source copies the newest slot
into the pending sample (re-checking the seqlock) and scales nearest-neighbour when the slot size differs from
the app's type (C2: +0.8 ms down, +3 ms up). The helper rejects a header whose `magic`/`version`/`slot_count`
it does not know (`vcam` `install: outdated`).

#### 4.6.3 Source DLL (`BeamerCameraSource.dll`)

As C2's prototype (`proto/win/source.cpp`), cleaned: activate -> `IMFMediaSourceEx` + `IMFGetService` +
`IKsControl` -> one `IMFMediaStream2`, `MF_DEVICESTREAM_FRAMESERVER_SHARED = 1`, NV12 **1280x720, 1920x1080,
640x360 @ 30** (640x480/4:3 only if an app needs it); 30 fps delivery clock; no network, no decode, no crypto;
reads `InstallGuid` and `OwnerSid` from HKLM to name and protect the objects; logs at most 1 line/s to
`%ProgramData%\Beamer\Camera\source.log` (rotated at 1 MB). Size of the DLL's own code ~1 kloc.

#### 4.6.4 What the camera shows (who decides)

| Condition (checked by the source on every delivery) | Picture |
|---|---|
| no section writer, or `beat_qpc` older than **250 ms** (Beamer not running, helper dead or stuck) | built-in image (`placeholder_builtin.inc`, Beamer dark surface, logo, "Beamer Camera", "Waiting for Beamer") + a small moving dot |
| `mode == Waiting` | slot 3 (the helper's placeholder) + moving dot |
| `mode == Live` or `Hold` | the newest live slot, repeated until a newer one |

The helper (not the DLL) decides between live, hold and waiting:
- **waiting** whenever no video recv stream is running (no route, Mac unreachable, starting, permission
  missing...), and at once (well under 250 ms) when the stream stops; text from Beamer's `vcam placeholder`;
- **live** from the first clean picture; **hold** while a running stream has nothing new to present; after **2 s**
  of hold -> waiting with Beamer's `reconnecting` text (Deviation 1).

The helper renders slot 3 itself (Direct2D + DirectWrite into BGRA, converted to NV12, 1280x720 and
1920x1080 rendered on demand at the app's size): Beamer's dark surface and accent colours (`win_app/tokens`
dark theme values copied as constants), the Beamer mark, a title line (24 px at 720p) and a detail line
(16 px). Texts (§8.4) come from Python; the helper never invents wording except the built-in image.

#### 4.6.5 Camera creation

At helper start, when `install == ok`: `MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
MFVirtualCameraLifetime_System, MFVirtualCameraAccess_CurrentUser, L"Beamer Camera", clsid, nullptr, 0)` +
`Start(nullptr)` on a worker thread (5.1 s the first time ever, 28 ms after, C2). The camera persists when
Beamer exits (apps keep listing it and show the built-in placeholder). `vcam enable:false` is never sent in
normal operation; `vcam-uninstall` removes it (`Remove()`).

#### 4.6.6 Who uses the camera (`app_usage_win.cpp`, best effort)

`HKCU\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\webcam` (packaged apps
by package family name, `NonPackaged\<path with #>` for desktop apps): entries with `LastUsedTimeStart` >
`LastUsedTimeStop` are in use. Read when `in_use` turns on and every 2 s while on. `app` = the display name
(package display name, else the exe's `FileDescription`, else the file name) **only when exactly one app is in
use**, else null. It covers any camera: on a PC with a physical camera too, `app` is only reported while
Beamer Camera itself is streaming, and the UI words it "probably" never; it shows the name or nothing.

### 4.7 On-demand start (sequence and budget)

1. App on T opens Beamer Camera -> source `Start` -> Demand event -> T's helper `vcam in_use:true want` (< 1 ms,
   C2) -> T's part republishes `camera_devices` (flood, direct link) and sets the placeholder "Starting...".
2. S's part receives it -> planner wants `("video", T)` -> **at the same time**: helper `send` (camera +
   encoder start in parallel: encoder session ~150 ms, C1) and `camera_open` to T.
3. T accepts -> helper `recv` -> `camera_opened ok`; T's recv stream sends PLI `start` every 100 ms.
4. S's first PLI: `start_encoding()` -> next captured frame -> IDR (37 / 56 ms, C1) -> paced ~130 ms -> T
   reassembles, decodes (2-3 ms), presents at D, writes the slot -> the app's next sample.

Budget: Python/link hops ~10-30 ms; camera start **[C4 / C1 real camera]**; IDR encode 37-56 ms; IDR pacing
~130 ms; D 50-70 ms; decode + slot 3-4 ms. **Target: first real frame <= 1.0 s after the app opened the camera
(p95 <= 1.5 s)**, to be confirmed by **[C4]**; the placeholder covers the gap.

Stop: the app closes -> `in_use:false` after 300 ms -> T republishes -> S's planner holds the demand **3 s**
(apps reopen the camera when they switch resolution or open a settings preview) -> `camera_close` + helper
`stop` -> camera light off <= 0.5 s later. Camera on for at most ~4 s after the last app closed it.

### 4.8 Latency budget (glass to glass, 720p)

| Term | Value | Source |
|---|---|---|
| Camera exposure + delivery to the helper | **[C1 real camera / C4]** | |
| Encode | ~5 ms | C1 |
| Pacing (P frames) | ~0 (bucket) | C3 |
| Playout deadline D (from capture, includes network + jitter allowance) | 50-70 ms | C3 |
| Decode + slot + source delivery | 2.4 ms (720p) / 4 ms (1080p) | C2 |
| App pipeline | Chrome 2-3 ms, DirectShow apps ~33 ms | C2 |
| **Total (excluding camera)** | **~60-80 ms** for MF/browser apps; **[C4]** measured | |

### 4.9 Lip sync

- Data now (CW2): every stream sends `sr`; every recv stream maps its `ts` to the sender clock; the audio recv
  stream publishes `audio_present_delay` = (now - sender capture time of the sample being rendered, mapped)
  + the render path latency (WASAPI period x queue; for CABLE the measured constant of A WP12: shared ~65 ms,
  exclusive ~30 ms beyond the render buffer, configured per mode); the video recv stream publishes its own
  `video_present_delay` = D + decode. Both on the same flow (same sender, same clock), so the clock offset
  cancels out.
- Policy (CW10): on a machine T that receives **both** the active mic stream from S (into its virtual mic) and
  a video stream from S, the video presenter adds `hold = clamp(audio_present_delay - video_present_delay,
  0, 150 ms)`, smoothed (EWMA 2 s, changes <= 10 ms per second), applied only after both streams have run 2 s.
  Audio is **never** delayed for video (calls are judged by audio delay); if video is the slower path (audio
  leads), nothing is done, and the stats show the negative skew. With today's numbers (Mac mic -> PC app ~110
  ms low-latency / ~170 ms shared, video ~70-90 ms) the hold is ~20-100 ms.
- Stats: `sync_ms` (current hold) on the video recv stream. Dev override `dev {"sync_offset_ms": n}` for
  measurement.
- Out of the engine's view: an app's own A/V buffering (e.g. Teams). Not compensated.

### 4.10 Rate safety and TCP fallback

- **Safety step-down** (sender, from video RR): over the last 2 s, post-repair `lost` > 2 % of packets or
  `margin` = 0 (late) for > 10 % of frames -> next level: base, base/2, base/4 (720p: 2.5 -> 1.25 -> 0.65
  Mbit/s; 1080p: 5 -> 2.5 -> 1.25). Step up one level after 10 s clean. No size change, no IDR.
- **Link state** (flow `link`, A§4.5): the video sender goes to `link.bitrate` (1 Mbit/s) and `link.fps` (15)
  at the current size, **sends video only through `link_out`** (no UDP duplicate; hellos still probe UDP),
  NACK off on both sides (TCP repairs; a packet Beamer drops is a hole answered by PLI -> refresh). Beamer
  drops video `link_out` packets while the link outbox holds > **32** messages (audio keeps its 64), dropping
  whole frames from the oldest; if the outbox stayed > 32 for 1 s it sends `video_link paused:true` (T holds,
  then shows "Reconnecting"); `paused:false` when the outbox is < 8 for 2 s or the flow is back `up`. Back in
  `up`: base bitrate/fps, UDP, NACK on.
- **SSH tunnel / loopback link** (no UDP host, A§4.2): no video stream is opened; status `no_direct_path`.

### 4.11 Mac capture and encode (C1 recommendations, binding for CW3)

- Format by size (`420v`, 1280x720 or 1920x1080), `activeVideoMinFrameDuration` 1/30; max frame duration left
  at the device default (auto exposure may drop to 15 fps in low light) **until Alex's low-light test** decides
  (open question 3); output `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`, `alwaysDiscardsLateVideoFrames`,
  the `CVPixelBuffer` handed straight to VT. A camera without the requested size: the nearest larger format, else
  the largest; the actual size goes into the AU (SPS) and the `camera` event.
- One `AVCaptureSession` per camera device, shared by every video send stream on it; one VT session per
  stream (each has its own LTR/ack state). Center Stage is left to the user (`.user`), reaction gestures default
  off through Info.plist (§9.1).
- VT: `EnableLowLatencyRateControl` + `RequireHardwareAcceleratedVideoEncoder` (fail rather than the 436 ms
  software encoder), `RealTime`, `ExpectedFrameRate` 30, `AllowFrameReordering` false,
  `ProfileLevel` `H264_ConstrainedHigh_AutoLevel`, `AverageBitRate`, `EnableLTR`; public keys only (no
  `MaxRefreshFrameIntervalDuration`). Session created as soon as the stream opens, kept for its life;
  re-created on a size change (new SPS/PPS + IDR).
- Observe `systemPreferredCamera` (KVO) when the source is null: switch devices in place (IDR). Session
  interruption / runtime error -> `camera` `interrupted` and retry every 2 s; device gone -> `camera_lost`.

## 5. Wire protocol on the Beamer link

### 5.1 Rules

- **No new cap.** Camera messages require cap `audio` on both ends (`_CAP_FOR`) **and** the feature word
  `camera1` in the peer's `audio_hello.features`; the part never posts a `camera_*` message to a peer without
  it. Caps stay 14 of `MAX_CAPS` 16; wire version byte stays `0x06`. `camera1` is announced when
  `feature_flags.CAMERA` is on and the helper's `ready.features` contains `video1`.
- Same rules as A§5.1: direct-link handling on link threads only enqueues; unknown fields ignored; malformed
  -> ignored, never ends a link; no `seq`. Limits: <= 16 cameras per machine, `key` <= 200 chars, `name` <= 128,
  `app` <= 128, message < 16 KiB.
- Video media and feedback in link state ride the existing `audio_frame` (flow-level, A§5.2); no new type.

### 5.2 Messages

| type | data | scope / when |
|---|---|---|
| `camera_devices` | `{"machine":id,"rev":int,"platform":str,"status":"ok"|"starting"|"failed"|"no_video","features":["camera_capture"|"virtual_camera"],"cameras":[{"key","name","transport","formats":["1280x720@30",...]}],"preferred":key|null,"permission":"granted"|"denied"|"undetermined"|"restricted"|"unknown"|null,"capture":{"state":"idle"|"starting"|"running"|"interrupted"|"failed","by_other":bool,"effects":[...],"error":code|null},"virtual_camera":{"install":"ok"|"missing"|"outdated"|"broken"|"unsupported","present":bool,"in_use":bool,"app":str|null,"want":{"w","h","fps"}|null,"multi_app":bool|null}}` | flooded (§5.3); Mac: `virtual_camera.install` `unsupported`; Windows: `cameras` empty, `permission` null |
| `camera_route` | §6.2 | flooded, per-entry merge |
| `camera_open` | `{"stream":u32,"flow":str,"codec":{...§4.3},"key":b64,"camera":{"key","name"}|null,"for":{"set_at":int,"by":id}}` | direct, S -> T |
| `camera_opened` | `{"stream":u32,"ok":bool,"error":"not_installed"|"outdated"|"helper"|"unsupported"|"busy"|null}` | direct, T -> S |
| `camera_close` | `{"stream":u32,"reason":"route"|"demand"|"permission"|"camera_gone"|"helper"|"shutdown"|"error"}` | direct, either side |

### 5.3 Propagation and link loss

- `camera_devices` and `camera_route` follow A§5.3 exactly (own `rev` per machine persisted in `camera.json`,
  newer `rev` or direct-from-author accepted, forwarded unchanged to every other direct peer with `camera1`;
  on link up both sides send their own list, every held list and the whole route). **Online** is the audio
  part's `online` set (the `links` graph of `audio_devices`); the camera part keeps no graph of its own.
- A machine republishes `camera_devices` when cameras, preferred camera, permission, capture state, install
  state, `in_use`/`app`/`want` or helper status change.
- Link down with P: video streams with P stop at once (camera off on S when no other stream uses it).
  Helper restart: every video stream of this machine is closed (`camera_close {reason:"helper"}`), status
  `starting`, then re-planned on `ready`. A PC helper restart leaves the camera in apps: the DLL shows its
  built-in placeholder within 250 ms, then the new helper's placeholder/live picture.

## 6. Route model

### 6.1 What Alex sets (cluster-wide, persisted on every machine)

1. **Active camera** `(machine, camera)`: `camera` null = that machine's system-preferred camera (followed);
   `machine` null = **off** (the initial state). Choosing a machine's camera is "turning the feature on" for
   that machine (its permission prompt, §9.1).
2. **Quality**: `720p` (default) or `1080p` = the largest size sent; bitrate 2.5 / 5 Mbit/s.

Nothing else: which machines see the camera is decided by demand (like the mic's virtual mic, A§6.3).

### 6.2 Route document (`camera_route` data and `route` in `camera.json`)

```json
{"camera":  {"machine":"<id>|null","device":{"key":"6C707041-05AC-0011-0004-000000000001","name":"MacBook Air Camera"}|null,"set_at":1793000000,"by":"<id>"},
 "quality": {"size":"720p","set_at":1793000000,"by":"<id>"}}
```

Per-entry stamps and merge as A§6.2 (`model.newer`, `next_stamp`). Unknown top-level entries are kept and
forwarded unchanged (so a later `sinks` entry passes through this version). Reserved for later, not
implemented: `"sinks": {"<T id>": {"from": "<S id>|null", ...}}` (per-sink override of the active camera).

### 6.3 On demand

The active camera on S feeds **every** online machine T != S that has `virtual_camera.install == ok`, only
while `virtual_camera.in_use` (demand) is true there, plus a 3 s hold after it turns false. The camera on S runs
only while at least one such stream exists. Why: same reasons as the mic (A§6.3): no focus rule, no extra
setting, camera light only during real use. If several PCs have demand, each gets its own stream (one capture,
one encoder per stream).

### 6.4 Planner (`core/camera/planner.py`, pure)

```python
def plan_camera(own: str, machines: Mapping[str, MachineCamera], audio_machines: Mapping[str, MachineAudio],
                route: CameraRoute, direct: frozenset[str], online: frozenset[str], now: float,
                demand_hold: Mapping[str, float], no_udp: frozenset[str]) -> CameraPlan

@dataclass(frozen=True)
class VideoSendSpec: peer: str; device: str | None; w: int; h: int; fps: int; bitrate: int
@dataclass(frozen=True)
class CameraPlan:
    sends: Mapping[str, VideoSendSpec]          # peer -> spec, only streams this machine sends
    status: CameraStatus                        # the active camera's overall state
    sinks: Mapping[str, CameraStatus]           # per machine with a virtual camera (or that lacks one)
    ask_permission: bool                        # own machine is S and permission is undetermined
    placeholder: Placeholder | None             # own machine is a sink: what the camera shows when not live
    wake_at: float | None
```

Rules (each machine evaluates the same function and acts on its own part):
1. S = `route.camera.machine`. None -> status `off`; every sink's placeholder `off`.
2. S must be online, have `camera_capture`, a resolvable camera (key, else unique name match, else
   `waiting_device`; null = S's `preferred`, none -> `no_camera`) and `permission == granted`
   (`undetermined` -> `waiting_permission`, `denied`/`restricted` -> `denied`).
3. For each online T != S with `virtual_camera.install == ok`: demand (or hold) and T in S's direct peers and
   T not in `no_udp` -> S sends `("video", T)` with size = min(quality, T's `want`) where `want` <= 720 lines
   counts as 720p (the source scales 640x360 down), bitrate by size, fps 30. Demand but not direct ->
   `unreachable`; direct but loopback-only -> `no_direct_path`; T without install -> `not_installed` /
   `outdated` / `broken` (shown even without demand); T = S or T on a platform without `virtual_camera` ->
   `no_virtual_camera` (shown as the out-of-scope note, §8.4).
4. Streams keyed by peer; the part diffs: new -> `send` + `camera_open` (Deviation 3); removed ->
   `camera_close {reason:"route"|"demand"|"permission"|"camera_gone"}` + `stop`; size/bitrate change of a
   running stream -> `video_config` (no reopen); device change -> close + open.
5. Receivers are passive: accept a `camera_open` from a direct peer when `install == ok` and the helper is
   ready, else `camera_opened {ok:false}`; the sender retries when the receiver's list or the route changes.
6. `CameraStatus = {state, reason, params}`; states: `off`, `idle` (ready, no app), `starting`, `streaming`,
   `reduced` (step-down or link state), `waiting_permission`, `denied`, `waiting_device`, `no_camera`,
   `waiting_machine`, `unreachable`, `no_direct_path`, `not_installed`, `outdated`, `broken`,
   `no_virtual_camera`, `busy` (the app was refused because another app holds the camera), `error`.

### 6.5 Beyond Mac -> Windows (model only)

A future Mac sink (needs an Apple-signed Camera Extension) or Linux sink (v4l2loopback) would announce
`virtual_camera` with its own `install` states; a Windows source would announce `camera_capture`. The route,
wire, planner and engine need no change; only backends do. This version never lists a Windows or Linux camera
as a source and never treats a Mac as a sink.

## 7. Python control plane

### 7.1 CameraPart (`core/camera/part.py`) and MediaContext

```python
class MediaContext(Protocol):          # implemented by AudioService (hunk, CW6); all calls on the worker
    own: str
    def post(self, peer: str, message: dict) -> bool: ...
    def helper_send(self, command: dict, reply: Callable[[dict], None] | None = None) -> None: ...
    def helper_state(self) -> str: ...                 # ok | starting | failed | no_helper
    def helper_features(self) -> frozenset[str]: ...
    def flow_for(self, peer: str) -> str | None: ...   # agreed flow id with a direct peer, else None
    def peer_has_udp(self, peer: str) -> bool: ...     # False for loopback / no host
    def new_stream_id(self) -> int: ...                # shared with audio streams
    def new_key(self) -> str: ...
    def peers_with(self, feature: str) -> frozenset[str]: ...  # direct peers whose audio_hello has it
    def direct(self) -> frozenset[str]: ...
    def online(self) -> frozenset[str]: ...
    def audio_machines(self) -> Mapping[str, MachineAudio]: ...
    def outbox_depth(self, peer: str) -> int: ...
    def changed(self) -> None: ...                     # publish a new snapshot to subscribers
    def schedule(self, at: float) -> None: ...         # wake the worker at monotonic time `at`
    clock: Callable[[], float]; wall: Callable[[], float]; logger: logging.Logger

class CameraPart:
    def __init__(self, ctx: MediaContext, state_dir: Path, platform: str): ...
    # worker thread only (the service routes to these)
    def helper_ready(self, ready: dict) -> None
    def helper_event(self, event: dict) -> bool        # True if consumed (cameras, vcam, camera, video streams)
    def link_up(self, peer: str) -> None
    def link_down(self, peer: str) -> None
    def link_message(self, peer: str, message: dict) -> None   # camera_*
    def link_out(self, peer: str, event: dict) -> None         # video link_out: outbox rule §4.10
    def apply(self, now: float) -> None                        # plan + diff; called after every audio apply and on wake
    def snapshot(self) -> CameraSnapshot
    # setters, from any thread through the service's queue (service.set_camera(...) etc.)
    def set_camera(self, machine: str | None, device: CameraRef | None) -> None
    def set_quality(self, size: str) -> None                   # "720p" | "1080p"
    def ask_permission(self) -> None                           # the page's "Allow camera" button (Mac)
```

The service gains thin public setters `set_camera`, `set_quality`, `ask_camera_permission`,
`camera_snapshot()` that enqueue to the worker / read the part's last snapshot; `subscribe` callbacks fire
for camera changes too. The part's own state lives on the worker, like the audio state (A§8.1).

### 7.2 Snapshot (`core/camera/model.py`)

```python
@dataclass(frozen=True) class CameraRef: key: str; name: str
@dataclass(frozen=True) class Camera: key: str; name: str; transport: str; formats: tuple[str, ...]; preferred: bool
@dataclass(frozen=True) class Capture: state: str; by_other: bool; effects: tuple[str, ...]; error: str | None
@dataclass(frozen=True) class VirtualCamera: install: str; present: bool; in_use: bool; app: str | None
                                             want: tuple[int, int, int] | None; multi_app: bool | None
@dataclass(frozen=True) class MachineCamera:
    id: str; platform: str; rev: int; status: str; features: frozenset[str]
    cameras: tuple[Camera, ...]; preferred: str | None; permission: str | None
    capture: Capture; virtual_camera: VirtualCamera
@dataclass(frozen=True) class Stamp: set_at: int; by: str            # same rules as core.audio.model.Stamp
@dataclass(frozen=True) class CameraEntry: machine: str | None; device: CameraRef | None; stamp: Stamp
@dataclass(frozen=True) class QualityEntry: size: str; stamp: Stamp
@dataclass(frozen=True) class CameraRoute: camera: CameraEntry; quality: QualityEntry; extra: Mapping[str, Any]
@dataclass(frozen=True) class CameraStatus: state: str; reason: str | None = None; params: Mapping[str, str] = {}
@dataclass(frozen=True) class VideoStreamView: stream: int; source: str; target: str; state: str; path: str
    w: int; h: int; fps: float | None; kbps: float | None; delay_ms: float | None; frozen_ms: float | None
@dataclass(frozen=True) class CameraSnapshot:
    own: str; machines: Mapping[str, MachineCamera]; route: CameraRoute
    status: CameraStatus; sinks: Mapping[str, CameraStatus]; streams: tuple[VideoStreamView, ...]
    permission: str | None; vcam_install_offer: str | None   # "install" | "update" | None (this PC only)
```

Pure functions: `merge_route`, `accept_devices`, `resolve_camera(ref, cameras)` (key, else unique name),
`size_for(quality, want)`.

### 7.3 Persistence: `<state_dir>/camera.json`

`{"version":1,"route":{...§6.2},"own_rev":int,"machines":{"<id>":{...last camera_devices data}}}`, atomic
(tmp + `os.replace`), debounced 500 ms, loaded at start (route re-applied as soon as links and devices allow).
`audio.json` is not touched.

### 7.4 Dev command files

Same mechanism as A§8.1 (`audio-dev.enabled`; one `audio-dev-cmd.json` read per second): new commands
`{"set_camera": {"machine": id|null, "device": key|null}}`, `{"set_quality": "720p"}`,
`{"camera_test_source": "synth"}` (the Mac sends the synthetic source instead of the camera: the harness waits
until `audio-dev-status.json` names it before setting the route), `{"helper_dev": {...}}` (passes a `dev`
command, e.g. `video_loss`). `audio-dev-status.json` gains `camera` (the camera snapshot + last video stats).

## 8. UI

### 8.1 Where the camera lives, and why

The **Audio page becomes "Audio & Camera"**: same key (`audio`), same place after Crossing, same shortcut
(Ctrl+3 on the PC, Cmd+<n> on the Mac), last-page memory unchanged. Reasons from the code:
- The camera module needs exactly what the page already has and owns: the shared machine map
  (`AudioMap`, `core/audio/view.py` tiles and arrows), the notices strip, the helper status and its Retry,
  and the AudioService snapshot/subscription the page already listens to. A separate page would duplicate the
  map, the notices and the helper state, and add a sidebar entry for one selector.
- The mic and the camera are chosen together for calls, and work the same way (one active source, on demand,
  apps pick "Beamer Microphone"/"CABLE Output" and "Beamer Camera" once). The Microphone module's pattern
  ("Microphone your apps hear" + status + hint) maps one-to-one onto "Camera your apps see".
- A section inside an unrenamed "Audio" page would hide the feature; renaming keeps the page discoverable at no
  cost (the key stays, so tests, settings and Alex's habit are unaffected).

The Camera module is its own widget file per app (`mac_app/camera_section.py`, `win_app/camera_section_win.py`)
inserted by a small hunk, with its own pure model (`core/camera/view.py`), so the camera WPs never edit the
audio modules beyond the named hunks. Look: same module chrome, fonts, theme tokens and status-line style as the
Microphone module; a camera glyph (SF Symbol `web.camera` / the Qt equivalent drawn like the mic glyph).

### 8.2 Page model (`core/camera/view.py`, shared by both apps)

```python
def camera_section(snap: CameraSnapshot, audio: AudioSnapshot, own_label: str, platform: str) -> CameraSection
def map_extras(snap: CameraSnapshot) -> CameraMapExtras     # passed to core.audio.view.page_model(camera=...)

@dataclass(frozen=True)
class CameraSection:
    label: str                      # "Camera your apps see"
    choices: tuple                  # core.audio.view.Choice: "Off", headers per machine, cameras (+ "Default camera")
    selected: str
    line: str; tone: str            # status sentence (§8.4) and its tone (ok / amber / fault / muted)
    detail: str                     # "1280x720 · 30 fps · 72 ms · Wi-Fi" while streaming, else ""
    quality_label: str; quality_choices: tuple; quality_selected: str   # "Quality": 720p / 1080p
    hint: str                       # "Apps on a PC choose 'Beamer Camera'."
    this_machine: tuple             # rows about this machine: (label, value, tone, action, action_label)
    notices: tuple                  # core.audio.view.Notice
@dataclass(frozen=True)
class CameraMapExtras:
    source: str | None; source_line: str             # tile badge "MacBook Air Camera"
    watching: Mapping[str, str]                      # sink machine -> "Teams uses the camera" / "Camera ready"
    arrows: tuple                                    # (source, target, state "active"|"waiting")
def apply_camera(service, choice: Choice) -> None
def apply_quality(service, size: str) -> None
def apply_camera_notice(service, notice: Notice) -> bool    # permission / settings / install / retry actions
```

All strings through `core.i18n` (`N_`/`tr`), English in code, as the Audio page.

### 8.3 Layout (both apps, same order and words)

```
 Audio & Camera
 Every machine's microphones, speakers and camera. Choose the microphone your apps hear, the camera they
 see, and where each machine's sound plays.

 ┌ Microphone ───────────────────────────────────────────────────────────────────┐
 │ (unchanged)                                                                  │
 └──────────────────────────────────────────────────────────────────────────────┘
 [ notices strip, unchanged ]
 ┌ Camera ───────────────────────────────────────────────────────────────────────┐
 │ Camera your apps see   [ MacBook Air Camera · alexandres-macbook-air     ▾ ]  │
 │ Quality                [ 720p | 1080p ]                                      │
 │ ● Microsoft Teams on lebig is using it.                                      │
 │   1280x720 · 30 fps · 72 ms · Wi-Fi                                          │
 │ Apps on a PC choose "Beamer Camera". Macs cannot show another machine's      │
 │ camera yet.                                                                  │
 │ ─────────────────────────────────────────────────────────────────────────── │
 │ This Mac   Camera access   Allowed                                           │   (Mac)
 │ This PC    Beamer Camera   Installed                        [ Remove… ]      │   (PC)
 │            or:             Not installed    [ Install Beamer Camera… ]  (UAC) │
 │ ⚠ lebig: Beamer Camera is not installed there. Install it in Beamer on lebig.│   (camera notices)
 └──────────────────────────────────────────────────────────────────────────────┘
 ┌ Machines ─────────────────────────────────────────────────────────────────────┐
 │  ┌───────────────────────┐            ┌──────────────┐                        │
 │  │ alexandres-macbook-air │  ─ ─ ▶     │ lebig        │   camera arrow: dotted │
 │  │ 🎙 MacBook Air Mic     │  ════▶     │ Sound -> ... │   accent, grey waiting │
 │  │ 📷 MacBook Air Camera  │            │ 📷 Teams uses│                        │
 │  └───────────────────────┘            │   the camera │                        │
 │                                       └──────────────┘                        │
 └──────────────────────────────────────────────────────────────────────────────┘
 [ selected machine panel, unchanged ]
```

Selector content: "Off"; per machine with `camera_capture` (header = machine name): "Default camera
(<preferred name>)", then each camera by name; machines without `camera_capture` are not listed. The Quality
control is disabled while Off. The module is hidden entirely when no machine in the cluster announces
`camera_capture` or `virtual_camera` (e.g. two PCs).

### 8.4 Status lines and notices (English, final wording owned by CW7)

| State | Line (tone) |
|---|---|
| `off` | "Off. Apps that open Beamer Camera see a 'camera off' picture." (muted) |
| `idle` | "Ready. No app on <T> is using Beamer Camera." (ok) |
| `starting` | "Starting for <app or 'an app'> on <T>…" (ok) |
| `streaming` | "<App> on <T> is using it." / "An app on <T> is using it." (ok); several sinks: joined list |
| `reduced` | "<…> is using it. Reduced quality: the network is slow." (amber) |
| `waiting_permission` | "Waiting for camera access on <S>. Answer the prompt on <S>." (amber) |
| `denied` | "<S> does not allow Beamer to use its camera." + action on S "Open Privacy Settings" (fault) |
| `waiting_device` / `no_camera` | "<camera> is not connected to <S>." / "<S> has no camera." (amber) |
| `waiting_machine` / `unreachable` / `no_direct_path` | "<S> is offline." / "<S> and <T> are not linked directly." / "Video needs a direct network path between <S> and <T>." (amber) |
| `not_installed` / `outdated` / `broken` | "Beamer Camera is not installed on <T>." / "needs an update on <T>" / "is damaged on <T>" + on T: action "Install Beamer Camera…" / "Update…" / "Repair…" (amber) |
| `busy` | "Another app on <T> has Beamer Camera. Windows lets one app use a camera at a time unless 'Allow multiple apps to use camera' is on for it." (amber) |
| `error` | "The camera stopped on <S>: <reason>." + Retry (fault) |

Out of scope, stated once in the hint: "Macs cannot show another machine's camera yet." and, when a Linux peer
announces audio, nothing (Linux never lists cameras). Placeholder texts (sent to the PC helper, §4.6.4):
`off` "Beamer Camera is off" / "Choose a camera in Beamer"; `waiting` "Waiting for <S>" / "<camera name>";
`starting` "Starting <camera name>" / "on <S>"; `denied` "<S> has not allowed the camera" / "Allow it in
Beamer on <S>"; `reconnecting` "Reconnecting to <S>" / "<camera name>"; `error` "Beamer Camera stopped" /
"<reason>". Built-in DLL image: "Beamer Camera" / "Waiting for Beamer".

### 8.5 Mac indicator

- macOS shows its own green camera light and the Control Center indicator naming Beamer; never hidden.
- Beamer's menu (`kvm_bridge_app.py`): while the Mac sends video, a line "Camera: <App> on <T>" (or "Camera:
  in use on <T>") under the drive status, and an item "Stop Sharing the Camera" that sets the active camera to
  Off (cluster-wide, so the PC shows the `off` placeholder). The menu-bar button tooltip gets the same line.
- The tile of the Mac on the map shows the camera badge in the accent colour while streaming.
- No notification banner (the system indicator already appears at each start).

### 8.6 Windows page specifics

The PC's own "This PC" row shows the install state with Install / Update / Repair / Remove actions (§9.2),
offered only while no video stream runs on this PC; after a UAC cancel the row says "Installation was
cancelled." Under it, when `multi_app` is false, a note: "One app at a time can use Beamer Camera. Windows
Settings > Bluetooth & devices > Cameras > Beamer Camera can allow several."

## 9. Packaging, install, permissions

### 9.1 Mac

- `setup.py` plist: `NSCameraUsageDescription` = "Beamer sends this Mac's camera to another computer while an
  app there uses Beamer Camera, after you choose it on the Audio & Camera page."; 
  `NSCameraReactionEffectGesturesEnabledDefault` = false (C1 Q5).
- `entitlements.plist` (bundle + `python`) and `entitlements-audio.plist` (helper) += 
  `com.apple.security.device.camera`. `build_dmg.sh` signing unchanged otherwise (the helper is already signed
  with its own entitlements). `Install-Beamer-Mac.command` adds `Camera` to the TCC services it resets for
  ad-hoc builds.
- **When the prompt appears**: as soon as the route names this Mac as the camera source (from either machine's
  page) and permission is `undetermined`, the part sends `permission_request` once per activation; the Mac
  page's "Allow Camera Access…" button sends it again (and, when `denied`, opens
  `x-apple.systempreferences:com.apple.preference.security?Privacy_Camera`). Never triggered by a PC app opening
  the camera. The prompt names Beamer (responsible process) and shows the usage string (human check H1).

### 9.2 Windows: install, update, uninstall

- **Shipped files**: `beamer-audio.exe` (unchanged location) and `camera\BeamerCameraSource.dll` beside it
  (`{app}\camera\` in Inno, the portable build's `COLLECT`, `Install-Beamer.ps1`). The DLL there is never loaded
  by Frame Server; it is the install source.
- **Install / update** (elevated, once per DLL version): the page's button calls `win_app/vcam_install.py`,
  which runs `ShellExecuteExW(lpVerb="runas", lpFile=<helper exe>, lpParameters="vcam-install --dll
  <bundled dll> --owner-sid <current user SID>")` (UAC prompt), waits on a background thread, then asks the
  helper for `vcam` status. `vcam-install`: copy to `%ProgramFiles%\Beamer\Camera\<kDllVersion>\
  BeamerCameraSource.dll` (inherited ACL: Users read, so LocalService reads it), write
  `HKLM\Software\Classes\CLSID\{19FC...}\InprocServer32` (default = path, `ThreadingModel=Both`) and
  `HKLM\SOFTWARE\Beamer\Camera` (`InstallGuid` kept if present, else new; `OwnerSid`; `DllVersion`; `DllPath`);
  delete older version folders that are not locked (a locked one is retried at the next install); exit code
  per §3.1. Then the unelevated helper creates and starts the camera (§4.6.5).
- **Versioned path**: a new DLL version installs beside the old one and re-points the CLSID; Frame Server
  loads the new file at its next activation after it idles (no service stop, C2 risk). The helper compares
  `bundled_version` with `DllVersion` -> `outdated` -> "Update Beamer Camera…". Beamer updates that do not bump
  `kDllVersion` need no admin step.
- **Uninstall**: the page's "Remove…" (and Beamer's uninstaller, best effort) -> helper `Remove()`s the camera
  (unelevated), then elevated `vcam-uninstall` deletes the CLSID, `HKLM\SOFTWARE\Beamer\Camera` and the
  folders; if the DLL is loaded it stops the `FrameServer` service first (all cameras pause ~1 s, warned).
- **Dev**: `ssh lebig` sessions are elevated (C2), so a dev agent runs `beamer-audio.exe vcam-install ...`
  over ssh with no UAC; `dev/deploy.sh` is not changed by this design (operator-owned); CW9 documents the
  one-liner in `native/audio/vcam-win/README.md`.
- No firewall change: the DLL has no network; the helper's existing allowance covers video.
- Signing: both binaries signed with `Sign-BeamerArtifact` when a signtool identity exists (Smart App Control
  risk, §11).

### 9.3 Mac permission and PC install in the UI (who sees what)

| Machine state | Mac page | PC page |
|---|---|---|
| Mac permission undetermined, Mac chosen | "Waiting for camera access" + Allow button | "Waiting for camera access on <Mac>. Answer the prompt on <Mac>." |
| Mac permission denied | fault + "Open Privacy Settings" | fault, no action (it is the Mac's) |
| PC not installed | amber notice naming the PC | "Install Beamer Camera…" (UAC) |
| PC outdated | amber notice | "Update Beamer Camera…" (UAC) |

## 10. Testing

### 10.1 Python (`core/tests/test_camera_*.py`, `unittest`, both machines)

`mac_app/.venv/bin/python -m unittest discover -s core/tests -p "test_camera*" -t .` (and the PC venv).
- `test_camera_wire`: builders/validators, limits, unknown fields, malformed ignored, `camera1` gating helpers.
- `test_camera_model`: route merge (commutative, idempotent, per entry, ties by `by`, unknown entries kept),
  device-list acceptance, `resolve_camera`, `size_for`.
- `test_camera_planner` (table-driven): off; on demand 0/1/2 sinks; hold 3 s; permission undetermined/denied;
  missing camera and return; preferred camera followed; not installed/outdated; not direct; loopback-only;
  quality vs `want` (360p, 720p, 1080p); Mac as T -> `no_virtual_camera`; Linux peer ignored.
- `test_camera_part` with `FakeHelper` + `fake_video` and a fake `MediaContext`: send+open at once on demand,
  close after the hold, `video_config` on quality change without reopen, `camera_opened` failure and retry
  rule, permission request exactly once per activation, placeholder pushed on every sink status change,
  link_out video drop over 32 and pause after 1 s, helper restart closes and re-plans, never blocks a link thread.
- `test_camera_cluster` (harness extension): 3 machines (Mac source, PC sink, Linux without `audio`) + one
  fork machine without `camera1`: convergence < 50 rounds for 200 seeded edit/partition sequences; no
  `camera_*` ever posted to a peer without `audio` or without `camera1`.
- `test_camera_view`: section model from `camera_fixtures.py` for every status of §8.4, choices, hint, map
  extras; `test_i18n_coverage` passes.

### 10.2 Engine (`native/audio/tests/video_tests.cpp`, in `engine_tests`)

Video header/nack/pli/sr/RR round trips and limits; `ReplayWindowN<1024>` (in-order, reorder, replay, too old);
fragmentation of AUs 1 B..300 KB and reassembly with any fragment order; NACK scheduling (immediate, 2 x RTT
retry, <= 3 tries, deadline, hole > 64 -> PLI); sender recovery state machine against a fake `CameraHandle`
(PLI -> refresh, duplicate PLI ignored, 3 failed refreshes -> IDR, IDR rate limit 2 s, broken-range ack
filtering); pacer timing (IDR at 3x bitrate within ±5 %, bucket 32 packets, audio bypass), playout target on
synthetic delay traces (p99 + 8 within [50,150], rise/fall rules), presenter late/newest rule, hold -> 2 s ->
waiting, `sr` mapping and lip-sync hold computation (CW10 part), rate ladder transitions, link-state switch
(video only on link, NACK off, fps 15). ASan/UBSan and TSan builds on the Mac; plain on the PC.

### 10.3 Null video backend (`null_video.{h,cpp}`)

`make_null_video_backend("cams=2;perm=granted;vcam=ok;ip=120000;p=10000;refresh=14000;enc_ms=5;dec_ms=2")`:
- Encoder: AUs at the requested fps with sizes from C1 (IDR, P lognormal CV 0.34, refresh), each AU carrying
  its frame number and its reference (previous frame, or the acknowledged LTR for a refresh, or none for an
  IDR) in its bytes; honours every `CameraHandle` call, including real LTR bookkeeping (LTR every 30 frames).
- Sink: a fake decoder that tracks reference validity exactly like an infinite-GOP H.264 decoder (a frame is
  correct iff its reference is correct and its bytes are complete) and **counts any `keep=true` picture that is
  not correct** (must stay 0); records present times; settable `vcam_in_use`, `want`, `install`.
- So two `beamer-audio-null` helpers exercise the whole protocol over 127.0.0.1, with `dev video_loss`.

### 10.4 OS acceptance (per backend WP, real hardware)

Mac: `cameras` inventory, synthetic source through VT, real camera once permission exists (H1). Windows: C1's
recordings through `recv` into the real DLL, consumers `camprobe` (MF), ffmpeg dshow, headless Chrome/Edge
(`proto/win/web/gum.html`) in Alex's session through the scheduled-task trick (`proto/win/scripts/in-session.ps1`).

### 10.5 Two-machine E2E (`dev/camera/e2e/`)

Built on `dev/audio/e2e` (imports `beamer.py`, `clock.py`, never edits them) and on C4's tools when they land
(`proto/e2e`). The Mac sends its helper's synthetic source (C1 code strip: frame number + Mac host time in
every picture); the PC reads frames with `camprobe read --strip c1` in Alex's session; Mac/PC clocks fitted by
`clock.py`. Cases and pass marks:

| Case | What | Pass |
|---|---|---|
| `cam_start` | open Beamer Camera on the PC 20 times (10 s on, 10 s off) | first real frame <= 1.0 s p50, <= 1.5 s p95 **[C4 adjusts]**; placeholder before it, never black |
| `cam_latency` | 10 min, 720p and 1080p | capture->app (synthetic, i.e. excluding the camera) p50 <= 90 ms, p95 <= 130 ms **[C4 adjusts]** |
| `cam_loss` | `video_loss` 0.5 % bursty (mean 6) and 1 % iid, 10 min each | frozen <= 300 / 200 ms/min; IDR count = 1 (start); 0 corrupt pictures (code strip checks) |
| `cam_stall` | `video_drop_ms 1500` every 2 min, 10 min | hold then recovery <= 1 RTT + 2 frames after the drop; recovery by refresh, no IDR |
| `cam_fallback` | `drop_udp` 30 s during a mic + camera session | video on the link <= 1.1 Mbit/s; mic glitches/min not above the audio-only fallback run + 1; resumes on UDP <= 2.5 s |
| `cam_stop` | close the PC app | Mac camera stopped (`camera` `stopped`) <= 3.5 s after the app closed |
| `cam_helper_kill` | kill the PC helper while streaming | DLL built-in placeholder <= 300 ms; live again <= 5 s after the watchdog restart |
| `cam_soak` | 30 min mic + camera, Wi-Fi as is | <= 1 frozen episode > 500 ms per 10 min outside logged Wi-Fi stalls; every hole > 5 packets logged with frame type; memory growth < 5 MB both helpers; CPU Mac helper < 6 %, PC helper < 5 % of a core |
| `cam_lipsync` (CW10) | flash + beep test source on the Mac mic and camera | A/V offset at the PC within -20..+40 ms (audio later = positive) |
| `cam_oldpeer` | PC <-> asusrog | link unaffected, no `camera_*` sent to it (log) |

Human checks (batched for Alex): **H1** the Mac camera prompt names Beamer and the image appears on the PC;
**H2** Camera app, Teams, Discord, a browser call show "Beamer Camera" and the placeholder; **H3** the
"multiple apps" toggle; **H4** after a logoff/reboot Beamer Camera is still listed; **H5** low light and
effects look (C1 Needs Alex 2-3).

## 11. Risks and open points

1. **Camera numbers missing** until H1 (cold start, capture latency, low-light fps): the start and latency
   targets carry **[C4]** marks.
2. **Smart App Control / WDAC** would likely block an unsigned DLL in Frame Server: sign when an identity
   exists; detect `install: broken` (CLSID present, activation fails) and say so.
3. **Logoff/reboot, sleep/resume, fast user switching** untested for the System-lifetime camera (H4); the helper
   re-runs create+Start at every start anyway (idempotent).
4. **Silent decoder corruption**: correctness rests on §4.5's clean/broken logic; the null sink's "0 corrupt kept
   pictures" and the E2E code-strip check are the guards.
5. **Keyframe-triggered Wi-Fi holes** (C3): IDRs paced at 3x (4x untested); soak logs holes > 5 packets with
   frame type; `idr_pace_x` tunable.
6. **One app at a time** on the PC (Windows default); surfaced as `busy` + the toggle note.
7. **Elevation UX**: `ShellExecuteEx runas` from a per-user app; over-the-shoulder admin accounts handled by
   passing `--owner-sid`; multi-user PCs: one owner only (out of scope).
8. **Video effects** apply at the user's choice; reactions cannot be disabled by Beamer.
9. **App name** is best effort (ConsentStore semantics are undocumented).
10. **CPU on the Mac** with mic + AEC + camera: estimated +2-3 % of a core for video (C1); soak checks.
11. **[C4]** risks from the end-to-end prototype.

Open questions (for the operator or Alex):
1. Page name "Audio & Camera" (vs keeping "Audio" with a Camera section): this design renames; cheap to revert.
2. Quality default 720p and the cluster-wide quality (vs per sink): kept simple; per-sink later via `sinks`.
3. Pin 30 fps in low light (darker, steady) or let auto exposure drop to 15 fps (brighter): needs H5; default
   not pinned.
4. "Stop Sharing the Camera" in the Mac menu sets the route Off for the whole cluster (simple, visible on the
   PC): acceptable, or should it only pause for N minutes?
5. Should Beamer offer to open Windows' camera settings to turn on "multiple apps"?

## 12. Work packages

Each WP works on its own branch from `feat/camera` (own worktree where the operator can give one; otherwise in
the shared checkout committing only its own paths, as in Phase 1), commits with `git commit -s`, and is merged by
the operator. File ownership is exclusive except the named hunks of §2.2. PC agents (`ssh lebig 'claude -p
...'`) work in `C:\Users\Alex\beamer-cwN\` (a clone, never the PC clone Beamer is built from) and write their
report to `~\briefs\<wp>.report.md`. Contract headers are committed first and never changed without the
operator: `video_backend.h` (CW2), `shm_protocol.h` (CW5), `core/camera/model.py` (CW1), `core/camera/view.py`
dataclasses (CW7).

**Wave 1 (start now, in parallel): CW1, CW2, CW3, CW4, CW5, CW9.** CW3/CW4 code against `video_backend.h` (CW2's
first commit, within its first hours) with local harnesses, and CW4 against `shm_protocol.h` (CW5's first
commit). **Wave 2: CW6 (after CW1), CW7 (after CW1 milestone 1), CW8 (after CW7 milestone 1).** **Wave 3: CW10,
CW11.**

### CW1 Python camera control plane (Mac agent)
- Goal: `core/camera` minus `view.py`: wire, model, planner, store, part, fake video helper; tests; cluster
  harness extension.
- Owns: `core/camera/{__init__,wire,model,planner,store,part,fake_video}.py`,
  `core/tests/test_camera_{wire,model,planner,part,cluster}.py`; hunks in `core/audio/fake_helper.py`
  (delegation) and `core/tests/audio_cluster_harness.py` (optional camera parts).
- Inputs: §3 (client side), §4.7, §4.10 (Python side), §5, §6, §7. Milestone 1 (first commit): `model.py` exactly
  as §7.2.
  Depends on: nothing (fake helper, fake `MediaContext`).
- Acceptance: all `test_camera_*` of §10.1 pass on the Mac and the PC; cluster: convergence < 50 rounds for
  200 seeded sequences, no `camera_*` to a peer lacking `audio` or `camera1`; on-demand open issued in the same
  worker iteration as the `vcam in_use` flood arrives; close at hold 3.0 s ±0.1 s (simulated clock); permission
  request exactly once per activation; the part never calls `post` off the worker (asserting host).

### CW2 Engine video transport (Mac agent; builds and runs its tests on the PC over ssh)
- Goal: §4.1-4.5, §4.9 data (sr, mappings, `audio_present_delay`/`video_present_delay`), §4.10, the control
  protocol additions of §3 on the engine side, `video_backend.h`, the null video backend, `run_helper` overload.
- Owns: `native/audio/engine/{video_backend.h,video_packet.*,video_send.*,video_recv.*,pacer.*,sync.*,null_video.*}`,
  `native/audio/tests/{video_tests.cpp,two_helpers_video.py}`; hunks of §2.2 in `packet.*`, `flow.*`,
  `streams.*`, `helper.*`, `sources.txt`, `build-tests.*`, `null_main.cpp`, golden control lines.
- Inputs: C3 report Q1-Q5, §2.4, §3, §4, §10.2, §10.3. Depends on: nothing. **First commit: `video_backend.h`
  verbatim.**
- Acceptance: engine tests pass on the Mac (plain, ASan/UBSan, TSan) and the PC; existing audio tests and
  `two_helpers.py` unchanged; `two_helpers_video.py`, two null helpers, 720p30 2.5 Mbit/s, 120 s each, mic
  stream on the same flow: no loss -> 0 frames missed after the start, first frame <= 300 ms after `send`;
  1 % iid -> frozen <= 200 ms/min and 0 IDRs after the start; 0.5 % Gilbert (mean 6) -> frozen <= 400 ms/min;
  1.5 s drop -> clean again <= RTT + 2 frames after it ends, by refresh; **0 incorrect kept pictures** in every
  run; no 10 ms window carries more than 3.0x bitrate x 10 ms + 32 packets during an IDR; audio `late+plc`
  within +1/min of the same run without video; link state: video <= 1.1 Mbit/s, only via `link_out` with
  `video:true`; engine video CPU < 1 % of a core per helper (null encode/decode excluded).

### CW3 Mac camera backend (Mac agent)
- Goal: `VideoBackend` for macOS (§4.11): inventory + KVO preferred camera, permission state/request, capture
  session sharing, VT low-latency encoder with LTR token map, refresh/IDR/bitrate/fps/format, interruption
  handling, effects/by_other reporting, `--dev` synthetic source; `main.mm` and `build-mac.sh` hunks.
- Owns: `native/audio/mac/{video_backend_mac.h,video_backend_mac.mm,camera_mac.mm,videoenc_mac.mm,synth_mac.mm}`,
  a `native/audio/mac/video_check.mm` harness; hunks in `main.mm`, `build-mac.sh`.
- Inputs: C1 report, `proto/mac/src` (lift `capture.mm`/`encode.mm`/`synth.mm`), §2.4, §3.3, §4.3, §4.11.
  Depends on: CW2's `video_backend.h` (local harness until the engine lands); real-camera items need H1.
- Acceptance (this Mac): `cameras` lists "MacBook Air Camera" with both formats and `preferred`; synthetic
  720p30 through the helper: encode p95 <= 8 ms, 1 s bitrate within ±10 %, helper CPU < 4 % of a core;
  `request_ltr_refresh` after `acknowledge` gives a non-IDR AU <= 40 KB (720p); without acks an IDR;
  `set_bitrate` effective within 300 ms; `set_format` -> IDR at the new size; `stop` -> capture session stopped
  <= 500 ms; universal binary (`lipo -archs` = `x86_64 arm64`); after H1: camera start -> first AU measured
  and reported (feeds §4.7), `permission` transitions `undetermined -> granted` seen without restart.

### CW4 Windows video sink in the helper (PC agent)
- Goal: `VideoBackend` for Windows (§4.6.1, 4.6.4-4.6.6): MF decoder (HW + SW fallback), picture pool,
  shared-memory client (open, heartbeat, live/hold/waiting, seqlocked writes), camera create/Start, install
  state detection, placeholder renderer, app usage, elevated verbs; `main.cpp` and `build.ps1` hunks.
- Owns: `native/audio/win/{video_backend_win.h,video_backend_win.cpp,videodec_win.cpp,vcam_win.cpp,placeholder_win.cpp,app_usage_win.cpp,vcam_install_win.cpp}`,
  a `native/audio/win/video_check.cpp` harness; hunks in `main.cpp`, `build.ps1`.
- Inputs: C2 report, `proto/win/{feeder,vcamctl}.cpp`, §2.4, §3, §4.6, §9.2.
  Depends on: CW2's `video_backend.h`, CW5's `shm_protocol.h` (and CW5's DLL for the live checks).
- Acceptance (lebig, helper in Alex's session via the scheduled task): C1's 720p/1080p recordings through
  `recv` -> DLL -> `camprobe`: decode p95 <= 4 ms (720p) / <= 5 ms (1080p) hardware, 0 frames of decoder lag;
  forced software works (CPU < 30 % of a core at 1080p); `vcam in_use` <= 50 ms after an app opens the
  camera, off 300 ms after it closes; `app` reported for Chrome and `camprobe`; placeholder on screen <= 100 ms
  after a `vcam placeholder`; hold then waiting at 2 s; `vcam-install`/`vcam-uninstall` work elevated (ssh)
  and exit 3 unelevated; install states `missing/ok/outdated/broken` detected; simulated device removal ->
  software decode within 1 s and back to hardware after 30 s.

### CW5 Windows camera source DLL (PC agent)
- Goal: `BeamerCameraSource.dll` per §4.6.2-4.6.3, built-in placeholder image, tools (`camprobe` with
  `--strip c1`, `feeder` writing the shared memory), DLL build script.
- Owns: `native/audio/vcam-win/**`. **First commit: `shm_protocol.h` verbatim.**
- Inputs: C2 report and `proto/win/{source.cpp,common.h,camprobe.cpp,feeder.cpp,web/gum.html}`, §4.6.
  Depends on: nothing.
- Acceptance (installed with `vcam-install` or a local script until CW4 lands): MF, DirectShow (ffmpeg dshow),
  headless Chrome/Edge see "Beamer Camera"; 720p/1080p/360p NV12 @30 for 60 s with 0 gaps; `feeder` -> app p95
  <= 1.0 ms (720p) / 1.5 ms (1080p); built-in placeholder 250 ±50 ms after the heartbeat stops; `mode`
  waiting -> slot 3 within one frame; repeated Stop/Shutdown idempotent (state_seq increments, no crash);
  objects' DACL = SYSTEM, LocalService, owner SID (checked with `icacls`-style dump); Frame Server CPU <= 1.5 %
  of a core at 1080p; unregister + delete leaves no `SWD\VCAMDEVAPI` node.

### CW6 Link and service integration (Mac agent; PC verified over ssh)
- Goal: the `camera1` feature, `camera_*` gating and dispatch on both apps, the `MediaContext` hunk in the
  service, the flag, WIRE.md.
- Owns: §2.2 hunks in `core/link.py`, `core/receiver.py`, `mac_app/bridge.py`, `win_app/sender.py`,
  `win_app/kvm_bridge_win.py` (message forwarding only), `core/audio/service.py`, `core/feature_flags.py`,
  `WIRE.md`, `mac_app/setup.py` (`includes`); new `core/tests/test_camera_link_gating.py`.
- Inputs: §2.2, §5, §7.1. Depends on: CW1 (`part.py` API).
- Acceptance: gating test (initiator and responder links drop `camera_*` without `audio`; the part sends nothing
  to a peer without `camera1`); full `core/tests`, `mac_app/tests`, PC `win_app/tests` unchanged except the known
  failures (`dev/README.md`); `dev/deploy.sh` both with null video (`audio-dev.enabled` helper =
  `beamer-audio-null`) until CW3/CW4 land: both logs show `camera1` and `camera_devices` exchanged; caps still
  14; asusrog link up and silent; audio E2E `mic_mac_pc` unchanged.

### CW7 Camera page model, Mac UI, menu indicator (Mac agent)
- Goal: §8.2-8.5 on the Mac: `core/camera/view.py`, the AppKit Camera module, map extras, page rename, menu
  line and "Stop Sharing the Camera".
- Owns: `core/camera/view.py`, `core/tests/{test_camera_view.py,camera_fixtures.py}`, `mac_app/camera_section.py`,
  `mac_app/tests/test_camera_section.py`; hunks in `core/audio/view.py`, `mac_app/audio_page.py`,
  `mac_app/pages.py`, `mac_app/kvm_bridge_app.py`, `mac_app/setup.py` (`"camera_section"`), `locales/beamer.pot`.
- Inputs: §6.4 statuses, §7.2, §8. Depends on: CW1 milestone 1. **Milestone 1 (first commit): `view.py` +
  `camera_fixtures.py` + `test_camera_view.py`** so CW8 can start.
- Acceptance: `test_camera_view`, `test_audio_view`, `test_i18n_coverage` pass; offscreen renders (as
  `mac_app/tests/design_offscreen.py`) of 6 fixture states (off; idle; streaming to lebig with app name;
  permission undetermined; PC not installed; reduced) in light and dark, reviewed by the operator; the menu
  line appears and "Stop Sharing the Camera" calls `set_camera(None, None)` (fake service); no main-thread call
  > 16 ms over a 60 s run with snapshots at 4 Hz.

### CW8 Qt Camera module (PC agent)
- Goal: §8.3, §8.4, §8.6 in `win_app` (Windows; on Linux the module stays hidden), install/update/remove actions
  wired to `win_app/vcam_install.py`.
- Owns: `win_app/camera_section_win.py`, `win_app/tests/test_camera_section_win.py`; hunks in
  `win_app/audio_page_win.py`, `win_app/pages_win.py`.
- Inputs: §8, `win_app/widgets.py`, `win_app/theme.py`. Depends on: CW7 milestone 1 (never edits `view.py`, asks
  CW7), CW9's `vcam_install.py` API (mock until it lands).
- Acceptance: offscreen renders (`QT_QPA_PLATFORM=offscreen`) of the same 6 states, light/dark; install button
  calls `vcam_install.install()` with mocks and shows the cancelled state; keyboard reaches every control; the
  page still opens with Ctrl+3; `win_app` suite otherwise unchanged.

### CW9 Packaging, install, permissions (Mac agent; PC over ssh)
- Goal: §9: plist keys, entitlements, TCC reset, DLL shipping on Windows (Inno, portable, Install-Beamer.ps1),
  `win_app/vcam_install.py` (state, UAC launch, owner SID, wait), uninstall hook in the Inno uninstaller (best
  effort), README one-liner for dev installs.
- Owns: `mac_app/{entitlements.plist,entitlements-audio.plist,Install-Beamer-Mac.command,build_dmg.sh}` (camera
  hunks), `mac_app/setup.py` (plist keys), `win_app/{build_win_app.ps1,Install-Beamer.ps1,Beamer-Setup.iss,build_portable.spec}`,
  new `win_app/vcam_install.py`, `win_app/tests/test_vcam_install.py`.
- Inputs: §9, §2.5 outputs. Depends on: build outputs (skips the DLL with a warning while CW5's build script
  is missing, as audio WP8 did), CW4 verbs for the live check.
- Acceptance: `dev/deploy.sh` both succeeds; `codesign -d --entitlements -` shows `device.camera` on the app,
  `python` and `MacOS/beamer-audio`; `plutil -p Info.plist` shows both camera keys; `%LOCALAPPDATA%\Beamer\camera\
  BeamerCameraSource.dll` present; `vcam_install.py` unit-tested (verb line, SID lookup, cancel = error 1223,
  versions) with mocks; one real install via the UAC path is a human check (H6, ~1 min).

### CW10 Lip sync (Mac agent; PC part over ssh)
- Goal: §4.9 policy in the PC helper (hold computation, smoothing, application in the presenter), CABLE render
  latency constants per mode, stats, dev offset.
- Owns: `native/audio/engine/sync.{h,cpp}` (policy part, after CW2 merges), the presenter hook in `video_recv.cpp`
  (agreed with CW2's owner), `native/audio/tests/sync_tests.cpp`, E2E case `cam_lipsync` in `dev/camera/e2e/`
  (agreed with CW11).
- Depends on: CW2, CW4 merged (CW3 for the live case).
- Acceptance: unit tests on synthetic delay pairs (hold within ±2 ms of the formula, bounds, smoothing rate);
  `cam_lipsync` within -20..+40 ms with the mic in shared and in low-latency mode.

### CW11 Two-machine E2E harness and soak (Mac agent with ssh)
- Goal: §10.5 (all cases except `cam_lipsync`'s analysis, which CW10 adds).
- Owns: `dev/camera/e2e/**` (may import `dev/audio/e2e/*` and C4's `dev/camera/proto/e2e/*`; edits neither).
- Depends on: CW1-CW6 and CW9 merged; CW7/CW8 not needed (dev command files).
- Acceptance: every case runs unattended from the Mac and writes `runs/<case>/result.json`; pass marks of §10.5
  met or each miss explained with its trace; results table handed to the operator for `STATUS.md`.

### Dependency summary

| WP | Machine | Wave | Can run concurrently with | Needs before it can finish |
|---|---|---|---|---|
| CW1 Python control plane | Mac | 1 | all wave 1 | - |
| CW2 Engine video transport | Mac (+PC build) | 1 | all wave 1 | - |
| CW3 Mac camera backend | Mac | 1 | all wave 1 | CW2 `video_backend.h` (first commit); CW2 for the full helper; H1 for real-camera items |
| CW4 Windows video sink | PC | 1 | all wave 1 | CW2 `video_backend.h`, CW5 `shm_protocol.h`; CW2 + CW5 for live checks |
| CW5 Source DLL | PC | 1 | all wave 1 | - |
| CW9 Packaging | Mac (+PC) | 1 | all wave 1 | CW3/CW4/CW5 build outputs for the full result |
| CW6 Link + service integration | Mac (+PC) | 2 | CW7, CW8 | CW1 |
| CW7 Page model + Mac UI | Mac | 2 | CW6, CW8 | CW1 milestone 1; CW6 for live use |
| CW8 Qt module | PC | 2 | CW6, CW7 | CW7 milestone 1; CW9 `vcam_install.py` |
| CW10 Lip sync | Mac (+PC) | 3 | CW11 | CW2, CW4 (CW3 for live) |
| CW11 E2E + soak | Mac (+ssh) | 3 | CW10 | CW1-CW6, CW9 |

Two Mac agents and two PC agents never edit the same file: the only multi-owner file is `mac_app/setup.py`
(CW6 `includes`, CW7 one `py_modules` line before `"audio_page",`, CW9 plist keys: non-adjacent hunks), as in
audio. `core/audio/service.py` is CW6's alone; `core/audio/view.py` and the audio pages are CW7/CW8's alone for
their hunk; engine files are CW2's alone until it merges (CW10 after).

## 13. C4 amendment points

When `reports/c4.md` lands, update: PHASE1 §1 (C4 table) and §2 (camera start, glass-to-glass); §4.7 start
budget and target; §4.8 total; §10.5 pass marks of `cam_start` and `cam_latency`; §11 risk 11; any change C4
forces in the recovery loop (§4.3 rules 2-4, ack cadence §4.2) or the start sequence (§4.7 step 2-4).
