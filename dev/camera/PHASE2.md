# Camera across Beamer: Phase 2 design (integration into the app)

Date 2026-11-11. Branch `feat/camera` (from `feat/audio`). Inputs: `PHASE1.md` (verdicts), the operator
decisions in `STATUS.md` and in D1's brief, `reports/c1.md`, `c2.md`, `c3.md`, the prototypes in `proto/`, and
the audio system this extends: `dev/audio/PHASE2.md` (cited as **A§n**) and its code (`native/audio`,
`core/audio`, the Audio pages). C4 (end-to-end prototype) runs in parallel: every number it owns is marked
**[C4]** and will be amended when `reports/c4.md` lands. This document is the contract for the implementation
agents; section numbers (§) are referenced by the work packages in §12.

Revision D2 (same day): integrates the R1 review (`reviews/r1-phase2.md`) and the operator's rulings on it;
the finding-by-finding resolution is `reviews/r1-resolution.md`. `video_backend.h` and `shm_protocol.h` were
changed by this revision; they freeze when CW2 / CW5 commit them (§12).

## 0. Decisions at a glance

| # | Question | Decision |
|---|---|---|
| 1 | Process | **No new helper.** Video is a second backend of the existing `beamer-audio` helper (name kept, §2.3): Mac capture + VideoToolbox in ObjC++ (`native/audio/mac/`), Windows MF decode + virtual-camera feed in C++ (`native/audio/win/`), the transport in the portable engine (`native/audio/engine/video_*`). A new interface `video_backend.h` sits beside the unchanged `backend.h` (§2.4). |
| 2 | Windows camera | Own media source DLL `BeamerCameraSource.dll` (`native/audio/vcam-win/`), loaded by Frame Server, **owns the shared memory and treats everything the helper writes there as hostile** (§4.6.3); the helper creates the camera **System lifetime, CurrentUser access**, unelevated, at every start (idempotent). Decode stays in the helper (MF H.264, D3D11, `AVLowLatencyMode`, software fallback). Shared-memory protocol §4.6.2. |
| 3 | Media plane | Stream kind `video` on the **existing UDP flow**, packet v1 header unchanged, codec id 2 (H.264), 12 B video header, <= 1200 B UDP payload; new ptypes `nack` (7), `pli` (8) on the video stream, `time_map` (9) on the flow's control channel; 28 B video receiver report; replay window 1024 for video (§4.1-4.2). **One frame numbering**: the engine numbers each AU when the encoder hands it over; a number is never reused or skipped, so any AU dropped after that is a visible hole (§4.1). |
| 4 | Recovery | NACK (<= 3 tries, holes <= 64 packets, within the frame deadline) + **LTR refresh from LTRs the receiver reported inside a clean run**; IDR only at stream start, size change, decoder reopen or last resort, **always paced (3x bitrate)**; start/size IDRs deduplicated and <= 1 per 1 s, recovery IDRs <= 1 per 2 s; no FEC, no periodic intra refresh, no ABR (safety step-down only) (§4.3-4.5, §4.10). |
| 5 | Playout | Adaptive deadline p99(10 s) + 8 ms in [50, 150] ms on a flow clock offset estimated from the existing ping/pong; late frames decoded, never shown; hold the last good frame across loss, `reconnecting` placeholder after 2 s of hold (engine-driven, §4.5); placeholder after 250 ms with nothing feeding the camera (§4.6.4). |
| 6 | Wire | **No new cap** (still 14 of 16): camera messages ride cap `audio`, announced by the feature word `camera1` in `audio_hello`. New types `camera_devices`, `camera_route` (flooded), `camera_open`, `camera_opened`, `camera_close` (direct). Fallback packets stay in `audio_frame`; **all feedback (RR, NACK, PLI, time map) also travels over the link in link state** (§4.10, §5). |
| 7 | Routing | Cluster-wide **active camera** (machine + camera, or off) and a cluster-wide **quality** (720p / 1080p), like the active mic. It feeds the "Beamer Camera" of every machine that has one, **only while an app there has it open** (on demand), always at the cluster quality (the PC source scales to the app's size). Model allows any source -> any sink; implemented: Mac source -> Windows sink (§6). |
| 8 | Kill switch | **"Stop Sharing the Camera" is a Mac-local switch** (`sharing` in that Mac's own `camera_devices`, persisted there): while it is off the Mac refuses to start its camera whatever the route says, and no other machine can turn it back on; PCs show "Turned off on <Mac>" (§6.1, §8.5). |
| 9 | Python | New package `core/camera/` (wire, model, planner, store, part, view). `CameraPart` runs **inside the AudioService worker** and uses its helper and flows through a narrow `MediaContext` (§7). |
| 10 | UI | The **Audio page becomes "Audio & Camera"** (same key, place and shortcut) with a **Camera** module under the Microphone module, a camera badge and arrow on the shared machine map, and Mac menu items (sharing switch, permission) (§8). |
| 11 | Install / permissions | PC: one elevated step offered from the page ("Install Beamer Camera", UAC prompt) runs `beamer-audio.exe vcam-install`: DLL to `%ProgramFiles%\Beamer\Camera\<dll version>\`, HKLM COM class + `HKLM\SOFTWARE\Beamer\Camera`; uninstall through Beamer's existing elevated maintenance step. Mac: `NSCameraUsageDescription`, `com.apple.security.device.camera` on app and helper; **the macOS prompt is only ever triggered by an action on the Mac** (choosing its camera on the Mac's page, or "Allow Camera Access…" on the Mac) (§9). |
| 12 | Capture size | The Mac always encodes exactly the cluster quality (1280x720 or 1920x1080): a format by size when the camera has it, else the smallest larger format, **centre-cropped to 16:9 and scaled** (§4.11). One scaled picture per camera; one encoder session per sink stream (§4.11, operator note in §11). |
| 13 | Lip sync | Every video stream, and every audio stream to a peer with `video1`, is covered by the flow's `time_map` (capture-time mapping) from day one. Policy: on a PC receiving both the Mac mic and the Mac camera, video is held back to the mic's presentation delay, within 0..150 ms; audio is never delayed for video (§4.9). Implemented by CW10. |
| 14 | Tests | Python `unittest` with the fake helper grown a video side; engine C++ tests; a **null video backend** (fake encoder + reference-checking fake decoder) so two null helpers exercise the whole video protocol on one machine; shared-memory fuzzing of the DLL; OS acceptance per backend; a two-machine E2E harness `dev/camera/e2e/` built on `dev/audio/e2e` (§10). |

### Deviations from the operator decisions (with reasons)

1. **Placeholder vs network stalls (clarification).** "Placeholder after 250 ms of no frames" is applied to
   *nothing feeding the camera*: no stream, stream starting, helper gone or stuck. During a Wi-Fi stall of a
   running stream the helper keeps the camera fed with the **last good frame for up to 2 s** (C3: "hold the
   last frame, not black, during the stall"; stalls of 0.5-1.5 s are normal here), then switches to the
   placeholder "Reconnecting to <Mac>". Reason: a placeholder flashing in a call at every 0.3-1.5 s Wi-Fi
   stall is worse than a short freeze. §4.5, §4.6.4.
2. **TCP fallback keeps the size, not "720p15"**: in link state the encoder drops to 1 Mbit/s and 15 fps at
   the current size (no size change, so no IDR over the slow path; LTR continuity kept). C3's "720p15 or
   360p30" both need an IDR (~80-120 KB) at 1 Mbit/s, i.e. ~1 s of link time. §4.10.
3. **Video send starts before `camera_opened`** (unlike audio, A§6.4 rule 4, where `send` waits for
   `audio_opened`): the camera warms up while the open round trip runs, and no media leaves the Mac before
   the receiver's start PLI, so nothing is lost to an unknown stream id. Reason: on-demand start latency.
   §4.7.
4. **Helper name kept** `beamer-audio` (C1 offered `beamer-media`): the PC's firewall allowance is for
   `beamer-audio.exe` (`dev/audio/TEST-ALEX.md` §1), and the name is in packaging, tests and Alex's notes.

The operator accepted these four. The operator's later rulings on R1 replace two earlier defaults: "Stop
Sharing the Camera" is a Mac-local switch, not a cluster-wide route change (§6.1); the Windows multi-app
setting is shown as a static help line, and the `busy` state is gone (§8.6). No other deviation.

## 1. Terms

- **Audio terms** as A§1 (machine id, direct peer, helper, engine, flow, stream, device key).
- **Source machine** S: owns the physical camera (Mac). **Sink machine** T: has a virtual camera (Windows).
- **Beamer Camera**: the Windows virtual camera (`MFCreateVirtualCamera`, friendly name "Beamer Camera"; Windows
  shows "Beamer Camera (Caméra virtuelle Windows)" in French, its suffix). **Source DLL**: `BeamerCameraSource.dll`,
  the media source Frame Server loads. **vcam**: the helper's side of it (create/start, shared memory, demand).
- **Camera key**: AVFoundation `uniqueID`. **CameraRef** = `{key, name}`. "Default" = macOS
  `AVCaptureDevice.systemPreferredCamera`, followed.
- **AU**: one H.264 access unit (Annex-B, SPS/PPS before every IDR). **Frame number** `frame`: u32 the engine
  assigns in `EncoderSink::on_frame`, i.e. when the encoder hands the AU to the transport: 0, 1, 2... per
  video send stream, +1 per AU, never reused, never skipped. The same number is on the wire, in the receiver
  report and in `acknowledge_ltr()`. An AU dropped after it was numbered (sender queue overflow, Python link
  drop) leaves a **hole** in the numbers; the receiver treats a hole as a lost frame.
  **LTR**: long-term reference (VideoToolbox `EnableLTR`). **Refresh**: an AU that references only LTRs the
  encoder was told are acknowledged (`ForceLTRRefresh`).
- **Clean**: a frame the receiver decoded from complete data **and whose whole reference chain it received and
  decoded without error** (§4.5.2). **Clean run**: consecutive frames `[clean_since, acked]`, all clean,
  started by an IDR or a refresh. **Broken**: the receiver lost a frame (or the decoder failed) and no IDR or
  refresh has made it clean since.
- **Verified LTR**: an LTR frame number inside a clean run the receiver reported (§4.3); the only LTRs the
  sender acknowledges to the encoder.
- **Demand**: an app on T has Beamer Camera open (the source DLL is streaming).
- **Playout deadline D**: time from capture to display on T (§4.5). **Hold**: re-showing the last good frame.
- **Sharing switch**: the Mac-local "Share the camera" on/off (§6.1); off = the Mac never starts its camera.

## 2. Code layout, ownership, native build

### 2.1 Tree (new files, with their owner work package, §12)

```
native/audio/engine/video_backend.h   VideoBackend contract (§2.4), verbatim                                CW2 (first commit)
native/audio/engine/video_packet.{h,cpp}  video header, nack/pli/time_map payloads, video RR (§4.1-4.2)      CW2
native/audio/engine/video_send.{h,cpp}    VideoSendStream: number, queue, fragment, seal, history, PLI, acks  CW2
native/audio/engine/video_recv.{h,cpp}    VideoRecvStream: reassembly, clean runs, NACK/PLI/RR, presenter     CW2 (presenter hook: CW10)
native/audio/engine/pacer.{h,cpp}         per-flow video token bucket (§4.4)                                 CW2
native/audio/engine/sync.{h,cpp}          time-map bookkeeping, ts->local mapping, lip-sync hold (§4.9)      CW2 (hold policy: CW10)
native/audio/engine/null_video.{h,cpp}    null video backend: fake encoder + reference-checking sink (§10.3) CW2
native/audio/tests/video_tests.cpp        engine video tests (§10.2), built into engine_tests                CW2
native/audio/tests/sync_tests.cpp         lip-sync policy tests (§4.9)                                       CW10
native/audio/tests/two_helpers_video.py   two null helpers over 127.0.0.1 with loss (CW2 acceptance)         CW2

native/audio/mac/video_backend_mac.{h,mm} VideoBackend for macOS                                             CW3
native/audio/mac/camera_mac.mm            AVFoundation discovery, preferred camera, permission, capture     CW3
native/audio/mac/scale_mac.mm             format choice, centre crop + scale to the cluster size (§4.11)    CW3
native/audio/mac/videoenc_mac.mm          VTCompressionSession low latency, LTR token map, refresh/IDR, rate CW3
native/audio/mac/synth_mac.mm             dev synthetic source (C1's synth.mm + code strip), --dev only     CW3
native/audio/mac/video_check.mm           CW3's local harness                                                CW3

native/audio/win/video_backend_win.{h,cpp} VideoBackend for Windows                                          CW4
native/audio/win/videodec_win.cpp         MF H.264 decoder: D3D11 + staging readback, software fallback      CW4
native/audio/win/vcam_win.cpp             camera create/start, install state, shared memory, demand, beat    CW4
native/audio/win/placeholder_win.cpp      DirectWrite/D2D placeholder renderer -> NV12 (§4.6.4)              CW4
native/audio/win/app_usage_win.cpp        which app uses the camera (CapabilityAccessManager), best effort  CW4
native/audio/win/vcam_install_win.cpp     verbs vcam-install / vcam-uninstall (elevated), vcam-remove (§9.2) CW4
native/audio/win/video_check.cpp          CW4's local harness                                                CW4

native/audio/vcam-win/                    the source DLL (like mac-driver/: its own binary)                  CW5
  shm_protocol.h                          shared-memory layout, names, CLSID (§4.6.2), verbatim              CW5 (first commit)
  source.cpp, source.def, placeholder_builtin.inc (baked "waiting for Beamer" image), build.ps1, README.md
  tools/camprobe.cpp, tools/feeder.cpp, tools/shmfuzz.cpp  ports of proto/win camprobe (+ C1 code-strip reader)
                                          and feeder; shmfuzz = hostile writer for §4.6.3

core/camera/__init__.py                   exports CameraPart, model types                                    CW1
core/camera/wire.py                       camera_* builders, validators, limits (pure)                       CW1
core/camera/model.py                      dataclasses (§7.2), merges, resolve (pure)                         CW1 (milestone 1)
core/camera/planner.py                    plan_camera(...) (pure, §6.4)                                      CW1
core/camera/store.py                      camera.json (atomic)                                               CW1
core/camera/part.py                       CameraPart + MediaContext protocol (§7.1)                          CW1
core/camera/fake_video.py                 video side of the fake helper (cameras, vcam, video streams)       CW1
core/camera/view.py                       pure Camera module model + map extras (§8.2)                       CW7 (milestone 1)
core/tests/test_camera_{wire,model,planner,part,cluster}.py                                                  CW1
core/tests/test_camera_view.py, core/tests/camera_fixtures.py                                                CW7
core/tests/test_camera_link_gating.py                                                                        CW6

mac_app/camera_section.py                 AppKit Camera module                                               CW7
mac_app/tests/test_camera_section.py                                                                         CW7
win_app/camera_section_win.py             Qt Camera module                                                   CW8
win_app/tests/test_camera_section_win.py                                                                     CW8
win_app/vcam_install.py                   install state, UAC launch of vcam-install/-uninstall (§9.2)       CW9
win_app/tests/test_vcam_install.py                                                                           CW9
dev/camera/e2e/                           two-machine harness (§10.5)                                        CW11 (cam_lipsync analysis: CW10)
```

Every new source file carries `SPDX-License-Identifier: GPL-3.0-only`. No new third-party code: AVFoundation,
VideoToolbox, Media Foundation, D3D11, Direct2D/DirectWrite and WIC are OS frameworks.

### 2.2 Hunks in existing files (the only shared surfaces)

| File | Hunk | Owner |
|---|---|---|
| `native/audio/engine/packet.{h,cpp}` | `PType` += `kNack=7, kPli=8, kTimeMap=9`; `PCodec` += `kCodecH264=2`; `ReplayWindow` gets a size template (`ReplayWindowN<128>` = today's, `<1024>` for video; `ReplayWindow` stays an alias) | CW2 |
| `native/audio/engine/flow.{h,cpp}` | dispatch `kNack`/`kPli` (video stream ids) to send streams, `kTimeMap` (stream 0, control channel) to recv streams; `attach_send/recv` accept video streams; **clock offset estimator** from the existing 24 B pong (§4.5.3); **feedback path** `send_feedback()`: sealed by the network thread, UDP always plus the link outbox in link state (§4.2, §4.10); time-map timer; video RR / NACK-retry / PLI drain in `tick`; flow-owned `Pacer`; **video outbox** `send_video()`: UDP when up, link only in link state, `link_out` events of one frame each with `"video":true,"frame":N,"end":bool` (§4.10) | CW2 |
| `native/audio/engine/streams.{h,cpp}` | audio `SendStream` publishes its latest (`ts`, capture `now_ns`) pair (atomics) for the time map when its `send` has `sr:true`; audio `RecvStream` publishes the `ts` of the sample being rendered + render latency (atomics) for §4.9. No change to audio seq, loss or RR | CW2 |
| `native/audio/engine/helper.{h,cpp}` | `run_helper(Backend&, VideoBackend*, argc, argv)` overload (the old signature forwards with `nullptr`); commands/events of §3; audio `send` accepts `sr` (bool); `ready.features` += video words | CW2 |
| `native/audio/engine/sources.txt` | every new engine `.cpp`, **including `null_video.cpp`** (as `null_backend.cpp`: `--video null:` works in any helper) | CW2 |
| `native/audio/engine/build-tests.{sh,cmd}`, `native/audio/tests/null_main.cpp`, `native/audio/tests/golden_control.txt` | add `video_tests.cpp` (and CW10's `sync_tests.cpp` later); `beamer-audio-null` builds the null video backend; golden control lines for the §3 additions | CW2 |
| `native/audio/mac/main.mm` | build the video backend, call the new `run_helper` | CW3 |
| `native/audio/build-mac.sh` | `MAC_BACKEND` += the new `.mm`; frameworks += `CoreMedia CoreVideo VideoToolbox` | CW3 |
| `native/audio/win/main.cpp` | `vcam-install` / `vcam-uninstall` / `vcam-remove` / `vcam-status` verbs before `run_helper`; build the video backend | CW4 |
| `native/audio/win/build.ps1` | `$backendSrc` += the new `.cpp`; libs += `mfplat mf mfuuid mfreadwrite d3d11 dxgi d2d1 dwrite windowscodecs advapi32 shlwapi`; call `vcam-win\build.ps1` | CW4 (DLL build script itself: CW5) |
| `core/audio/service.py` | construct `CameraPart` when the flag is on; route helper events `cameras`, `vcam`, `camera`, `permission.camera`, `stream_state`/`stats` of video stream ids, `link_out` with `video` to the part; route `camera_*` link messages, link up/down, helper ready/restart to the part; call `part.apply()` after the audio plan; implement `MediaContext` (§7.1); `_new_stream` also excludes `part.stream_ids()`; audio `send` gets `"sr": true` when the peer's `audio_hello.features` has `video1`; `_dev_command` passes every item to `part.dev_command(item)` and the dev status file gains `camera` = `part.dev_status()` (§7.4); `audio_hello.features` += `camera1` | CW6 |
| `core/audio/fake_helper.py` | delegate commands it does not know to `core.camera.fake_video` when present | CW1 |
| `core/tests/audio_cluster_harness.py` | optional camera parts per machine | CW1 |
| `core/audio/view.py` | `page_model(..., camera=None)`: when given a `CameraMapExtras` (§8.2), tiles carry `camera_line`/`has_active_camera`/`watching_line` and `arrows` get `kind="camera"` | CW7 |
| `tools/i18n_extract.py`, `core/tests/test_i18n_coverage.py` | `SOURCES` += `"core/camera"` (both scan with a non-recursive `glob("*.py")`, so `core/camera/view.py` strings are otherwise never extracted nor checked) | CW7 |
| `locales/beamer.pot`, `locales/it.po` | regenerated **only by the merge step** (§12 rules), never committed by a WP | merge step |
| `core/link.py` | `_CAP_FOR` += every `camera_*` type -> `"audio"` | CW6 |
| `core/receiver.py` | `_is_audio`/dispatch: `camera_*` handled like `audio_*` (same callback) | CW6 |
| `mac_app/bridge.py`, `win_app/sender.py`, `win_app/kvm_bridge_win.py` | forward `camera_*` like `audio_*` (message elif only; caps unchanged) | CW6 |
| `core/feature_flags.py` | `CAMERA = True` (fork) | CW6 |
| `WIRE.md` | section "Camera (inside capability `audio`, fork)" = §5 | CW6 |
| `mac_app/pages.py`, `win_app/pages_win.py` | title "Audio & Camera" and purpose when `CAMERA` (key `audio` unchanged) | CW7 / CW8 |
| `mac_app/audio_page.py` | insert the Camera module after the notices strip; map draws camera badge/arrow | CW7 |
| `win_app/audio_page_win.py` | same for Qt | CW8 |
| `mac_app/kvm_bridge_app.py` | menu-bar line, "Stop Sharing the Camera" / "Share the Camera", "Allow Camera Access…" (§8.5) | CW7 |
| `mac_app/setup.py` | `py_modules`: CW7 inserts `"camera_section",` immediately **before** `"audio_page",`; `includes` += `core.camera.*` (CW6); plist keys (CW9) | CW6/CW7/CW9 |
| `mac_app/entitlements.plist`, `entitlements-audio.plist`, `build_dmg.sh`, `Install-Beamer-Mac.command` | camera entitlement, TCC reset list | CW9 |
| `win_app/build_win_app.ps1`, `Install-Beamer.ps1`, `Beamer-Setup.iss`, `build_portable.spec` | ship `camera\BeamerCameraSource.dll` beside the helper (§9.2); Inno `[UninstallRun]` runs `beamer-audio.exe vcam-remove` before the existing maintenance entry | CW9 |
| `win_app/Install-Maintenance.ps1` | `-Remove`: delete the camera's HKLM keys and `%ProgramFiles%\Beamer\Camera` (§9.2); install/repair: retry deleting stale camera version folders | CW9 |

### 2.3 Why inside `beamer-audio`, and the one-backend split

- The video stream must share the flow's socket, keys, probing, hole punch, receiver reports and link
  fallback (C3 Q1), and audio must be scheduled ahead of video on that socket (C3 Q2): both only work in the
  process that owns the flow. A second process would need its own flow (C3: no gain, double machinery).
- TCC attribution is the same either way (Beamer.app is responsible for any child, C1 Q6); encoder crashes
  are already isolated in Apple's `VTEncoderXPCService`; on Windows a decoder crash kills only the helper
  (the source DLL then shows its placeholder, Beamer restarts the helper, A§8.2 watchdog).
- **Engine/backends split**: the engine owns everything that is policy and must be testable without a camera
  (numbering, packetization, loss tracking, clean runs, NACK/PLI/ack logic, pacing, playout timing, the hold ->
  reconnecting rule, rate ladder, lip sync). Backends own OS objects only: the Mac backend turns a `CameraSpec`
  into encoded AUs of exactly the requested size and obeys `request_ltr_refresh()` / `request_idr()` /
  `acknowledge_ltr()` / `set_bitrate()` / `set_input_paused()`; the Windows backend decodes the AUs it is given
  and shows what the engine says to show (a picture or a named placeholder). The null video backend implements
  both sides in the engine (§10.3).
- The helper stays one binary per OS; `backend.h` (A§2.4) is not touched.

### 2.4 Video backend interface (`native/audio/engine/video_backend.h`, verbatim contract for CW2/CW3/CW4)

```cpp
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace beamer::audio {
struct VideoFormat { int w = 0, h = 0, fps = 30; };
struct CameraInfo {
  std::string key, name, transport;          // transport: "builtin","usb","continuity","virtual","other"
  std::vector<VideoFormat> native;           // every native format at its max fps (<= 16 listed; diagnostic only)
  bool preferred = false;                    // macOS systemPreferredCamera
};
struct CameraSnapshot { std::vector<CameraInfo> cameras; std::string preferred;  // key, "" = none
                        std::string permission; };  // granted|denied|undetermined|restricted|unknown
struct EncodedFrame {                         // one Annex-B AU; valid only during on_frame
  const uint8_t* data = nullptr; size_t len = 0;
  uint64_t capture_ns = 0;                    // capture time on now_ns()'s clock (engine util.h)
  bool idr = false, refresh = false, ltr = false;  // refresh: references only acknowledged LTRs
  int w = 0, h = 0;                           // always the CameraSpec / set_format size
};
struct EncoderSink {                          // called on the backend's encoder output thread
  // Copy and return; never block > 1 ms. Returns the frame number the engine gave this AU
  // (0, 1, 2... per CameraHandle, one per call, never reused). The backend keeps it to map LTR tokens.
  virtual uint32_t on_frame(const EncodedFrame&) = 0;
  virtual void on_camera_event(const std::string& what, const std::string& detail) = 0;
      // what: "running","interrupted","resumed","lost","native" (capture format "WxH@fps"),"fps","effects","by_other"
  virtual ~EncoderSink() = default;
};
struct CameraSpec { std::string device;       // "" = system-preferred (followed)
                    VideoFormat fmt;          // output size: 1280x720 or 1920x1080, fps 30 (§4.11)
                    int bitrate = 2500000; bool ltr = true;
                    std::string test;  };     // --dev only: "synth" or "synth:WxH" = synthetic camera of that native size
class CameraHandle {                          // one capture tap + one encoder session; all calls thread-safe
 public:
  virtual ~CameraHandle() = default;
  virtual void start_encoding() = 0;          // no AU exists before this call; the first AU after it is an IDR
  virtual void request_idr() = 0;
  virtual void request_ltr_refresh() = 0;     // an IDR inside when no LTR is acknowledged
  virtual void acknowledge_ltr(uint32_t frame) = 0;  // that LTR frame is verified (§4.3); idempotent
  virtual void set_bitrate(int bps) = 0;
  virtual void set_fps(int fps) = 0;          // 30 or 15 (15 = every other captured frame)
  virtual void set_format(const VideoFormat&) = 0;   // new output size; the next AU is an IDR at that size
  virtual void set_input_paused(bool) = 0;    // captured frames are not given to the encoder while paused;
                                              // the reference chain is untouched (no AU is lost)
  virtual std::string device_in_use() const = 0;
};
struct VideoSinkListener {                    // called on the backend's thread
  virtual void on_decoded(uint32_t frame, bool ok, double decode_ms) = 0;
  virtual void on_sink_event(const std::string& what, const std::string& detail) = 0;
      // "decoder_sw","decoder_hw","device_lost","size_rejected" (SPS larger than kMaxW x kMaxH or odd)
  virtual ~VideoSinkListener() = default;
};
class VideoSinkHandle {                       // one decoder feeding the virtual camera; engine's video thread
 public:
  virtual ~VideoSinkHandle() = default;
  virtual void decode(const uint8_t* au, size_t len, uint32_t frame, bool keep) = 0;
      // keep = the picture may be shown (§4.5.2); !keep pictures are decoded and dropped
  virtual void present(uint32_t frame) = 0;   // make that kept picture the camera's current picture (live)
  virtual void hold() = 0;                    // keep showing the current picture (stall)
  virtual void drop(uint32_t frame) = 0;      // kept picture no longer needed (or revoked)
  virtual void show_placeholder(const std::string& state) = 0;
      // show the placeholder of that state ("starting","reconnecting") with the texts of vcam_placeholders();
      // the next present() goes back to live
};
struct Placeholder { std::string title, line; };
struct VcamState {
  std::string install;                        // ok|missing|outdated|broken|unsupported
  int dll_version = 0;                        // registered in HKLM, 0 = none
  int loaded_version = 0;                     // Header.dll_version of the running source, 0 = none seen
  bool present = false, in_use = false, creating = false;
  std::string app;                            // best effort, "" = unknown
  VideoFormat want;                           // the app's media type while in_use (validated, §4.6.3)
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
  // Texts per state ("off","waiting","waiting_permission","sharing_off","denied","error","starting",
  // "reconnecting") and the state shown while no sink is open.
  virtual void vcam_placeholders(const std::map<std::string, Placeholder>& texts, const std::string& idle) = 0;
  // Fails ("internal: sink open") while another sink exists: one virtual camera, one picture source.
  virtual std::unique_ptr<VideoSinkHandle> open_sink(VideoSinkListener*, std::string* err) = 0;
};
std::unique_ptr<VideoBackend> make_null_video_backend(const std::string& options);
class Backend;
int run_helper(Backend& backend, VideoBackend* video, int argc, char** argv);   // video may be null
}
```

Error strings follow A§2.4 (`"<code>: message"`); codes for video: `no_camera`, `permission`,
`not_installed`, `unsupported`, `decoder`, `internal`.

### 2.5 Build

- Mac: `build-mac.sh` unchanged in shape; the new `.mm` files are compiled per arch like the audio backend and
  linked with `-framework CoreMedia -framework CoreVideo -framework VideoToolbox` (AVFoundation is already
  linked). Deployment target stays 13.0 (`systemPreferredCamera` is 13.0+); macOS 14+ APIs (reaction effects,
  `AVCaptureDevice.reactionEffectsEnabled`, Studio Light / effects state reporting) are behind
  `@available(macOS 14, *)`, and the `effects` list is simply shorter on 13.
- Windows: `build.ps1` builds the helper as today plus `vcam-win\build.ps1` ->
  `native\audio\build\win\camera\BeamerCameraSource.dll` (`cl /std:c++20 /O2 /MT /W4 /utf-8 /LD`, `source.def`,
  links `mfplat mfuuid ole32 advapi32 windowscodecs`), and the tools `camprobe.exe`, `feeder.exe`,
  `shmfuzz.exe` in `native\audio\build\win\`. The dev tree therefore has the same `camera\` sub-folder as the
  shipped one (§9.2 lookup rule).
- Engine tests: `build-tests.sh` / `.cmd` add `video_tests.cpp`; `null_video.cpp` is in `sources.txt`, so
  every helper accepts `--video null:<opts>` and `beamer-audio-null` uses it by default.

## 3. Helper control protocol additions (stdio, A§3)

### 3.1 Process

- Command line additions: `--video null:<options>` (any helper; replaces the OS video backend, §10.3),
  `--no-video` (no video backend: the helper behaves exactly as today). Windows verbs without `--control`:
  `beamer-audio vcam-install --owner-sid <SID> [--dll <path>]` (elevated), `beamer-audio vcam-uninstall`
  (elevated), `beamer-audio vcam-remove` (unelevated: `Remove()` this user's camera), `beamer-audio
  vcam-status` (prints the `vcam` event once); exit 0 ok, 2 bad args, 3 not elevated, 4 failed. Without
  `--dll`, the DLL is looked up by the §9.2 rule.
- `ready.proto` stays 1 (only additions). `ready.features` gains `video1` (engine has §4), plus the backend's
  words: `camera_capture` (Mac), `virtual_camera` (Windows, independent of the install state).
- Threads added: per video send stream one **sender thread** (fragments, seals and hands AUs to the pacer; the
  only user of that stream's sender-direction `Sealer`), per flow one **pacer thread** created with the first
  video send stream (sends stored bytes, seals nothing); per video recv stream one **video thread**
  (reassembly hand-off, decode calls, presenter timer, heartbeat on Windows). The **network thread** parses,
  authenticates and enqueues as for audio, and is the **only thread that seals** receiver-direction video
  packets (RR, NACK, PLI) and the flow's `time_map` (§4.2 "Who seals").

### 3.2 Commands (Beamer -> helper)

| cmd | fields | effect |
|---|---|---|
| `send` | `kind:"video"`, `stream`, `flow`, `key`, `source` `{"camera": key|null}` (null = system-preferred, followed); with `--dev` also `{"test":"synth"}` or `{"test":"synth:1920x1440"}`; `codec` (§4.3: `{"name":"h264","profile":"constrained_high","w":1280,"h":720,"fps":30,"bitrate":2500000,"mtu":1200,"ltr":true}`); `link` `{"bitrate":1000000,"fps":15}` (optional, the link-state settings) | opens the camera + encoder at once; **no AU exists and no media is sent before the receiver's first PLI** (`reason` start), whose answer is the stream's first IDR (§4.7); `stream_state starting` -> `running` at the first sent AU |
| `send` (audio) | existing fields + `sr` (bool, default false) | `sr:true`: the stream's (`ts`, capture time) pairs go into the flow's `time_map` (§4.2). Beamer sets it only when the peer announced `video1`, so old peers never see a `time_map` |
| `recv` | `kind:"video"`, `stream`, `flow`, `key`, `codec`, `sink` `{"virtual_camera": true}` | opens a sink (fails while another sink is open), shows `starting`, sends PLI `start` per §4.5.6, then §4.5 |
| `video_config` | `stream` (a video send), any of `w`,`h`,`fps`,`bitrate` | live reconfiguration; a size change gives one paced IDR (the receiver needs no message: SPS carries the size) |
| `video_link` | `stream`, `paused` (bool, optional), `dropped` (list of frame numbers, optional) | Beamer's link outbox is full (§4.10): `paused` -> `set_input_paused` (capture keeps running, no AU lost); `dropped` -> the sender ignores NACKs for those frames and calls `request_ltr_refresh()` at once |
| `cameras` | - | emit `cameras` now |
| `permission_request` | `what:"camera"` | Mac: `requestAccessForMediaType:AVMediaTypeVideo` (the prompt names Beamer); answered `ok` at once, the result comes as a `permission` event; elsewhere error `unsupported`. Beamer sends it only for an action on the Mac (§9.1) |
| `vcam` | `enable` (bool, optional), `placeholders` `{"<state>":{"title","line"},...}` (optional), `idle` (state name, optional) | Windows: create/Start the camera if installed (async; `vcam` event `creating` while it takes up to ~5 s the first time), store the placeholder texts, set the state shown while no sink is open (§4.6.4); Mac: error `unsupported` |
| `dev` | with `--dev`, additions: `video_loss` `{"pct":0..100,"burst":mean packets}` (drop video at send: iid when burst 0, else Gilbert), `video_drop_ms` (drop all video for that long), `video_drop_au` (n: drop the next n AUs **after** numbering, tests §4.3 holes), `vcam_kill_beat` (stop the heartbeat, tests the DLL placeholder), `idr_pace_x` (2.0..6.0, default 3.0), `sync_offset_ms` (§4.9) | E2E and acceptance switches |

`stop` (A§3.2) also stops video streams; on a Mac video send it closes the encoder at once and the capture
session within 500 ms when no other stream uses that camera (camera light off).

### 3.3 Events (helper -> Beamer)

| ev | fields |
|---|---|
| `cameras` | `cameras` `[{key,name,transport,native:["1920x1080@30",...],preferred}]`, `preferred` (key or null), `permission`. After `ready` (Mac), on every change (debounced 300 ms), on `cameras` |
| `vcam` | `install` (`ok`/`missing`/`outdated`/`broken`/`unsupported`), `dll_version`, `loaded_version`, `bundled_version`, `present`, `creating`, `in_use`, `app` (string or null), `want` (`{"w","h","fps"}` or null), `error`. After `ready` (Windows) and on every change; `in_use` on at once, off after 300 ms without a stream |
| `camera` | `stream`, `state` (`starting`/`running`/`interrupted`/`lost`/`stopped`), `device` (key in use), `native` (capture format, `"1920x1440@30"`), `format` (sent, `"1280x720@30"`), `fps` (measured, last 1 s), `by_other` (another app on the Mac also uses the camera, when known), `effects` (list of active macOS effects: `center_stage`,`portrait`,`studio_light`,`background`,`reactions`,`edge_light`; macOS 14+) |
| `permission` | gains `camera` (`granted`/`denied`/`undetermined`/`restricted`/`unknown`); Mac re-reads an `undetermined` state every 2 s and at once after `permission_request` completes (A§3.3 rule) |
| `stream_state` | unchanged shape; `error` codes for video: `no_camera`, `permission`, `not_installed`, `decoder`, `internal` |
| `stats` | video send: `dir`,`kind:"video"`,`path`,`kbps`,`bitrate` (target), `fps`, `frames`, `idr`, `refresh`, `nack_rx`, `retx`, `pli_rx`, `q_drop` (AUs dropped after numbering), `paused_ms` (input paused), `pace_ms` (mean pacer wait), `enc_ms` (mean), `cam_fps`, `step` (rate-ladder level 0..2), `link_paused`, `rtt_ms`, `verified_ltr` (newest). Video recv: `dir`,`kind:"video"`,`path`,`kbps`,`fps_shown`,`frozen_ms` (display ticks without a new picture), `held_ms`, `late`, `lost` (frames, after repair), `holes` (frame-number holes), `nack_tx`, `retx_ok`, `pli_tx`, `target_ms` (D), `d_p99_ms`, `dec_ms`, `decoder` (`hw`/`sw`), `sync_ms` (lip-sync hold, §4.9), `offset_age_s`, `w`,`h`. Counters per interval as A§3.3 |
| `link_out` | unchanged for audio and for feedback (RR, NACK, PLI, time map: ordinary events, never marked `video`); video media packets come in separate events `{"ev":"link_out","flow":..,"p":[...],"video":true,"frame":N,"end":bool}`, each holding packets of one frame only (`end` on the frame's last event) |

### 3.4 Example session (PC app opens Beamer Camera; abbreviated)

```
PC  -> {"cmd":"vcam","placeholders":{"waiting":{"title":"Waiting for alexandres-macbook-air","line":"MacBook Air Camera"},"starting":{...},"reconnecting":{...},...},"idle":"waiting"}
PC  <- {"ev":"vcam","install":"ok","dll_version":1,"loaded_version":1,"bundled_version":1,"present":true,"creating":false,"in_use":true,"app":"Microsoft Teams","want":{"w":1280,"h":720,"fps":30},"error":null}
    (PC Beamer floods camera_devices; the Mac's planner wants ("video", PC): sharing on, permission granted)
Mac -> {"cmd":"send","req":7,"stream":1288,"flow":"9f2c01aa","kind":"video","source":{"camera":null},"codec":{"name":"h264","profile":"constrained_high","w":1280,"h":720,"fps":30,"bitrate":2500000,"mtu":1200,"ltr":true},"key":"..."}
Mac <- {"ev":"ok","req":7}
Mac <- {"ev":"stream_state","stream":1288,"state":"starting","device":"6C707041-05AC-0011-0004-000000000001"}
    (Mac Beamer posts camera_open; PC accepts: the Mac is the route's camera machine)
PC  -> {"cmd":"recv","req":12,"stream":1288,"flow":"9f2c01aa","kind":"video","codec":{...},"key":"...","sink":{"virtual_camera":true}}
    (PC engine shows "starting"; PC helper sends pli start; Mac encodes an IDR, paced)
Mac <- {"ev":"camera","stream":1288,"state":"running","device":"6C70...","native":"1280x720@30","format":"1280x720@30","fps":30.0,"by_other":false,"effects":[]}
Mac <- {"ev":"stream_state","stream":1288,"state":"running","device":"6C70..."}
PC  <- {"ev":"stream_state","stream":1288,"state":"running","device":"vcam"}
```

## 4. Media plane

### 4.1 Video media packet (C3 Q1, packet v1)

- Engine header (A§4.1) unchanged: `ptype=kMedia`, `codec=2` (H.264), `stream`, `seq` (per stream, +1 per
  packet sealed; a retransmission re-sends the identical stored bytes, so seq and nonce repeat with identical
  plaintext), `ts` = capture time in 90 kHz units since the stream's first captured frame, computed from
  `capture_ns` (wraps after 13 h; compare modulo 2^32), `nsamp=0`, `channels=0`, `flags` bit1 marker on the
  first packet of the stream, `reserved=0`.
- AEAD plaintext = **12 B video header** + fragment:
  `frame u32 | frag u16 | nfrag u16 | vflags u8 | ltr u8 | rsv u16` (big endian).
  `frame`: the §1 frame number (assigned in `on_frame`, the same number everywhere). `vflags`: bit0 IDR, bit1
  refresh, bit2 this AU is an LTR, bits 3-7 0. `ltr`: 1 when the AU is an LTR, else 0 (VideoToolbox tokens
  never travel). Fragment: <= 1148 B of the AU, cut at any byte; `nfrag` <= 1024 (a 1080p IDR is ~225). UDP
  payload <= 1200 B (Tailscale MTU 1280).
- Sealed with the stream key, sender direction (A§4.1 nonce rule), by the stream's sender thread only.
  Replay window per video recv stream: **1024** packets (`ReplayWindowN<1024>`, 16 x u64).

### 4.2 Feedback and timing packets

`nack`, `pli` and video `rr` are sealed with the stream key in the **receiver direction** (`stream | kDirBit`,
its own seq space and window, like `kRR` today). The `time_map` is a **flow control packet** (`stream` 0,
the flow key and control direction, the existing `ctrl_sealer_`/`ctrl_window_`, like hello/ping/pong): it
never touches a stream's seq space, so it cannot look like a media gap or count as audio loss.

**Path**: every packet of this section goes by UDP; **in link state it is also queued to the flow's (non-video)
link outbox** (as `send_media` already does for audio), so feedback keeps flowing during the TCP fallback
(§4.10). The duplicate that arrives second is rejected as a replay (harmless). Never via the link while the
flow is `up` (a `link_in` would push the peer to link state, `peer_link_ns`).

**Who seals**: the network thread only. NACKs are built and sealed in the same network-thread pass that
detects the gap; NACK retries, the RR timer and the time-map timer run in `Flow::tick` (network thread); PLIs
raised by the video thread (deadline expiry, start/size repeats) or the backend thread (`on_decoded(ok=false)`)
are posted to the recv stream's feedback mailbox (mutex + flags) and sealed by the next `tick` (<= 2 ms). No
other thread touches a receiver-direction `Sealer`.

| ptype | payload | sent |
|---|---|---|
| `kNack` 7 | `n u8` (1..32), then `n` x (`seq_base u64`, `mask u16`: bit i = seq_base+1+i also missing) | at once when a gap appears (a packet arrives with seq > highest+1: reorder was 0 in every C3 run); repeated after max(2 x RTT, 15 ms), at most 3 times per seq, only while that frame's present time has not passed and the hole is <= 64 packets. Also in link state (§4.10) |
| `kPli` 8 | `last_clean u32` (newest clean frame, or 0xFFFFFFFF = none), `reason u8` (0 loss, 1 start, 2 decoder error, 3 stalled, 4 size), `rsv u8` | §4.5.6 |
| `kTimeMap` 9 (flow control, stream 0) | `n u8` (1..32), then `n` x (`stream u32`, `ts u32`, `sender_ns u64` = `now_ns` at the capture of that `ts`) | every 1 s per flow, and in the first `tick` after a covered stream sent its first media packet; covers every video send stream and every audio send stream with `sr:true` on that flow; nothing is sent when it would be empty (so a flow to an old peer never carries one). Lets the receiver map any `ts` to the sender's clock (§4.5.3, §4.9). For video the mapping is exact for the stream's life (ts is derived from `capture_ns`); for audio (device clock) it is refreshed every second |
| `kRR` 5 (video) | 28 B: `newest_ts u32`, `margin u16` (min playout margin over the interval, 0.1 ms, 0 = late), `hi_seq_lo u32`, `lost u32` (frames lost after repair, cumulative), `late u16` (cumulative), `clean_since u32` (first frame of the newest clean run, 0xFFFFFFFF = none yet), `acked u32` (newest frame of that run), `recv_kbps u16`, `flags u8` (bit0 broken now), `rsv u8` | every 50 ms per video recv stream **regardless of what this side sends** (`Flow::send_rr` keeps its 10 ms audio rule; video has its own 50 ms timer), plus at once when a refresh/IDR starts a new clean run |

Audio RR stays 6 B; a sender tells the two apart by the stream kind it knows. Old helpers drop `kNack`/`kPli`
(unknown stream) and never receive a `time_map` (gated by `sr` / `video1`).

### 4.3 Sender (`VideoSendStream`, engine)

- `codec` object (wire and `send`): `name` `"h264"`, `profile` `"constrained_high"` (C1: Baseline only if a
  decoder needs it; the PC's does not), `w`,`h` (1280x720 or 1920x1080, the cluster quality), `fps` 30,
  `bitrate` (2,500,000 at 720p, 5,000,000 at 1080p), `mtu` 1200, `ltr` true.
- **Numbering and queue.** `on_frame` (encoder thread) gives the AU the next frame number, copies it into the
  stream's frame queue and returns the number. The sender thread fragments, seals, stores each sealed packet in
  the **history** (1 s or 2048 packets, whichever is less; ~625 KB at 5 Mbit/s) and hands them to the flow's
  pacer (or, in link state, to the video outbox, §4.10).
- **Backpressure keeps the reference chain intact.** While the stream's backlog (AUs queued + packets waiting
  in the pacer) exceeds 250 ms at the current release rate, or the frame queue holds >= 6 AUs, the engine calls
  `set_input_paused(true)`: the backend stops feeding captured frames to the encoder, so no AU is created and
  none is lost; `false` again below 100 ms / 2 AUs. (An IDR paced at 3x is ~130 ms of backlog, so a normal
  start does not pause.) `video_link paused` uses the same switch (both reasons ORed).
- **Hard cap (defence, counted `q_drop`).** If the frame queue still reaches 16 AUs or 4 MB, the **incoming AU
  is dropped after numbering** (if it is an IDR, every queued non-IDR AU is dropped instead: they precede the
  IDR that restarts the chain). The numbers stay assigned, so the receiver sees a hole and goes broken; the
  sender calls `request_ltr_refresh()` at once instead of waiting for the PLI. The dropped frames never appear
  in a clean run, so no LTR at or after them is ever acknowledged.
- `kNack` from the network thread -> look up each seq in the history -> retransmit the stored bytes through the
  pacer (or the video outbox) **ahead of new data**; seqs no longer in history, or of frames dropped after
  numbering, are ignored (the receiver's PLI follows).
- **`kPli` -> recovery** (C1 + C3). Two classes:
  1. `start` (1) and `size` (4) -> `request_idr()`. **Ignored as a duplicate** while an IDR is queued or being
     paced, or when the last IDR's first packet left less than RTT + its pace time + 50 ms ago. Otherwise at
     most **one per 1 s**: a request inside that window is served when it ends. Never converted to a refresh
     (a fresh or reopened decoder has no SPS/PPS and no LTR).
  2. `loss` (0), `decoder` (2), `stalled` (3) -> `request_ltr_refresh()` when a verified LTR exists, else
     `request_idr()`. A PLI whose `last_clean` is unchanged within RTT + 50 ms of the last refresh is a
     duplicate and ignored. If 3 refreshes in a row did not produce a newer clean run (RR) within 1 s each ->
     `request_idr()`. **IDRs of this class: at most one per 2 s**; a request inside that window becomes a
     refresh when a verified LTR exists, else waits for the window's end.
- **Acks from clean runs.** The sender keeps the frame numbers of the LTR AUs it sent (last 64). Each video RR
  gives a clean run `[clean_since, acked]` in which the receiver decoded every frame from complete data and a
  complete reference chain; every sent LTR inside it becomes **verified** (a frame's cleanliness never changes
  once decoded, so the union over all RRs is valid; lost or reordered RRs only delay it) and is passed once to
  `acknowledge_ltr(frame)`. The Mac backend maps frame numbers to VideoToolbox
  `RequireLTRAcknowledgementToken` values (from the `on_frame` return value) and sends every verified token in
  `kVTEncodeFrameOptionKey_AcknowledgedLTRTokens` (C1). LTRs decoded during a broken period, or after a hole,
  are never inside a clean run and so never acknowledged, whether or not any PLI arrived.
- Rate: `set_bitrate` from the safety ladder (§4.10) and from `video_config`.
- Before the first PLI the backend produces nothing: `start_encoding()` has not been called; the camera and the
  encoder session are warm, no AU exists, no number is used.

### 4.4 Pacer (per flow, `pacer.{h,cpp}`)

- Video media packets only (new data and retransmissions); audio media, RR, hello, ping, `time_map`, `nack`,
  `pli` bypass it (sent at once by the network thread or their streams' threads, as today).
- Token bucket: capacity **32 packets**, refill **25 Mbit/s** for P frames, refreshes and retransmissions
  (effectively unpaced: they are 9-38 packets, C3). Packets of an **IDR** are released at
  `idr_pace_x` x the stream's target bitrate (default **3.0**, `dev` 2.0-6.0): 120 KB at 7.5 Mbit/s = 128 ms,
  256 KB at 15 Mbit/s = 137 ms. Frames queued behind an IDR wait (C3: ~100 ms once per stream start).
- A dedicated pacer thread waits on a condition variable until the next token (the 2 ms `NetLoop` poll is too
  coarse at 25 Mbit/s). Sends go through `Flow::send_video` (UDP when up; in link state see §4.10). The pacer
  only sends stored bytes; it never seals.

### 4.5 Receiver (`VideoRecvStream`, engine)

1. **Reassembly** per frame number (up to 64 frames in flight; an AU completes when all `nfrag` fragments are
   in). Gaps in `seq` -> NACK (§4.2).
2. **Clean/broken** (the only correctness signal: C2 Q5, corrupt pictures decode silently).
   - A frame number is **lost** when its present time passes while it is incomplete, when NACK gives up on one
     of its packets (3 tries or hole > 64), when it is a hole (a newer frame completed and this number never
     got a packet before its present time), or, in link state, as soon as a newer frame completes (the link
     delivers in order).
   - The stream starts broken. Frame `f` is **clean** iff it is complete **and** (a) it is an IDR, or (b) it is
     a refresh (it references only verified LTRs, which by §4.3 are frames this receiver itself reported
     clean), or (c) frame `f-1` is clean and decoded ok. A lost frame, or a decoder error on any frame, makes
     the stream broken; nothing but a complete IDR or refresh makes it clean again.
   - Every complete AU is passed to the decoder in frame order (`decode(keep = clean)`), including AUs of a
     broken period, so the decoder's reference state follows the encoder's (C2: recovery is bit-exact from
     the refresh on).
   - `on_decoded(f, ok=false)` revokes `f` and every later kept picture: the engine calls `drop()` for them,
     never presents them, goes broken and sends PLI reason 2. The clean run in the RR (`acked`) only advances
     over frames already reported `on_decoded(ok)`.
3. **Playout deadline.** For each frame, `d` = completion time (receiver clock) - capture time mapped to the
   receiver clock: `sender_ns` from the flow's `time_map` for that `ts`, minus the flow's **clock offset**.
   - **Clock offset** (new, in `Flow`): each pong already carries `t0` (our send), `t1` (peer receive), `t2`
     (peer send) and we know `t3` (our receive): `offset = ((t1 - t0) + (t2 - t3)) / 2` (peer clock minus
     ours). The flow keeps the sample with the **lowest RTT among the last 10 pongs** (10 s at the 1 s ping
     period) and exports it atomically with its age. Pings are UDP-only, so in link state the last estimate is
     kept: drift is 19-25 ppm (C3), i.e. <= 1.5 ms per minute, which the p99 tracking below absorbs. If a stream
     completes its first frame before any pong (flow already in link state), a provisional offset is taken
     from that frame (`completion - sender_ns - 20 ms`) and replaced by the first pong estimate.
   - Target `D` = p99 of `d` over the last 10 s (IDR frames excluded: paced on purpose) + 8 ms, clamped **[50,
     150] ms**, rises at once, falls by at most 15 ms per 0.5 s (C3 Q4: ~70 ms here). Present time = mapped
     capture time + D + lip-sync hold (§4.9).
4. **Presenter** (video thread): presents each kept picture at its present time; a picture completed after its
   present time + one frame period is **late**: decoded, never shown, unless it is the newest (then shown at
   once: after a stall the display jumps to the newest frame). Backlog older than **D + hold + 150 ms** is
   decoded without being kept. When nothing new is presentable, the engine calls `hold()`; after **2 s** of
   hold it calls `show_placeholder("reconnecting")` (Deviation 1); the next clean picture is `present()`ed and
   the camera is live again. From `recv` until the first clean picture the engine shows `starting`.
5. **RR** every 50 ms (§4.2): `clean_since`/`acked` = the newest clean run, `acked` advancing only over frames
   decoded ok.
6. **PLI** (sealed by the network thread, §4.2):
   - `start`: sent at `recv` and every 100 ms **until the first packet of an IDR whose frame number is newer
     than the newest frame seen when the request began** (none, at stream start). Same rule for `size` (the
     decoder rejected the SPS size: odd, or larger than `kMaxW x kMaxH`; `on_sink_event("size_rejected")`).
   - `loss` / `stalled` / `decoder`: when the stream becomes broken (lost frame; `stalled` when the frames were
     lost because no packet at all arrived for >= 500 ms, a Wi-Fi stall, handled like `loss`; decoder error),
     then every 300 ms while broken.
   - 3 decoder errors within 10 s -> the sink reopens its decoder (software if the GPU path failed, §4.6.1) and
     the stream sends PLI `start` (rule above): a reopened decoder always gets an IDR.

### 4.6 Windows sink

#### 4.6.1 Decode (`videodec_win.cpp`)

`CLSID_CMSH264DecoderMFT`, `MF_LOW_LATENCY` + **`CODECAPI_AVLowLatencyMode` = TRUE** (mandatory, C2 Q5), NV12
out, D3D11 device manager (`MFT_MESSAGE_SET_D3D_MANAGER`) with an own staging-texture readback (C2: the fastest
path in the user session, 1.8 / 2.8 ms). Software fallback when no D3D11 video device, on
`DXGI_ERROR_DEVICE_REMOVED`/`RESET` (then retry hardware after 30 s), or with `--dev` `decoder=sw`. Size changes
via `MF_E_TRANSFORM_STREAM_CHANGE` (new output type, no message needed); an output type with odd sizes or larger
than `kMaxW x kMaxH` is refused (`size_rejected`, nothing written to a slot). Kept pictures go to a pool of
**12 NV12 buffers** at the stream size: (D 150 + hold 150 ms) / 33 ms + 2 = 12, enough for the worst present
delay; when the pool is exhausted the oldest unpresented kept picture is dropped (counted `late`). `present`
copies the picture into the next live slot (0.07-0.13 ms, C2).

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
// DACL on all three: SYSTEM, LocalService, OwnerSid (full); nobody else. Creators check ownership (§4.6.3).
constexpr uint32_t kMagic = 0x4D414342;   // 'BCAM'
constexpr uint32_t kVersion = 1;          // bump only on a layout change; the helper supports [1, its max]
constexpr int kDllVersion = 1;            // bump on any DLL change that needs a reinstall
constexpr uint32_t kMinW = 64, kMinH = 64, kMaxW = 1920, kMaxH = 1080;
constexpr uint32_t kSlotBytes = 3112960;  // 1920*1080*3/2 rounded up to 4096
constexpr uint32_t kLiveSlots = 3;        // + 1 placeholder slot (index 3)
constexpr uint32_t kSlotCount = 4;
constexpr uint64_t kDataOffset = 4096;    // slot i's pixels: kDataOffset + i * kSlotBytes (fixed, never stored)
constexpr uint64_t kSectionBytes = kDataOffset + uint64_t(kSlotCount) * kSlotBytes;
enum Mode : uint32_t { kModeNone = 0, kModeLive = 1, kModeHold = 2, kModeWaiting = 3 };
struct alignas(64) Slot {                 // written by the helper
  std::atomic<uint32_t> seq;              // seqlock: odd while the helper writes
  uint32_t w, h, stride;                  // NV12: Y at the slot's pixels, UV at + stride*h
  uint64_t written_qpc;
  uint32_t picture;                       // helper's picture counter
  uint32_t rsv[9];
};
struct alignas(64) Header {
  uint32_t magic, version, header_bytes, slot_count;   // written by the source at creation; never read back by it
  // written by the source
  std::atomic<uint32_t> state_seq;        // +1 on every change below; the source then sets the Demand event
  uint32_t streaming;                     // 1 between Start and Stop/Shutdown (repeated stops are harmless)
  uint32_t want_w, want_h, want_fps;      // the app's media type
  uint64_t state_qpc;
  uint32_t dll_version, source_pid;       // dll_version = the running DLL's kDllVersion (= loaded_version)
  std::atomic<uint64_t> delivered, placeholders, repeats, rejected;   // rejected: helper data refused (§4.6.3)
  // written by the helper (hostile input for the source, §4.6.3)
  uint32_t helper_pid;                    // informational only
  std::atomic<uint64_t> beat_qpc;         // written by the thread that presents (§4.6.4)
  std::atomic<uint32_t> mode;             // Mode
  std::atomic<uint32_t> latest;           // live slot with the newest picture
  uint32_t rsv[16];
  Slot slots[kSlotCount];
};
static_assert(sizeof(Header) <= kDataOffset, "header must fit before the pixels");
}
```

Rules: the source creates section (`kSectionBytes`) and frame event at activation (C2 Q4: only LocalService
may create `Global\` sections) and keeps them while any source object lives; the helper opens them when the
Demand event fires or by polling every 1 s, maps exactly `kSectionBytes`, writes `helper_pid`, beats (§4.6.4),
writes a live picture into slot `(latest+1) % 3` under the seqlock, publishes `latest`, sets the frame event;
the source copies the newest slot per §4.6.3 and scales nearest-neighbour when the slot size differs from the
app's type (C2: +0.8 ms down, +3 ms up). The helper rejects a header whose `magic`/`version`/`slot_count` it
does not know (`vcam` `install: outdated`).

#### 4.6.3 Source DLL (`BeamerCameraSource.dll`)

As C2's prototype (`proto/win/source.cpp`), cleaned: activate -> `IMFMediaSourceEx` + `IMFGetService` +
`IKsControl` -> one `IMFMediaStream2`, `MF_DEVICESTREAM_FRAMESERVER_SHARED = 1`, NV12 **1280x720, 1920x1080,
640x360 @ 30** (640x480/4:3 only if an app needs it); 30 fps delivery clock; no network, no decode, no crypto;
reads `InstallGuid` and `OwnerSid` from HKLM to name and protect the objects; logs at most 1 line/s to
`%ProgramData%\Beamer\Camera\source.log` (rotated at 1 MB; the folder is created by `vcam-install` with write
access for SYSTEM, LocalService and Administrators only, Users read). Size of the DLL's own code ~1 kloc.

**Only inside Frame Server.** `MFCreateVirtualCamera` also loads the DLL once in the calling process (the
helper) to validate it (C2 Q1). At activation the source checks its process token: unless the user is
LocalService (`S-1-5-19`), it is an inert source (valid media type list, no kernel objects, no log, no
registry writes), so validation passes and nothing collides with the Frame Server instance.

**Kernel object ownership.** The source creates the section and the frame event with an explicit DACL; on
`ERROR_ALREADY_EXISTS` it checks the object's owner (`GetSecurityInfo`) is LocalService or SYSTEM, else it logs
`squatted`, refuses to use it and shows the built-in image (events need no privilege, so another session's
process could pre-create `.Frame`). The helper creates `.Demand` with `CreateEventExW` and the same DACL; on
`ERROR_ALREADY_EXISTS` it requires the owner to be its own user SID, else `vcam` `error: "squatted"` and it
falls back to polling every 1 s. Both sides open objects created by the other with the minimum access they
need (`FILE_MAP_WRITE` on the section for the helper; `EVENT_MODIFY_STATE` for the events).

**Hostile-input rule: everything the helper side writes is untrusted.** Any process of the owner's account can
open the section (the DACL grants the owner SID), and the source runs in the shared Frame Server service, so a
bad value must never make it read outside its own memory, crash, or leak another buffer. On every delivery:
1. Geometry comes from constants only: the view is `kSectionBytes`, slot `i`'s pixels are at
   `kDataOffset + i * kSlotBytes`; `magic`/`version`/`header_bytes`/`slot_count` are never read back.
2. Each helper-written field is read **once** into a local (atomic or volatile load) and validated before use:
   `mode` in `{0,1,2,3}`, else treated as none; `latest < kLiveSlots`, else none; `beat_qpc` within
   `[now - 250 ms, now + 1 s]`, else stale; `helper_pid` is never used for anything but the log.
3. Slot read under the seqlock: `s1 = seq` must be even; copy `w`, `h`, `stride` to locals and require `w`, `h`
   even, `kMinW <= w <= kMaxW`, `kMinH <= h <= kMaxH`, `w <= stride <= 4096`, and `stride * h * 3 / 2 <=
   kSlotBytes` (64-bit arithmetic); copy `stride * h * 3 / 2` bytes into a **source-private buffer**; re-read
   `seq` == `s1`. Two failed attempts -> repeat the previous good picture.
4. Scaling and conversion read only the private buffer (no time-of-check/time-of-use on shared memory).
5. Any failed check: built-in image (or the previous good picture for a torn seqlock), `rejected` += 1, one log
   line per second at most.

The helper applies the same rule in the other direction to the source-written fields it uses (`want_*` must be
one of the offered types, else 1280x720; `streaming` is 0/1; `state_seq` only compared for change).

#### 4.6.4 What the camera shows (who decides)

| Condition (checked by the source on every delivery) | Picture |
|---|---|
| no section writer, `beat_qpc` older than **250 ms** or invalid (Beamer not running, helper dead or stuck), or a refused object (§4.6.3) | built-in image (`placeholder_builtin.inc`, Beamer dark surface, logo, "Beamer Camera", "Waiting for Beamer") + a small moving dot |
| `mode == Waiting` | slot 3 (the helper's placeholder) + moving dot |
| `mode == Live` or `Hold` | the newest valid live slot, repeated until a newer one |

**Heartbeat = liveness of the presenting thread.** `beat_qpc` is written by the thread that owns the picture,
in the same loop iteration that presents or re-presents it: the sink's presenter (video thread) while a sink is
open, the vcam idle loop otherwise; never by a separate timer. A wedged engine therefore turns into the
built-in image within 250 ms, well before the Python watchdog restarts the helper.

The helper (not the DLL) decides between live, hold and waiting, driven by the engine (§4.5.4):
- **waiting** (slot 3) whenever no sink is open, with the `idle` state's texts from `vcam` (`off`, `waiting`,
  `waiting_permission`, `sharing_off`, `denied`, `error`, chosen by Python, §6.4), and at once (well under 250
  ms) when the sink closes;
- **starting** (slot 3) from `open_sink` until the first clean picture;
- **live** from the first clean picture; **hold** while a running stream has nothing new to present; after
  **2 s** of hold the engine calls `show_placeholder("reconnecting")` -> waiting with the `reconnecting` texts;
  back to live on the next clean picture.

The helper renders slot 3 itself (Direct2D + DirectWrite into BGRA, converted to NV12, 1280x720 and
1920x1080 rendered on demand at the app's size): Beamer's dark surface and accent colours (`win_app/tokens`
dark theme values copied as constants), the Beamer mark, a title line (24 px at 720p) and a detail line
(16 px). Texts (§8.4) come from Python through `vcam placeholders`; the helper never invents wording except the
built-in image. A state without texts renders the title "Beamer Camera" and no detail line.

#### 4.6.5 Camera creation

At helper start, when `install == ok`: `MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
MFVirtualCameraLifetime_System, MFVirtualCameraAccess_CurrentUser, L"Beamer Camera", clsid, nullptr, 0)` +
`Start(nullptr)` on a worker thread (5.1 s the first time ever, 28 ms after, C2). The camera persists when
Beamer exits (apps keep listing it and show the built-in placeholder). `vcam enable:false` is never sent in
normal operation; `vcam-remove` (unelevated) removes it (`Remove()`), `vcam-uninstall` also unregisters the DLL
(§9.2).

#### 4.6.6 Who uses the camera (`app_usage_win.cpp`, best effort)

`HKCU\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\webcam` (packaged apps
by package family name, `NonPackaged\<path with #>` for desktop apps): entries with `LastUsedTimeStart` >
`LastUsedTimeStop` are in use. Read when `in_use` turns on and every 2 s while on. `app` = the display name
(package display name, else the exe's `FileDescription`, else the file name) **only when exactly one app is in
use**, else null. It covers any camera: on a PC with a physical camera too, `app` is only reported while
Beamer Camera itself is streaming, and the UI never words it "probably"; it shows the name or nothing. It is
never used to infer that a second app was refused (the source cannot see refused apps, C2 Q6).

### 4.7 On-demand start (sequence and budget)

1. App on T opens Beamer Camera -> source `Start` -> Demand event -> T's helper `vcam in_use:true want` (< 1 ms,
   C2) -> T's part republishes `camera_devices` (flood, direct link); the camera shows T's `idle` placeholder
   (e.g. `waiting`) until a sink opens.
2. S's part receives it -> planner wants `("video", T)` (sharing on, permission granted, §6.4) -> **at the same
   time**: helper `send` (camera + encoder start in parallel: encoder session ~150 ms, C1) and `camera_open`
   to T.
3. T accepts (S is T's route camera machine, §6.4 rule 5) -> helper `recv` (the engine shows `starting`) ->
   `camera_opened ok`; T's recv stream sends PLI `start` every 100 ms until an IDR's first packet arrives.
4. S's first PLI: `start_encoding()` -> next captured frame -> IDR (37 / 56 ms, C1) -> paced ~130 ms -> T
   reassembles, decodes (2-3 ms), presents at D, writes the slot -> the app's next sample.

Budget: Python/link hops ~10-30 ms; camera start **[C4 / C1 real camera]**; IDR encode 37-56 ms; IDR pacing
~130 ms; D 50-70 ms; decode + slot 3-4 ms. **Target: first real frame <= 1.0 s after the app opened the camera
(p95 <= 1.5 s)**, to be confirmed by **[C4]**; the placeholder covers the gap.

Stop: the app closes -> `in_use:false` after 300 ms -> T republishes -> S's planner holds the demand **3 s**
(apps reopen the camera when they switch resolution or open a settings preview) -> `camera_close` + helper
`stop` -> camera light off <= 0.5 s later. Camera on for at most **~4 s** (300 ms + 3 s + link + 500 ms) after
the last app closed it. Turning sharing off on S, or the route moving away, stops it without the hold.

### 4.8 Latency budget (glass to glass, 720p)

| Term | Value | Source |
|---|---|---|
| Camera exposure + delivery to the helper | **[C1 real camera / C4]** | |
| Crop/scale (only for a camera without the exact size) | ~1 ms (VT pixel transfer, GPU) | estimate, CW3 measures |
| Encode | ~5 ms | C1 |
| Pacing (P frames) | ~0 (bucket) | C3 |
| Playout deadline D (from capture, includes network + jitter allowance) | 50-70 ms | C3 |
| Decode + slot + source delivery | 2.4 ms (720p) / 4 ms (1080p) | C2 |
| App pipeline | Chrome 2-3 ms, DirectShow apps ~33 ms | C2 |
| **Total (excluding camera)** | **~60-80 ms** for MF/browser apps; **[C4]** measured | |

### 4.9 Lip sync

- Data now (CW2): the flow's `time_map` covers every video stream and every audio stream sent with `sr:true`
  (§4.2); each recv stream maps its `ts` to the sender clock. The audio recv stream publishes
  `audio_present_delay` = (now - sender capture time of the sample being rendered, mapped with the flow offset)
  + the render path latency (WASAPI period x queue; for CABLE the measured constant of A WP12: shared ~65 ms,
  exclusive ~30 ms beyond the render buffer, configured per mode); the video recv stream publishes its own
  `video_present_delay` = D + decode. Both streams are on the same flow (same sender, same clock, same offset
  estimate), so the offset and its error **cancel in the difference**: lip sync does not depend on the offset's
  accuracy, only on both streams using the same value.
- Policy (CW10): on a machine T that receives **both** the active mic stream from S (into its virtual mic) and
  a video stream from S, the video presenter adds `hold = clamp(audio_present_delay - video_present_delay,
  0, 150 ms)`, smoothed (EWMA 2 s, changes <= 10 ms per second), applied only after both streams have run 2 s.
  Audio is **never** delayed for video (calls are judged by audio delay); if video is the slower path (audio
  leads), nothing is done, and the stats show the negative skew. With today's numbers (Mac mic -> PC app ~110
  ms low-latency / ~170 ms shared, video ~70-90 ms) the hold is ~20-100 ms. The mic stream carries the time
  map only because the PC peer announces `video1` (Beamer sets `sr`).
- Stats: `sync_ms` (current hold) on the video recv stream. Dev override `dev {"sync_offset_ms": n}` for
  measurement.
- Out of the engine's view: an app's own A/V buffering (e.g. Teams). Not compensated.

### 4.10 Rate safety and TCP fallback

- **Safety step-down** (sender, from video RR): over the last 2 s, post-repair `lost` > 2 % of frames or
  `margin` = 0 (late) for > 10 % of frames -> next level: base, base/2, base/4 (720p: 2.5 -> 1.25 -> 0.65
  Mbit/s; 1080p: 5 -> 2.5 -> 1.25). Step up one level after 10 s clean. No size change, no IDR.
- **Link state** (flow `link`, A§4.5):
  - **Media**: the video sender goes to `link.bitrate` (1 Mbit/s) and `link.fps` (15) at the current size and
    **sends video only through the video outbox -> `link_out`** (no UDP copy; hellos still probe UDP). Each
    `link_out` video event holds packets of exactly one frame (<= 4 packets, `MAX_FRAME_PACKETS`), tagged
    `frame` and `end` (§3.3).
  - **Feedback**: RR, NACK, PLI and the time map travel over the link too (§4.2 path rule), in ordinary
    `link_out` events (not `video`), so they obey only the audio 64-message rule, never the video 32 rule. LTR
    acknowledgements therefore keep working and a PLI still ends as a 12-38 KB refresh, not an IDR.
  - **NACK** stays on: the receiver's loss logic does not change with the path. TCP itself does not lose; the
    holes on the link are frames Beamer dropped (below), whose packets the sender does not retransmit (it was
    told by `video_link`), so those NACKs go unanswered and the PLI -> refresh follows; a packet lost at the
    UDP -> link transition is repaired normally.
  - **Beamer's drop rule** (`CameraPart.link_out`): while the peer's link outbox holds > **32** messages (audio
    keeps its 64), a video event is dropped, **and every later event of the same frame number is dropped too**
    (already posted events of that frame cannot be withdrawn, and a partial frame is useless); the part tells
    the engine `video_link {"stream", "dropped": [frames]}` (the sender ignores NACKs for them and requests a
    refresh at once). If the outbox stayed > 32 for 1 s it sends `video_link paused:true` (input paused; T
    holds, then shows "Reconnecting"); `paused:false` when the outbox is < 8 for 2 s or the flow is back `up`.
  - Back in `up`: base bitrate/fps, UDP; feedback by UDP only.
- **SSH tunnel / loopback link** (no UDP host, A§4.2): no video stream is opened; status `no_direct_path`.

### 4.11 Mac capture and encode (C1 recommendations, binding for CW3)

- **Output size is always the cluster quality** (`CameraSpec.fmt` / `set_format`: 1280x720 or 1920x1080,
  16:9), whatever the camera; the PC source scales to each app's media type. One quality, one encoded size for
  the whole cluster.
- **Capture format choice** (per camera, for the target `Wt x Ht`), among `420v`/`420f` formats whose max frame
  rate is >= 30 (if none, the highest-rate ones):
  1. a format of exactly `Wt x Ht` (no conversion: C1's "pick the format, do not scale in AVFoundation");
  2. else, among landscape formats (`W >= H`), the one with the smallest area whose 16:9 centre crop (below)
     is >= `Wt x Ht`;
  3. else the landscape format with the largest 16:9 crop;
  4. else (portrait or square formats only) the format with the largest 16:9 crop.
- **Crop rule (non-16:9 or other-size cameras)**: the largest centred 16:9 rectangle: `Wc = min(W, floor(H *
  16 / 9))`, `Hc = floor(Wc * 9 / 16)`, both rounded down to even; origin `((W - Wc) / 2, (H - Hc) / 2)` rounded
  down to even. Then scale `Wc x Hc` to `Wt x Ht` (down or up). Never letterbox, never stretch. Examples:
  1920x1440 -> crop 1920x1080 at y = 180; 1760x1328 -> 1760x990 at y = 168, scaled to 1280x720; 1552x1552 ->
  1552x872 at y = 340; 1080x1920 (portrait) -> 1080x606 at y = 656; 640x480 -> 640x360 at y = 60, upscaled.
- **Scaler**: one `VTPixelTransferSession` per camera device (NV12 video range in and out, source crop rectangle
  as above, `kVTScalingMode_Trim` semantics), only when the chosen format is not exactly `Wt x Ht`; its output
  `CVPixelBuffer` (from a `CVPixelBufferPool`) goes to the encoder(s). The `camera` event reports `native` and
  `format`.
- `activeVideoMinFrameDuration` 1/30; max frame duration left at the device default (auto exposure may drop
  to 15 fps in low light) **until Alex's low-light test** decides (§11 question 3); output
  `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`, `alwaysDiscardsLateVideoFrames`.
- One `AVCaptureSession` and one scaler per camera device, shared by every video send stream on it (all streams
  use the cluster size); **one VT session per stream** (each sink has its own LTR/ack state, so one PC's losses
  never force refreshes on another; with one PC, the only case today, that is one encode). A quality change
  re-plans every stream on the device (`video_config` w/h -> `set_format` -> IDR at the new size).
- Center Stage is left to the user (`.user`); reaction gestures default off through Info.plist (§9.1).
  macOS 14+ effect APIs behind `@available` (§2.5).
- VT: `EnableLowLatencyRateControl` + `RequireHardwareAcceleratedVideoEncoder` (fail rather than the 436 ms
  software encoder), `RealTime`, `ExpectedFrameRate` 30, `AllowFrameReordering` false (so encoder output order
  = decode order = frame-number order), `ProfileLevel` `H264_ConstrainedHigh_AutoLevel`, `AverageBitRate`,
  `EnableLTR`; public keys only (no `MaxRefreshFrameIntervalDuration`). Session created as soon as the stream
  opens, kept for its life; re-created on a size change (new SPS/PPS + IDR). Before `start_encoding()` and while
  `set_input_paused(true)`, captured frames are not submitted.
- Observe `systemPreferredCamera` (KVO) when the source is null: switch devices in place (new format choice,
  IDR). Session interruption / runtime error -> `camera` `interrupted` and retry every 2 s; device gone ->
  `camera_lost`.

## 5. Wire protocol on the Beamer link

### 5.1 Rules

- **No new cap.** Camera messages require cap `audio` on both ends (`_CAP_FOR`) **and** the feature word
  `camera1` in the peer's `audio_hello.features`; the part never posts a `camera_*` message to a peer without
  it. Caps stay 14 of `MAX_CAPS` 16; wire version byte stays `0x06`. `camera1` is announced when
  `feature_flags.CAMERA` is on and the helper's `ready.features` contains `video1`.
- Same rules as A§5.1: direct-link handling on link threads only enqueues; unknown fields ignored; malformed
  -> ignored, never ends a link; no `seq`. Limits: <= 16 cameras per machine, <= 16 `native` formats per camera,
  `key` <= 200 chars, `name` <= 128, `app` <= 128, message < 16 KiB.
- Video media and feedback in link state ride the existing `audio_frame` (flow-level, A§5.2); no new type.

### 5.2 Messages

| type | data | scope / when |
|---|---|---|
| `camera_devices` | `{"machine":id,"rev":int,"platform":str,"status":"ok"|"starting"|"failed"|"no_video","features":["camera_capture"|"virtual_camera"],"cameras":[{"key","name","transport","native":["1920x1080@30",...]}],"preferred":key|null,"permission":"granted"|"denied"|"undetermined"|"restricted"|"unknown"|null,"sharing":bool|null,"capture":{"state":"idle"|"starting"|"running"|"interrupted"|"failed","by_other":bool,"effects":[...],"error":code|null},"virtual_camera":{"install":"ok"|"missing"|"outdated"|"broken"|"unsupported","present":bool,"in_use":bool,"app":str|null,"want":{"w","h","fps"}|null}}` | flooded (§5.3); `sharing` is authored only by that machine (Mac: true/false; others null); Mac: `virtual_camera.install` `unsupported`; Windows: `cameras` empty, `permission` and `sharing` null |
| `camera_route` | §6.2 | flooded, per-entry merge |
| `camera_open` | `{"stream":u32,"flow":str,"codec":{...§4.3},"key":b64,"camera":{"key","name"}|null,"for":{"set_at":int,"by":id}}` | direct, S -> T |
| `camera_opened` | `{"stream":u32,"ok":bool,"error":"not_installed"|"outdated"|"helper"|"unsupported"|"not_source"|null}` | direct, T -> S; `not_source`: S is not the camera machine of T's route |
| `camera_close` | `{"stream":u32,"reason":"route"|"demand"|"permission"|"sharing_off"|"camera_gone"|"helper"|"shutdown"|"error"}` | direct, either side |

### 5.3 Propagation and link loss

- `camera_devices` and `camera_route` follow A§5.3 exactly (own `rev` per machine persisted in `camera.json`,
  newer `rev` or direct-from-author accepted, forwarded unchanged to every other direct peer with `camera1`;
  on link up both sides send their own list, every held list and the whole route). A machine's `sharing` value
  is only ever taken from that machine's own list (no other machine can author it). **Online** is the audio
  part's `online` set (the `links` graph of `audio_devices`); the camera part keeps no graph of its own.
- A machine republishes `camera_devices` when cameras, preferred camera, permission, sharing, capture state,
  install state, `in_use`/`app`/`want` or helper status change.
- Link down with P: video streams with P stop at once (camera off on S when no other stream uses it).
  Helper restart: every video stream of this machine is closed (`camera_close {reason:"helper"}`), status
  `starting`, then re-planned on `ready`. A PC helper restart leaves the camera in apps: the DLL shows its
  built-in placeholder within 250 ms, then the new helper's placeholder/live picture.

## 6. Route model

### 6.1 What Alex sets

Cluster-wide, persisted on every machine:
1. **Active camera** `(machine, camera)`: `camera` null = that machine's system-preferred camera (followed);
   `machine` null = **off** (the initial state). Any machine's page can set it.
2. **Quality**: `720p` (default) or `1080p` = the size sent to every sink; bitrate 2.5 / 5 Mbit/s.

Mac-local, set only on that Mac and published in its own `camera_devices`:
3. **Sharing switch** (`sharing`, default on, persisted in that Mac's `camera.json`): "Stop Sharing the
   Camera" turns it off, "Share the Camera" (Mac menu or Mac page) turns it on. While off, the Mac's planner
   never opens its camera, whatever the route says; the route is left unchanged, so it still names the Mac and
   every page shows "Turned off on <Mac>". No other machine can change it (it is not in the route, and a
   machine's `sharing` is only accepted from that machine).

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
while `virtual_camera.in_use` (demand) is true there, plus a 3 s hold after it turns false, and only while S's
`sharing` is on and its permission is granted. The camera on S runs only while at least one such stream
exists. Why: same reasons as the mic (A§6.3): no focus rule, no extra setting, camera light only during real
use. If several PCs have demand, each gets its own stream at the cluster size (one capture and scaler, one
encoder per stream, §4.11).

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
    needs_permission: bool                      # own machine is S and its permission is undetermined:
                                                # Mac notice + menu item (§8.5); never a prompt by itself
    idle_placeholder: str | None                # own machine is a sink: vcam `idle` state when no sink is open
    wake_at: float | None
```

Rules (each machine evaluates the same function and acts on its own part):
1. S = `route.camera.machine`. None -> status `off`; every sink's idle placeholder `off`.
2. S must be online, have `camera_capture`, `sharing` on (off -> `sharing_off`), a resolvable camera (key,
   else unique name match, else `waiting_device`; null = S's `preferred`, none -> `no_camera`) and
   `permission == granted` (`undetermined` -> `waiting_permission`, `denied`/`restricted` -> `denied`).
3. For each online T != S with `virtual_camera.install == ok`: demand (or hold) and T in S's direct peers and
   T not in `no_udp` -> S sends `("video", T)` at the **cluster quality** (1280x720 or 1920x1080; T's `want`
   is not used: the source scales), bitrate by size, fps 30. Demand but not direct -> `unreachable`; direct but
   loopback-only -> `no_direct_path`; T without install -> `not_installed` / `outdated` / `broken` (shown even
   without demand); T = S or T on a platform without `virtual_camera` -> `no_virtual_camera` (shown as the
   out-of-scope note, §8.4).
4. Streams keyed by peer; the part diffs: new -> `send` + `camera_open` (Deviation 3); removed ->
   `camera_close {reason:"route"|"demand"|"permission"|"sharing_off"|"camera_gone"}` + `stop`; size/bitrate
   change of a running stream -> `video_config` (no reopen); device change -> close + open.
5. Receivers are passive: accept a `camera_open` only from the peer that is `route.camera.machine` in their own
   route view (else `camera_opened {ok:false, error:"not_source"}`), when `install == ok` and the helper is
   ready (else the matching error). A receiver holds **one** video recv stream: an accepted `camera_open`
   first closes any existing one (`camera_close {reason:"route"}` + `stop`) before `recv`. The sender retries
   when the receiver's list or the route changes.
6. `CameraStatus = {state, reason, params}`; states: `off`, `idle` (ready, no app), `starting`, `streaming`,
   `reduced` (step-down or link state), `sharing_off`, `waiting_permission`, `denied`, `waiting_device`,
   `no_camera`, `waiting_machine`, `unreachable`, `no_direct_path`, `not_installed`, `outdated`, `broken`,
   `no_virtual_camera`, `error`.
7. Sink idle placeholder (own machine is a sink with no open sink): `off` (route off), `sharing_off`,
   `waiting_permission`, `denied`, `error` (S's capture failed), else `waiting`.

### 6.5 Beyond Mac -> Windows (model only)

A future Mac sink (needs an Apple-signed Camera Extension) or Linux sink (v4l2loopback) would announce
`virtual_camera` with its own `install` states; a Windows source would announce `camera_capture` (and its own
`sharing`). The route, wire, planner and engine need no change; only backends do. This version never lists a
Windows or Linux camera as a source and never treats a Mac as a sink.

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
    def new_stream_id(self) -> int: ...                # unique among audio AND camera streams (service hunk)
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
    def link_out(self, peer: str, event: dict) -> None         # video link_out: drop rule §4.10
    def apply(self, now: float) -> None                        # plan + diff; called after every audio apply and on wake
    def snapshot(self) -> CameraSnapshot
    def stream_ids(self) -> frozenset[int]                     # for the service's _new_stream
    def dev_command(self, item: dict) -> bool                  # §7.4; True if it used a key
    def dev_status(self) -> dict                               # §7.4 `camera` key of the dev status file
    # local actions, from any thread through the service's queue (service.set_camera(...) etc.)
    def set_camera(self, machine: str | None, device: CameraRef | None) -> None
        # a local page action; when machine == own and own permission is undetermined, it also sends
        # permission_request (an action on the Mac, §9.1)
    def set_quality(self, size: str) -> None                   # "720p" | "1080p"
    def set_sharing(self, on: bool) -> None                    # own machine only (Mac): the kill switch
    def ask_permission(self) -> None                           # "Allow Camera Access…" on the Mac (page or menu)
```

The service gains thin public setters `set_camera`, `set_quality`, `set_camera_sharing`,
`ask_camera_permission`, `camera_snapshot()` that enqueue to the worker / read the part's last snapshot;
`subscribe` callbacks fire for camera changes too. The part's own state lives on the worker, like the audio
state (A§8.1). Merges of a `camera_route` received from a peer never call `set_camera` and never send
`permission_request`.

### 7.2 Snapshot (`core/camera/model.py`)

```python
@dataclass(frozen=True) class CameraRef: key: str; name: str
@dataclass(frozen=True) class Camera: key: str; name: str; transport: str; native: tuple[str, ...]; preferred: bool
@dataclass(frozen=True) class Capture: state: str; by_other: bool; effects: tuple[str, ...]; error: str | None
@dataclass(frozen=True) class VirtualCamera: install: str; present: bool; in_use: bool; app: str | None
                                             want: tuple[int, int, int] | None
@dataclass(frozen=True) class MachineCamera:
    id: str; platform: str; rev: int; status: str; features: frozenset[str]
    cameras: tuple[Camera, ...]; preferred: str | None; permission: str | None; sharing: bool | None
    capture: Capture; virtual_camera: VirtualCamera
@dataclass(frozen=True) class Stamp: set_at: int; by: str            # same rules as core.audio.model.Stamp
@dataclass(frozen=True) class CameraEntry: machine: str | None; device: CameraRef | None; stamp: Stamp
@dataclass(frozen=True) class QualityEntry: size: str; stamp: Stamp
@dataclass(frozen=True) class CameraRoute: camera: CameraEntry; quality: QualityEntry; extra: Mapping[str, Any]
@dataclass(frozen=True) class CameraStatus: state: str; reason: str | None = None; params: Mapping[str, str] = {}
@dataclass(frozen=True) class VideoStreamView: stream: int; source: str; target: str; state: str; path: str
    w: int; h: int; fps: float | None; kbps: float | None; delay_ms: float | None; frozen_ms: float | None
@dataclass(frozen=True) class LocalVcam: loaded_version: int; dll_version: int; bundled_version: int
@dataclass(frozen=True) class CameraSnapshot:
    own: str; machines: Mapping[str, MachineCamera]; route: CameraRoute
    status: CameraStatus; sinks: Mapping[str, CameraStatus]; streams: tuple[VideoStreamView, ...]
    permission: str | None; needs_permission: bool; sharing: bool | None    # own machine
    vcam_install_offer: str | None      # "install" | "update" | "repair" | None (this PC only)
    vcam_local: LocalVcam | None        # this PC only: versions for "Restart to finish the update" (§9.2)
```

Pure functions: `merge_route`, `accept_devices` (a list's `sharing` is kept only when it comes from its own
machine's list), `resolve_camera(ref, cameras)` (key, else unique name), `size_for(quality)`.

### 7.3 Persistence: `<state_dir>/camera.json`

`{"version":1,"route":{...§6.2},"own_rev":int,"sharing":true,"machines":{"<id>":{...last camera_devices data}}}`,
atomic (tmp + `os.replace`), debounced 500 ms, loaded at start (route re-applied as soon as links and devices
allow; `sharing` applied before the first plan, so a Mac that was switched off stays off across restarts).
`audio.json` is not touched.

### 7.4 Dev command files

Same mechanism as A§8.1 (`audio-dev.enabled`; one `audio-dev-cmd.json` read per second, parsed by
`AudioService._dev_poll`/`_dev_command`). The service hunk (CW6) passes every command item to
`part.dev_command(item)` after its own keys; the part (CW1) implements the new keys:
`{"set_camera": {"machine": id|null, "device": key|null}}`, `{"set_quality": "720p"}`,
`{"camera_sharing": true|false}`, `{"camera_test_source": "synth"|"synth:1920x1440"|null}` (the Mac sends the
synthetic source instead of the camera: the harness waits until `audio-dev-status.json` names it before
setting the route). The existing `{"helper_dev": {...}}` already reaches the helper's `dev` command (e.g.
`video_loss`, `video_drop_au`). The service hunk adds `camera` = `part.dev_status()` (the camera snapshot + the
last video stats) to `audio-dev-status.json`.

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
def placeholder_texts(snap: CameraSnapshot, own_label: str) -> dict[str, dict[str, str]]  # vcam placeholders

@dataclass(frozen=True)
class CameraSection:
    label: str                      # "Camera your apps see"
    choices: tuple                  # core.audio.view.Choice: "Off", headers per machine, cameras (+ "Default camera")
    selected: str
    line: str; tone: str            # status sentence (§8.4) and its tone (ok / amber / fault / muted)
    detail: str                     # "1280x720 · 30 fps · 72 ms · Wi-Fi" while streaming, else ""
    quality_label: str; quality_choices: tuple; quality_selected: str   # "Quality": 720p / 1080p
    hint: str                       # "Apps on a PC choose 'Beamer Camera'."
    help: str; help_action: str     # PC only: the multi-app help line (§8.6) and its "Camera settings" link
    this_machine: tuple             # rows about this machine: (label, value, tone, action, action_label)
    notices: tuple                  # core.audio.view.Notice
@dataclass(frozen=True)
class CameraMapExtras:
    source: str | None; source_line: str             # tile badge "MacBook Air Camera"
    watching: Mapping[str, str]                      # sink machine -> "Teams uses the camera" / "Camera ready"
    arrows: tuple                                    # (source, target, state "active"|"waiting")
def apply_camera(service, choice: Choice) -> None
def apply_quality(service, size: str) -> None
def apply_camera_notice(service, notice: Notice) -> bool    # permission / sharing / settings / install / retry actions
```

All strings through `core.i18n` (`N_`/`tr`), English in code, as the Audio page; `core/camera` is added to the
i18n tools' `SOURCES` (§2.2).

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
 │ This Mac   Camera sharing  On                     [ Stop Sharing ]           │   (Mac)
 │            Camera access   Allowed                                           │   (Mac)
 │ This PC    Beamer Camera   Installed                        [ Remove… ]      │   (PC)
 │            or:             Not installed    [ Install Beamer Camera… ]  (UAC) │
 │ One app at a time can use Beamer Camera unless Windows allows several.       │   (PC help line)
 │                                                    [ Camera settings ]       │
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
| `sharing_off` | on other machines: "Turned off on <S>." (muted); on S: "Camera sharing is off on this Mac." + action "Share the Camera" (muted) |
| `waiting_permission` | on other machines: "Allow camera access on <S>." (amber); on S: notice "Beamer needs camera access to share this Mac's camera." + action "Allow Camera Access…" (amber) |
| `denied` | "<S> does not allow Beamer to use its camera." + action on S "Open Privacy Settings" (fault) |
| `waiting_device` / `no_camera` | "<camera> is not connected to <S>." / "<S> has no camera." (amber) |
| `waiting_machine` / `unreachable` / `no_direct_path` | "<S> is offline." / "<S> and <T> are not linked directly." / "Video needs a direct network path between <S> and <T>." (amber) |
| `not_installed` / `outdated` / `broken` | "Beamer Camera is not installed on <T>." / "needs an update on <T>" / "is damaged on <T>" + on T: action "Install Beamer Camera…" / "Update…" / "Repair…" (amber) |
| `error` | "The camera stopped on <S>: <reason>." + Retry (fault) |

Out of scope, stated once in the hint: "Macs cannot show another machine's camera yet." and, when a Linux peer
announces audio, nothing (Linux never lists cameras). Placeholder texts (`placeholder_texts`, sent to the PC
helper with `vcam placeholders`, §4.6.4): `off` "Beamer Camera is off" / "Choose a camera in Beamer";
`waiting` "Waiting for <S>" / "<camera name>"; `starting` "Starting <camera name>" / "on <S>";
`waiting_permission` "Waiting for camera access" / "Allow it in Beamer on <S>"; `sharing_off` "Camera
turned off on <S>" / "Turn it on in Beamer on <S>"; `denied` "<S> has not allowed the camera" / "Allow it in
Beamer on <S>"; `reconnecting` "Reconnecting to <S>" / "<camera name>"; `error` "Beamer Camera stopped" /
"<reason>". Built-in DLL image: "Beamer Camera" / "Waiting for Beamer".

### 8.5 Mac indicator, kill switch and permission items

- macOS shows its own green camera light and the Control Center indicator naming Beamer; never hidden.
- Beamer's menu (`kvm_bridge_app.py`), whenever the camera feature is on and this Mac has `camera_capture`:
  - while the Mac sends video, a line "Camera: <App> on <T>" (or "Camera: in use on <T>") under the drive
    status; the menu-bar button tooltip gets the same line;
  - **"Stop Sharing the Camera"** while `sharing` is on, **"Share the Camera"** while it is off
    (`set_camera_sharing`): the Mac-local kill switch (§6.1). Stopping closes every video stream at once
    (`camera_close {reason:"sharing_off"}`), camera light off <= 0.5 s later;
  - **"Allow Camera Access…"** while `needs_permission` (the route names this Mac and permission is
    undetermined): sends `permission_request`, i.e. the macOS prompt appears because of this click.
- The Mac page shows the same switch ("Camera sharing  On [Stop Sharing]" / "Off [Share the Camera]") and the
  permission notice with its "Allow Camera Access…" button.
- The tile of the Mac on the map shows the camera badge in the accent colour while streaming.
- No notification banner (the system indicator already appears at each start).

### 8.6 Windows page specifics

The PC's own "This PC" row shows the install state with Install / Update / Repair / Remove actions (§9.2),
offered only while no video stream runs on this PC; after a UAC cancel the row says "Installation was
cancelled."; when `loaded_version` != `dll_version` (an update is installed but Frame Server still runs the old
DLL) it says "Restart Windows to finish updating Beamer Camera." Under it, whenever Beamer Camera is installed,
a **static help line**: "One app at a time can use Beamer Camera unless Windows allows several: Settings >
Bluetooth & devices > Cameras > Beamer Camera." with a "Camera settings" link that opens `ms-settings:camera`
(a link only, no automation). Beamer does not try to detect a refused second app (the source never sees it,
C2 Q6), so there is no `busy` state.

## 9. Packaging, install, permissions

### 9.1 Mac

- `setup.py` plist: `NSCameraUsageDescription` = "Beamer sends this Mac's camera to another computer while an
  app there uses Beamer Camera, after you choose it on the Audio & Camera page.";
  `NSCameraReactionEffectGesturesEnabledDefault` = false (C1 Q5; ignored by macOS 13).
- `entitlements.plist` (bundle + `python`) and `entitlements-audio.plist` (helper) +=
  `com.apple.security.device.camera`. `build_dmg.sh` signing unchanged otherwise (the helper is already signed
  with its own entitlements). `Install-Beamer-Mac.command` adds `Camera` to the TCC services it resets for
  ad-hoc builds.
- **When the prompt appears: only after an action on the Mac.** The part sends `permission_request` only when
  (a) the Mac's own page chooses one of this Mac's cameras (`set_camera(own, …)`) while permission is
  `undetermined`, or (b) "Allow Camera Access…" is clicked on the Mac (page notice or menu item). A route
  received from another machine (the PC page chose the Mac's camera) never triggers it: the Mac then shows the
  notice and the menu item (§8.5), the PC shows "Allow camera access on <Mac>" and its apps get the
  `waiting_permission` placeholder. When `denied`, the Mac's action opens
  `x-apple.systempreferences:com.apple.preference.security?Privacy_Camera`. Never triggered by a PC app opening
  the camera. The prompt names Beamer (responsible process) and shows the usage string (human check H1).

### 9.2 Windows: install, update, uninstall

- **Shipped files**: `beamer-audio.exe` (unchanged location) and `camera\BeamerCameraSource.dll` beside it:
  `{app}\camera\` in Inno, `Install-Beamer.ps1` the same, and the portable build's `COLLECT` with a
  `("camera\\BeamerCameraSource.dll", <path>, "EXECUTABLE")` entry so it lands next to `beamer-audio.exe`
  rather than under `_internal` (`build_portable.spec` comment: EXECUTABLE entries go to the top of the folder;
  the sub-folder placement is **unverified** and is part of CW9's acceptance). The DLL there is never loaded by
  Frame Server; it is the install source.
- **Lookup rule** (one rule for shipped, portable and dev trees; implemented in the helper, used when
  `vcam-install` gets no `--dll`): `<helper exe dir>\camera\BeamerCameraSource.dll`, then `<helper exe
  dir>\BeamerCameraSource.dll`. The dev build writes `native\audio\build\win\camera\` (§2.5), so the first
  rule matches everywhere. `bundled_version` = the helper's compiled `kDllVersion` (both binaries share
  `shm_protocol.h` and are built together).
- **Install / update / repair** (elevated, once per DLL version): the page's button calls
  `win_app/vcam_install.py`, which runs `ShellExecuteExW(lpVerb="runas", lpFile=<helper exe>,
  lpParameters="vcam-install --owner-sid <current user SID>")` (UAC prompt), waits on a background thread, then
  asks the helper for `vcam` status. `vcam-install`: copy to `%ProgramFiles%\Beamer\Camera\<kDllVersion>\
  BeamerCameraSource.dll` (inherited ACL: Users read, so LocalService reads it), create
  `%ProgramData%\Beamer\Camera` with the §4.6.3 ACL, write `HKLM\Software\Classes\CLSID\{19FC...}\InprocServer32`
  (default = path, `ThreadingModel=Both`) and `HKLM\SOFTWARE\Beamer\Camera` (`InstallGuid` kept if present,
  else new; `OwnerSid`; `DllVersion`; `DllPath`); delete older version folders that are not locked; exit code
  per §3.1. Then the unelevated helper creates and starts the camera (§4.6.5).
- **Versioned path and when the new DLL runs.** A new DLL version installs beside the old one and re-points the
  CLSID. Frame Server keeps the old DLL while a source object of ours exists (C2), and with a System-lifetime
  camera that may be until the service stops or Windows restarts: whether it unloads when idle is **unverified**
  (CW5 measures it). The truth comes from the running source: `loaded_version` (its `Header.dll_version`) vs
  `DllVersion`; when they differ the page says "Restart Windows to finish updating Beamer Camera." (a dev agent
  over ssh may instead restart the `FrameServer` service). The helper keeps working with the old DLL
  while the shared-memory `version` is one it supports. `bundled_version` > `DllVersion` -> `outdated` ->
  "Update Beamer Camera…". Beamer updates that do not bump `kDllVersion` need no admin step.
- **Stale version folders**: only an elevated process can delete them; every elevated action (install, update,
  repair, uninstall, and the existing install maintenance `Install-Maintenance.ps1`) retries. Leftovers are
  harmless (~200 KB each).
- **Uninstall from the page** ("Remove…"): `vcam-remove` (unelevated helper verb, `Remove()` of this user's
  camera), then the elevated `vcam-uninstall` (deletes the CLSID, `HKLM\SOFTWARE\Beamer\Camera`, the
  `Camera` folders; if the DLL is loaded it stops the `FrameServer` service first: all cameras pause ~1 s,
  warned in the confirmation).
- **Uninstall from Beamer's uninstaller**: the Inno uninstaller already runs one elevated maintenance step
  (`[UninstallRun]` -> `Invoke-InstallMaintenance.ps1 -Remove` -> `Install-Maintenance.ps1` with `-Verb RunAs`).
  CW9 adds, **before** that entry, `{app}\beamer-audio.exe vcam-remove` (unelevated, best effort), and to
  `Install-Maintenance.ps1 -Remove` a `Remove-BeamerCamera` step in PowerShell (CLSID, HKLM key, `%ProgramFiles%
  \Beamer\Camera`, `%ProgramData%\Beamer\Camera`; stop `FrameServer` only if a file is locked). One UAC prompt,
  the one the uninstaller already shows. `Install-Beamer.ps1`'s uninstall path does the same.
- **Dev**: `ssh lebig` sessions are elevated (C2), so a dev agent runs `beamer-audio.exe vcam-install ...`
  over ssh with no UAC; `dev/deploy.sh` is not changed by this design (operator-owned); CW9 documents the
  one-liner in `native/audio/vcam-win/README.md`.
- No firewall change: the DLL has no network; the helper's existing allowance covers video.
- Signing: both binaries signed with `Sign-BeamerArtifact` when a signtool identity exists (Smart App Control
  risk, §11).

### 9.3 Mac permission, sharing and PC install in the UI (who sees what)

| Machine state | Mac page / menu | PC page | PC apps see |
|---|---|---|---|
| Mac permission undetermined, Mac chosen from the PC | notice + "Allow Camera Access…" (page and menu); no prompt until clicked | "Allow camera access on <Mac>." | `waiting_permission` placeholder |
| Mac permission undetermined, Mac chosen on the Mac | the prompt (that choice is the Mac action) | "Allow camera access on <Mac>." | `waiting_permission` |
| Mac permission denied | fault + "Open Privacy Settings" | fault, no action (it is the Mac's) | `denied` |
| Mac sharing off | "Camera sharing is off on this Mac." + "Share the Camera" | "Turned off on <Mac>." (no action) | `sharing_off` |
| PC not installed | amber notice naming the PC | "Install Beamer Camera…" (UAC) | (no camera) |
| PC outdated | amber notice | "Update Beamer Camera…" (UAC) | old DLL keeps working |
| PC updated, old DLL still loaded | - | "Restart Windows to finish updating Beamer Camera." | old DLL keeps working |

## 10. Testing

### 10.1 Python (`core/tests/test_camera_*.py`, `unittest`, both machines)

`mac_app/.venv/bin/python -m unittest discover -s core/tests -p "test_camera*" -t .` (and the PC venv).
- `test_camera_wire`: builders/validators, limits, unknown fields, malformed ignored, `camera1` gating helpers.
- `test_camera_model`: route merge (commutative, idempotent, per entry, ties by `by`, unknown entries kept),
  device-list acceptance (`sharing` only from its own machine), `resolve_camera`, `size_for`.
- `test_camera_planner` (table-driven): off; on demand 0/1/2 sinks; hold 3 s; permission undetermined/denied;
  **sharing off** (no send whatever the route; every sink `sharing_off`; a newer route naming the Mac does not
  change it); missing camera and return; preferred camera followed; not installed/outdated; not direct;
  loopback-only; size = cluster quality whatever `want` (360p, 720p, 1080p apps); Mac as T ->
  `no_virtual_camera`; Linux peer ignored; idle placeholder per state.
- `test_camera_part` with `FakeHelper` + `fake_video` and a fake `MediaContext`: send+open at once on demand,
  close after the hold, `video_config` on quality change without reopen, `camera_opened` failure and retry
  rule, **`permission_request` sent only by a local `set_camera(own, …)` or `ask_permission()`, never by a merged
  route or a PC demand**, `sharing` persisted and applied before the first plan, `camera_open` from a peer that
  is not the route's S refused `not_source`, a second accepted open closes the first recv, placeholders pushed
  on every sink status change, link_out video drop over 32 drops every later event of the same frame and
  reports it, pause after 1 s, helper restart closes and re-plans, `dev_command` keys, never blocks a link
  thread.
- `test_camera_cluster` (harness extension): 3 machines (Mac source, PC sink, Linux without `audio`) + one
  fork machine without `camera1`: convergence < 50 rounds for 200 seeded edit/partition sequences; no
  `camera_*` ever posted to a peer without `audio` or without `camera1`; a Mac with sharing off never sends,
  whatever the other machines set.
- `test_camera_view`: section model from `camera_fixtures.py` for every status of §8.4 on the Mac and the PC
  side, choices, hint, help line, placeholder texts, map extras; `test_i18n_coverage` passes with `core/camera`
  scanned.

### 10.2 Engine (`native/audio/tests/video_tests.cpp`, in `engine_tests`)

Video header/nack/pli/time-map/RR round trips and limits; `ReplayWindowN<1024>` (in-order, reorder, replay, too
old); fragmentation of AUs 1 B..300 KB and reassembly with any fragment order; **frame numbering**: numbers
assigned in `on_frame`, returned to the backend, contiguous; **queue overflow** with the null encoder (forced
backlog): backpressure pauses input with 0 AUs lost, and the hard cap's drops appear as holes, the sender
refreshes, **0 incorrect kept pictures**; `video_drop_au`; NACK scheduling (immediate, 2 x RTT retry, <= 3
tries, deadline, hole > 64 -> PLI); clean-run rules (hole, decoder error revoking kept pictures, refresh/IDR
restart); RR clean runs -> verified LTRs -> `acknowledge_ltr` (lost PLIs and lost RRs never produce an
acknowledgement of a corrupt LTR); sender recovery state machine against a fake `CameraHandle` (PLI -> refresh,
duplicate ignored, 3 failed refreshes -> IDR, recovery IDR limit 2 s; **start PLIs repeated during a paced IDR
give exactly one IDR**; decoder reopen -> start -> IDR, never a refresh; start/size limit 1 per 1 s); pacer
timing (IDR at 3x bitrate within ±5 %, bucket 32 packets, audio bypass); **time map** (sent only for covered
streams, never moves a stream's seq or loss; audio stream loss unchanged with `sr:true`); clock offset from
synthetic pong samples (min-RTT pick, stale in link state, provisional seed); playout target on synthetic
delay traces (p99 + 8 within [50,150], rise/fall rules), presenter late/newest rule, backlog threshold with
hold, hold -> 2 s -> `show_placeholder("reconnecting")` -> back to live; lip-sync hold computation (CW10 part);
rate ladder transitions; link-state switch (video only on the video outbox with frame-tagged events, feedback
also in the link outbox, NACK on, fps 15, **LTR refreshes still answered over the link**). ASan/UBSan and TSan
builds on the Mac, **TSan with forced PLI/NACK storms** (loss 20 %, decoder errors injected every 50 ms,
`video_drop_ms` bursts) proving that only the network thread seals receiver-direction packets; plain on the PC.

### 10.3 Null video backend (`null_video.{h,cpp}`)

`make_null_video_backend("cams=2;perm=granted;vcam=ok;ip=120000;p=10000;refresh=14000;enc_ms=5;dec_ms=2;native=1920x1440")`:
- Encoder: AUs at the requested fps with sizes from C1 (IDR, P lognormal CV 0.34, refresh), each AU carrying
  its frame number (from `on_frame`'s return) and its references (previous frame, or the verified LTR set for a
  refresh, or none for an IDR) in its bytes; honours every `CameraHandle` call, including real LTR bookkeeping
  (LTR every 30 frames), `set_input_paused` and `set_format`; reports `native` and always emits the requested
  output size.
- Sink: a fake decoder that tracks reference validity exactly like an infinite-GOP H.264 decoder (a frame is
  correct iff all its references are correct and its bytes are complete) and **counts any `keep=true` picture
  that is not correct** (must stay 0); records present times and placeholder states; settable `vcam_in_use`,
  `want`, `install`, `loaded_version`; rejects sizes above 1920x1080 (`size_rejected`).
- So two `beamer-audio-null` helpers exercise the whole protocol over 127.0.0.1, with `dev video_loss`.

### 10.4 OS acceptance (per backend WP, real hardware)

Mac: `cameras` inventory, synthetic source through VT (native 1280x720, 1920x1440, 1080x1920, 640x480 -> exact
output size, crop geometry checked on the code strip / test pattern), real camera once permission exists (H1,
needs a deployed Beamer.app: an agent-shell helper is attributed to T3 Code, C1 Q6). Windows: C1's recordings
through `recv` into the real DLL, consumers `camprobe` (MF), ffmpeg dshow, headless Chrome/Edge
(`proto/win/web/gum.html`) in Alex's session through the scheduled-task trick
(`proto/win/scripts/in-session.ps1`); `shmfuzz` against the installed DLL.

### 10.5 Two-machine E2E (`dev/camera/e2e/`)

Built on `dev/audio/e2e` (imports `beamer.py`, `clock.py`, never edits them) and on C4's tools when they land
(`proto/e2e`). The Mac sends its helper's synthetic source (C1 code strip: frame number + Mac host time in
every picture); the PC reads frames with `camprobe read --strip c1` in Alex's session; Mac/PC clocks fitted by
`clock.py`. Cases and pass marks:

| Case | What | Pass |
|---|---|---|
| `cam_start` | open Beamer Camera on the PC 20 times (10 s on, 10 s off) | first real frame <= 1.0 s p50, <= 1.5 s p95 **[C4 adjusts]**; placeholder before it, never black; exactly 1 IDR per start |
| `cam_latency` | 10 min, 720p and 1080p | capture->app (synthetic, i.e. excluding the camera) p50 <= 90 ms, p95 <= 130 ms **[C4 adjusts]** |
| `cam_loss` | `video_loss` 0.5 % bursty (mean 6) and 1 % iid, 10 min each; then `video_drop_au 1` every 30 s for 5 min | frozen <= 300 / 200 ms/min; IDR count = 1 (start); 0 corrupt pictures (code strip checks); each dropped AU recovered by a refresh |
| `cam_stall` | `video_drop_ms 1500` every 2 min, 10 min | hold then recovery <= 1 RTT + 2 frames after the drop; recovery by refresh, no IDR; `reconnecting` never shown (< 2 s) |
| `cam_fallback` | `drop_udp` 30 s during a mic + camera session, with `video_loss` 1 % on the link side | video on the link <= 1.1 Mbit/s; RRs and PLIs seen by the Mac over the link; recovery by refresh (no IDR after the start); mic glitches/min not above the audio-only fallback run + 1; resumes on UDP <= 2.5 s |
| `cam_stop` | close the PC app | Mac camera stopped (`camera` `stopped`) <= **4.0 s** after the app closed |
| `cam_killswitch` | sharing off on the Mac (dev command), then the PC sets the route to the Mac camera and opens Beamer Camera | the Mac never starts capture (`camera` events), PC status `sharing_off`, PC app shows the `sharing_off` placeholder; sharing on -> first real frame within the `cam_start` budget |
| `cam_helper_kill` | kill the PC helper while streaming | DLL built-in placeholder <= 300 ms; live again <= 5 s after the watchdog restart |
| `cam_soak` | 30 min mic + camera, Wi-Fi as is | <= 1 frozen episode > 500 ms per 10 min outside logged Wi-Fi stalls; every hole > 5 packets logged with frame type; memory growth < 5 MB both helpers; CPU Mac helper < 6 %, PC helper < 5 % of a core; mic `loss_pct` not above an audio-only run (no phantom loss from the time map) |
| `cam_lipsync` (CW10) | flash + beep test source on the Mac mic and camera | A/V offset at the PC within -20..+40 ms (audio later = positive) |
| `cam_oldpeer` | PC <-> asusrog | link unaffected, no `camera_*` and no `time_map` sent to it (logs, `n_rejected` unchanged on asusrog) |

Human checks (batched for Alex): **H1** the Mac camera prompt names Beamer, appears only after a Mac action, and
the image appears on the PC; **H2** Camera app, Teams, Discord, a browser call show "Beamer Camera" and the
placeholder; **H3** the "multiple apps" toggle and the help link; **H4** after a logoff/reboot Beamer Camera is
still listed; **H5** low light and effects look (C1 Needs Alex 2-3); **H6** one real install via the UAC path
(~1 min); **H7** uninstalling Beamer removes Beamer Camera from apps' lists.

## 11. Risks and open points

1. **Camera numbers missing** until H1 (cold start, capture latency, low-light fps): the start and latency
   targets carry **[C4]** marks.
2. **Smart App Control / WDAC** would likely block an unsigned DLL in Frame Server: sign when an identity
   exists; detect `install: broken` (CLSID present, activation fails) and say so.
3. **Logoff/reboot, sleep/resume, fast user switching** untested for the System-lifetime camera (H4); the helper
   re-runs create+Start at every start anyway (idempotent).
4. **Silent decoder corruption**: correctness rests on §4.5.2's clean-run rules and on acknowledging only
   verified LTRs; the null sink's "0 corrupt kept pictures", the queue-overflow test and the E2E code-strip
   check are the guards. Residual: the real decoder's long-term reference state after a lost AU is trusted to
   follow the encoder's (C2 measured bit-exact LTR recovery; **[C4]** runs the real loop).
5. **Keyframe-triggered Wi-Fi holes** (C3): IDRs paced at 3x (4x untested); soak logs holes > 5 packets with
   frame type; `idr_pace_x` tunable.
6. **One app at a time** on the PC (Windows default); only a static help line, Beamer cannot see a refused
   second app.
7. **Elevation UX**: `ShellExecuteEx runas` from a per-user app; over-the-shoulder admin accounts handled by
   passing `--owner-sid`; multi-user PCs: one owner only (out of scope).
8. **DLL updates may need a Windows restart** (Frame Server unload timing unverified, CW5 measures); surfaced
   from `loaded_version`.
9. **Video effects** apply at the user's choice; reactions cannot be disabled by Beamer.
10. **App name** is best effort (ConsentStore semantics are undocumented).
11. **CPU on the Mac** with mic + AEC + camera: estimated +2-3 % of a core for video (C1), + the scaler for
    cameras without an exact format; soak checks.
12. **Same-user processes can write the shared memory**: the source validates everything (§4.6.3), so the worst
    case is a wrong picture in the user's own camera, never a Frame Server crash or a leak.
13. **[C4]** risks from the end-to-end prototype.

Open points (operator defaults in force, Alex may override):
1. Page name "Audio & Camera" (operator default): cheap to revert.
2. One quality for the whole cluster, 720p default (operator default); per-sink later via `sinks`.
3. Pin 30 fps in low light or let auto exposure drop to 15 fps: needs H5; default not pinned.
4. **For the operator**: "one encode" read as one encoded *size* per cluster; with several PCs this design keeps
   one VT session per PC (independent LTR/ack state, +~2 % CPU per extra PC). Sharing one encoder across PCs
   would couple their recovery (any PC's PLI refreshes all, acks must be verified by all); change §4.11 if that
   was meant.

## 12. Work packages

Each WP works on its own branch from `feat/camera` (own worktree where the operator can give one; otherwise in
the shared checkout committing only its own paths, as in Phase 1), commits with `git commit -s`, and is merged by
the operator. File ownership is exclusive (the owner of every new file is in §2.1, of every hunk in §2.2).
PC agents (`ssh lebig 'claude -p ...'`) work in `C:\Users\Alex\beamer-cwN\` (a clone, never the PC clone Beamer
is built from) and write their report to `~\briefs\<wp>.report.md`. Contract headers are committed first and
then never changed without the operator: `video_backend.h` (CW2), `shm_protocol.h` (CW5),
`core/camera/model.py` (CW1), `core/camera/view.py` dataclasses (CW7).

**Translations**: no WP commits `locales/`. WPs that add strings run `test_i18n_coverage` and accept only the
`test_catalogues_are_current_and_complete` failure caused by a stale `beamer.pot` (state it in the report); the
merge step runs `python3 tools/i18n_extract.py` after each merge that adds strings and commits `locales/` alone.

**Human items**: any acceptance item that needs the real Mac camera, a UAC click, or Alex's eyes is marked
(H1-H7, §10.5) and listed in the WP report instead of blocking the WP.

**Wave 1 (start now, in parallel): CW1, CW2, CW3, CW4, CW5, CW9.** CW3/CW4 code against `video_backend.h` (CW2's
first commit, within its first hours) with local harnesses, and CW4 against `shm_protocol.h` (CW5's first
commit). **Wave 2: CW6 (after CW1), CW7 (after CW1 milestone 1), CW8 (after CW7 milestone 1).** **Wave 3: CW10,
CW11.**

### CW1 Python camera control plane (Mac agent)
- Goal: `core/camera` minus `view.py`: wire, model, planner (incl. sharing switch, cluster-size rule,
  accept-from-S), store, part (incl. `dev_command`/`dev_status`, `stream_ids`, permission only on local actions,
  link_out drop rule), fake video helper; tests; cluster harness extension.
- Owns: `core/camera/{__init__,wire,model,planner,store,part,fake_video}.py`,
  `core/tests/test_camera_{wire,model,planner,part,cluster}.py`; hunks in `core/audio/fake_helper.py`
  (delegation) and `core/tests/audio_cluster_harness.py` (optional camera parts).
- Inputs: §3 (client side), §4.7, §4.10 (Python side), §5, §6, §7, §9.1 (prompt rule). **Milestone 1 (first
  commit): `model.py` exactly as §7.2.** Depends on: nothing (fake helper, fake `MediaContext`).
- Acceptance: all `test_camera_*` of §10.1 pass on the Mac and the PC; cluster: convergence < 50 rounds for
  200 seeded sequences, no `camera_*` to a peer lacking `audio` or `camera1`, a Mac with sharing off never
  sends; on-demand open issued in the same worker iteration as the `vcam in_use` flood arrives; close at hold
  3.0 s ±0.1 s (simulated clock); `permission_request` count = number of local Mac actions (0 for PC-side
  selections); the part never calls `post` off the worker (asserting host).

### CW2 Engine video transport (Mac agent; builds and runs its tests on the PC over ssh)
- Goal: §4.1-4.5, §4.9 data (time map, offset, `audio_present_delay`/`video_present_delay`), §4.10 (engine
  side: video outbox, feedback over the link, input pause), the control protocol additions of §3 on the engine
  side, `video_backend.h`, the null video backend, `run_helper` overload.
- Owns: `native/audio/engine/{video_backend.h,video_packet.*,video_send.*,video_recv.*,pacer.*,sync.*,null_video.*}`,
  `native/audio/tests/{video_tests.cpp,two_helpers_video.py}`; hunks of §2.2 in `packet.*`, `flow.*`,
  `streams.*`, `helper.*`, `sources.txt`, `build-tests.{sh,cmd}`, `tests/null_main.cpp`,
  `tests/golden_control.txt`.
- Inputs: C3 report Q1-Q5, §2.4, §3, §4, §10.2, §10.3. Depends on: nothing. **First commit: `video_backend.h`
  verbatim.**
- Acceptance: engine tests pass on the Mac (plain, ASan/UBSan, TSan incl. the PLI/NACK storm run) and the PC;
  existing audio tests, golden lines (plus the new ones) and `two_helpers.py` unchanged; audio stream loss
  identical with `sr:true` vs false on a lossless run; `two_helpers_video.py`, two null helpers, 720p30 2.5
  Mbit/s, 120 s each, mic stream on the same flow: no loss -> 0 frames missed after the start, first frame <=
  300 ms after `send`, exactly 1 IDR; 1 % iid -> frozen <= 200 ms/min and 0 IDRs after the start; 0.5 %
  Gilbert (mean 6) -> frozen <= 400 ms/min; 1.5 s drop -> clean again <= RTT + 2 frames after it ends, by
  refresh; `video_drop_au` and forced queue overflow -> recovery by refresh; **0 incorrect kept pictures** in
  every run; no 10 ms window carries more than 3.0x bitrate x 10 ms + 32 packets during an IDR; audio
  `late+plc` within +1/min of the same run without video; link state (`drop_udp`): video <= 1.1 Mbit/s, only via
  frame-tagged `link_out` with `video:true`, RR/PLI/time map via ordinary `link_out`, LTR refreshes (not IDRs)
  answer PLIs; engine video CPU < 1 % of a core per helper (null encode/decode excluded).

### CW3 Mac camera backend (Mac agent)
- Goal: `VideoBackend` for macOS (§4.11): inventory + KVO preferred camera, permission state/request, capture
  session sharing, format choice + centre crop + scale to the cluster size, VT low-latency encoder with the
  frame-number -> LTR token map (`on_frame` return), refresh/IDR/bitrate/fps/format/input pause, interruption
  handling, effects/by_other reporting (`@available` guards), `--dev` synthetic source with a settable native
  size; `main.mm` and `build-mac.sh` hunks.
- Owns: `native/audio/mac/{video_backend_mac.h,video_backend_mac.mm,camera_mac.mm,scale_mac.mm,videoenc_mac.mm,synth_mac.mm,video_check.mm}`;
  hunks in `main.mm`, `build-mac.sh`.
- Inputs: C1 report, `proto/mac/src` (lift `capture.mm`/`encode.mm`/`synth.mm`), §2.4, §3.3, §4.3, §4.11.
  Depends on: CW2's `video_backend.h` (local harness until the engine lands). Real-camera items (marked H1)
  need permission granted to a **deployed Beamer.app**, i.e. CW9's packaging and an operator deploy
  (`dev/deploy.sh`); CW3 finishes without them and lists them.
- Acceptance (this Mac): `cameras` lists "MacBook Air Camera" with its native formats and `preferred`;
  synthetic 720p30 through the helper: encode p95 <= 8 ms, 1 s bitrate within ±10 %, helper CPU < 4 % of a core;
  synthetic native 1920x1440, 1760x1328, 1080x1920 and 640x480 -> output exactly 1280x720 and 1920x1080, crop
  per §4.11 (test-pattern geometry within 2 px), scaler p95 <= 2 ms; `request_ltr_refresh` after
  `acknowledge_ltr` gives a non-IDR AU <= 40 KB (720p); without acks an IDR; `on_frame` numbers map to the right
  tokens across `set_input_paused` and `set_format`; `set_bitrate` effective within 300 ms; `set_format` -> IDR
  at the new size; builds and runs on macOS 13 API level (no unguarded 14+ symbol: `-Wunguarded-availability`
  clean); universal binary (`lipo -archs` = `x86_64 arm64`). H1 items: `stop` -> capture session stopped <= 500
  ms; camera start -> first AU measured and reported (feeds §4.7); `permission` transitions `undetermined ->
  granted` seen without restart.

### CW4 Windows video sink in the helper (PC agent)
- Goal: `VideoBackend` for Windows (§4.6.1, 4.6.3 helper side, 4.6.4-4.6.6): MF decoder (HW + SW fallback, size
  rejection), 12-picture pool, shared-memory client (open, heartbeat from the presenting thread,
  live/hold/waiting, seqlocked writes, validation of source-written fields, Demand ownership check),
  camera create/Start, install state and `loaded_version`, placeholder renderer with the per-state texts and
  `show_placeholder`, app usage, verbs `vcam-install` (incl. the ProgramData folder ACL, DLL lookup rule),
  `vcam-uninstall`, `vcam-remove`, `vcam-status`; `main.cpp` and `build.ps1` hunks.
- Owns: `native/audio/win/{video_backend_win.h,video_backend_win.cpp,videodec_win.cpp,vcam_win.cpp,placeholder_win.cpp,app_usage_win.cpp,vcam_install_win.cpp,video_check.cpp}`;
  hunks in `main.cpp`, `build.ps1`.
- Inputs: C2 report, `proto/win/{feeder,vcamctl}.cpp`, §2.4, §3, §4.6, §9.2.
  Depends on: CW2's `video_backend.h`, CW5's `shm_protocol.h` (and CW5's DLL for the live checks).
- Acceptance (lebig, helper in Alex's session via the scheduled task): C1's 720p/1080p recordings through
  `recv` -> DLL -> `camprobe`: decode p95 <= 4 ms (720p) / <= 5 ms (1080p) hardware, 0 frames of decoder lag;
  forced software works (CPU < 30 % of a core at 1080p); an SPS of 2560x1440 is rejected (`size_rejected`, no
  slot written); `vcam in_use` <= 50 ms after an app opens the camera, off 300 ms after it closes; `app` reported
  for Chrome and `camprobe`; placeholder on screen <= 100 ms after a `vcam` idle change; `starting` at
  `open_sink`, hold then `reconnecting` at 2 s; a wedged presenter (test hook) -> DLL built-in image <= 300 ms;
  a pre-created foreign `.Demand` event is refused; `vcam-install`/`vcam-uninstall` work elevated (ssh) and exit
  3 unelevated, `vcam-remove` works unelevated; install states `missing/ok/outdated/broken` and
  `loaded_version` detected; simulated device removal -> software decode within 1 s and back to hardware after
  30 s.

### CW5 Windows camera source DLL (PC agent)
- Goal: `BeamerCameraSource.dll` per §4.6.2-4.6.3 (hostile-input validation, private copy, LocalService-only
  activation, object ownership checks), built-in placeholder image, tools (`camprobe` with `--strip c1`,
  `feeder` writing the shared memory, `shmfuzz`), DLL build script, and the DLL update measurement.
- Owns: `native/audio/vcam-win/**`. **First commit: `shm_protocol.h` verbatim.**
- Inputs: C2 report and `proto/win/{source.cpp,common.h,camprobe.cpp,feeder.cpp,web/gum.html}`, §4.6.
  Depends on: nothing.
- Acceptance (installed with `vcam-install` or a local script until CW4 lands): MF, DirectShow (ffmpeg dshow),
  headless Chrome/Edge see "Beamer Camera"; 720p/1080p/360p NV12 @30 for 60 s with 0 gaps; `feeder` -> app p95
  <= 1.0 ms (720p) / 1.5 ms (1080p); built-in placeholder 250 ±50 ms after the heartbeat stops; `mode`
  waiting -> slot 3 within one frame; repeated Stop/Shutdown idempotent (state_seq increments, no crash);
  **`shmfuzz` from Alex's unelevated session writing random and boundary values into every helper-written field
  at 1 kHz for 10 min while `camprobe` reads: Frame Server never crashes or restarts, every delivered frame is
  the built-in image, a previous good picture or a valid slot, `rejected` rises**; loading the DLL in a user
  process (`MFCreateVirtualCamera` validation) creates no kernel object and no log; a foreign pre-created
  `.Frame` event is refused; objects' DACL = SYSTEM, LocalService, owner SID (checked with an `icacls`-style
  dump); Frame Server CPU <= 1.5 % of a core at 1080p; unregister + delete leaves no `SWD\VCAMDEVAPI` node;
  **measured and reported**: after re-pointing the CLSID to a new version folder, when Frame Server loads the new
  DLL (app closed, camera idle N minutes, FrameServer restart, reboot), which fixes the §9.2 restart wording.

### CW6 Link and service integration (Mac agent; PC verified over ssh)
- Goal: the `camera1` feature, `camera_*` gating and dispatch on both apps, the `MediaContext` hunk in the
  service, stream-id uniqueness, audio `sr` gating on `video1`, the dev command and dev status plumbing (§7.4),
  the flag, WIRE.md.
- Owns: §2.2 hunks in `core/link.py`, `core/receiver.py`, `mac_app/bridge.py`, `win_app/sender.py`,
  `win_app/kvm_bridge_win.py` (message forwarding only), `core/audio/service.py`, `core/feature_flags.py`,
  `WIRE.md`, `mac_app/setup.py` (`includes`); new `core/tests/test_camera_link_gating.py`.
- Inputs: §2.2, §4.9 (`sr`), §5, §7.1, §7.4. Depends on: CW1 (`part.py` API).
- Acceptance: gating test (initiator and responder links drop `camera_*` without `audio`; the part sends nothing
  to a peer without `camera1`); a service test that `_new_stream` never returns a camera stream id, that audio
  `send` carries `sr:true` only to a `video1` peer, and that each §7.4 dev command reaches the part (and
  `helper_dev` the helper) and the status file has `camera`; full `core/tests`, `mac_app/tests`, PC
  `win_app/tests` unchanged except the known failures (`dev/README.md`); `dev/deploy.sh` both with null video
  (`audio-dev.enabled` helper = `beamer-audio-null`) until CW3/CW4 land: both logs show `camera1` and
  `camera_devices` exchanged; caps still 14; asusrog link up and silent (no `camera_*`, no `time_map`); audio E2E
  `mic_mac_pc` unchanged.

### CW7 Camera page model, Mac UI, menu items (Mac agent)
- Goal: §8.2-8.5 on the Mac: `core/camera/view.py` (incl. `placeholder_texts`), the AppKit Camera module with
  the sharing row and permission notice, map extras, page rename, menu line, "Stop Sharing the Camera" / "Share
  the Camera", "Allow Camera Access…"; i18n tool scope.
- Owns: `core/camera/view.py`, `core/tests/{test_camera_view.py,camera_fixtures.py}`, `mac_app/camera_section.py`,
  `mac_app/tests/test_camera_section.py`; hunks in `core/audio/view.py`, `mac_app/audio_page.py`,
  `mac_app/pages.py`, `mac_app/kvm_bridge_app.py`, `mac_app/setup.py` (`"camera_section"`),
  `tools/i18n_extract.py` and `core/tests/test_i18n_coverage.py` (`SOURCES`).
- Inputs: §6.4 statuses, §7.2, §8, §9.1. Depends on: CW1 milestone 1. **Milestone 1 (first commit): `view.py` +
  `camera_fixtures.py` + `test_camera_view.py`** so CW8 can start.
- Acceptance: `test_camera_view`, `test_audio_view` and `test_i18n_coverage` (stale-catalogue rule above) pass;
  offscreen renders (as `mac_app/tests/design_offscreen.py`) of 8 fixture states (off; idle; streaming to lebig
  with app name; permission undetermined chosen from the PC; sharing off; PC not installed; reduced; denied) in
  light and dark, reviewed by the operator; the menu shows the sharing item in both states and "Allow Camera
  Access…" only while `needs_permission`, and they call `set_camera_sharing` / `ask_camera_permission` (fake
  service); no main-thread call > 16 ms over a 60 s run with snapshots at 4 Hz.

### CW8 Qt Camera module (PC agent)
- Goal: §8.3, §8.4, §8.6 in `win_app` (Windows; on Linux the module stays hidden): install/update/repair/remove
  actions wired to `win_app/vcam_install.py`, the restart line, the static multi-app help line and its
  `ms-settings:camera` link.
- Owns: `win_app/camera_section_win.py`, `win_app/tests/test_camera_section_win.py`; hunks in
  `win_app/audio_page_win.py`, `win_app/pages_win.py`.
- Inputs: §8, §9.2, `win_app/widgets.py`, `win_app/theme.py`. Depends on: CW7 milestone 1 (never edits
  `view.py`, asks CW7), CW9's `vcam_install.py` API (mock until it lands).
- Acceptance: offscreen renders (`QT_QPA_PLATFORM=offscreen`) of the same 8 states plus "restart needed", light/
  dark; install button calls `vcam_install.install()` with mocks and shows the cancelled state; the help link
  calls the URL opener with `ms-settings:camera` (mock); keyboard reaches every control; the page still opens
  with Ctrl+3; `win_app` suite otherwise unchanged.

### CW9 Packaging, install, permissions (Mac agent; PC over ssh)
- Goal: §9: plist keys, entitlements, TCC reset, DLL shipping on Windows (Inno, portable, Install-Beamer.ps1)
  in `camera\`, `win_app/vcam_install.py` (state, UAC launch, owner SID, wait, remove sequence), uninstall
  (`[UninstallRun]` `vcam-remove` + `Install-Maintenance.ps1` camera removal and stale-folder retry), README
  one-liner for dev installs.
- Owns: `mac_app/{entitlements.plist,entitlements-audio.plist,Install-Beamer-Mac.command,build_dmg.sh}` (camera
  hunks), `mac_app/setup.py` (plist keys), `win_app/{build_win_app.ps1,Install-Beamer.ps1,Beamer-Setup.iss,build_portable.spec,Install-Maintenance.ps1}`
  (camera hunks), new `win_app/vcam_install.py`, `win_app/tests/test_vcam_install.py`; the dev-install section
  of `native/audio/vcam-win/README.md` (agreed with CW5, who owns the file).
- Inputs: §9, §2.5 outputs. Depends on: build outputs (skips the DLL with a warning while CW5's build script
  is missing, as audio WP8 did), CW4 verbs for the live check.
- Acceptance: `dev/deploy.sh` both succeeds; `codesign -d --entitlements -` shows `device.camera` on the app,
  `python` and `MacOS/beamer-audio`; `plutil -p Info.plist` shows both camera keys; Inno and
  `Install-Beamer.ps1` installs have `%LOCALAPPDATA%\Beamer\camera\BeamerCameraSource.dll`; **the portable build
  has `camera\BeamerCameraSource.dll` next to `beamer-audio.exe` (not under `_internal`)**; `vcam_install.py`
  unit-tested (verb line, SID lookup, cancel = error 1223, versions, remove order) with mocks;
  `Install-Maintenance.ps1 -Remove` camera step tested on lebig over ssh against a dev install (keys and folders
  gone, FrameServer stopped only when locked). H6 (real UAC install) and H7 (uninstall) are human checks.

### CW10 Lip sync (Mac agent; PC part over ssh)
- Goal: §4.9 policy in the PC helper (hold computation, smoothing, application in the presenter), CABLE render
  latency constants per mode, stats, dev offset.
- Owns: `native/audio/engine/sync.{h,cpp}` (policy part, after CW2 merges), the presenter hook in `video_recv.cpp`
  (agreed with CW2's owner), `native/audio/tests/sync_tests.cpp` (added to `build-tests.*` by a one-line hunk
  once CW2 merged), E2E case `cam_lipsync` in `dev/camera/e2e/` (agreed with CW11).
- Depends on: CW2, CW4 merged (CW3 for the live case), CW6 (`sr` on the mic stream).
- Acceptance: unit tests on synthetic delay pairs (hold within ±2 ms of the formula, bounds, smoothing rate,
  invariance to a ±20 ms offset error applied to both streams); `cam_lipsync` within -20..+40 ms with the mic in
  shared and in low-latency mode.

### CW11 Two-machine E2E harness and soak (Mac agent with ssh)
- Goal: §10.5 (all cases except `cam_lipsync`'s analysis, which CW10 adds).
- Owns: `dev/camera/e2e/**` (may import `dev/audio/e2e/*` and C4's `dev/camera/proto/e2e/*`; edits neither).
- Depends on: CW1-CW6 and CW9 merged (the §7.4 dev commands are CW6 + CW1); CW7/CW8 not needed.
- Acceptance: every case runs unattended from the Mac and writes `runs/<case>/result.json`; pass marks of §10.5
  met or each miss explained with its trace; results table handed to the operator for `STATUS.md`.

### Dependency summary

| WP | Machine | Wave | Can run concurrently with | Needs before it can finish |
|---|---|---|---|---|
| CW1 Python control plane | Mac | 1 | all wave 1 | - |
| CW2 Engine video transport | Mac (+PC build) | 1 | all wave 1 | - |
| CW3 Mac camera backend | Mac | 1 | all wave 1 | CW2 `video_backend.h` (first commit); CW2 for the full helper; H1 items: CW9 + an operator deploy |
| CW4 Windows video sink | PC | 1 | all wave 1 | CW2 `video_backend.h`, CW5 `shm_protocol.h`; CW2 + CW5 for live checks |
| CW5 Source DLL | PC | 1 | all wave 1 | - |
| CW9 Packaging | Mac (+PC) | 1 | all wave 1 | CW3/CW4/CW5 build outputs for the full result |
| CW6 Link + service integration | Mac (+PC) | 2 | CW7, CW8 | CW1 |
| CW7 Page model + Mac UI | Mac | 2 | CW6, CW8 | CW1 milestone 1; CW6 for live use |
| CW8 Qt module | PC | 2 | CW6, CW7 | CW7 milestone 1; CW9 `vcam_install.py` |
| CW10 Lip sync | Mac (+PC) | 3 | CW11 | CW2, CW4, CW6 (CW3 for live) |
| CW11 E2E + soak | Mac (+ssh) | 3 | CW10 | CW1-CW6, CW9 |

Two Mac agents and two PC agents never edit the same file: the only multi-owner file is `mac_app/setup.py`
(CW6 `includes`, CW7 one `py_modules` line before `"audio_page",`, CW9 plist keys: non-adjacent hunks), as in
audio; `locales/` belongs to the merge step. `core/audio/service.py` is CW6's alone; `core/audio/view.py` and
the audio pages are CW7/CW8's alone for their hunk; engine files are CW2's alone until it merges (CW10 after);
`native/audio/vcam-win/README.md` is CW5's, with CW9's dev-install section agreed.

## 13. C4 amendment points

When `reports/c4.md` lands, update: PHASE1 §1 (C4 table) and §2 (camera start, glass-to-glass); §4.7 start
budget and target; §4.8 total; §10.5 pass marks of `cam_start` and `cam_latency`; §11 risk 13; any change C4
forces in the recovery loop (§4.3 PLI classes and acks from clean runs, RR cadence §4.2) or the start sequence
(§4.7 step 2-4).
