# CW3 report: macOS camera backend

Date 2026-11-11. Branch `feat/camera`. Mac agent. Contract: `dev/camera/PHASE2.md` §2.4, §3.3, §4.3, §4.11, §12 CW3.
Commits: `0082992` (backend + harness + build hunk), `94ac57c` (encoder wake, camera_ms, synthetic picture,
idle probe), `c6b5938` (camera format kept across startRunning, bitrate metric), `e8693d1` (`main.mm` hunk),
plus this report's commit. All builds and runs used private exports of HEAD (`git archive` + my files) under the
session scratchpad. Nothing was deployed, Beamer.app was not touched, no system setting changed, and
`requestAccessForMediaType` was never called.

## Files

| File | What |
|---|---|
| `native/audio/mac/video_backend_mac.h` | `make_mac_video_backend(options)` (`dev`, `standby=session|prewarm`), internal API shared by the files below and the harness |
| `native/audio/mac/video_backend_mac.mm` | `VideoBackend` + `CameraHandle`: shared capture per device, one VT session per stream, frame number -> LTR token map, refresh/IDR/bitrate/fps/format/pause, standby adoption and re-creation, permission guard, `permission_changed` polling (every 2 s while undetermined), preferred-camera follower (KVO + connect/disconnect, debounced 300 ms), camera loss, encoder wake |
| `native/audio/mac/camera_mac.mm` | inventory (`cameras`), transport words, preferred camera, permission words, `CameraSource` (AVCaptureSession, format by §4.11, interruption / runtime error retry every 2 s, loss, `effects` / `by_other` polling every 1 s with `@available` guards), async stop |
| `native/audio/mac/scale_mac.mm` | `crop_16x9`, `choose_format` (pure), `Scaler` (one `VTPixelTransferSession`, crop via the source's clean aperture, NV12 pool) |
| `native/audio/mac/videoenc_mac.mm` | `Encoder`: low-latency, hardware-required, RealTime, no reordering, Constrained High, `AverageBitRate`, `EnableLTR`; Annex-B with SPS/PPS before every IDR; `ForceKeyFrame`, `ForceLTRRefresh`, `AcknowledgedLTRTokens` |
| `native/audio/mac/synth_mac.mm` | `--dev` synthetic camera: C1's picture at any native size inside its 16:9 crop, geometry markers, C1 code strip sized for the output; `synth[:WxH][;start_ms=N][;noise=N]`; never touches AVCaptureDevice |
| `native/audio/mac/video_check.mm` | harness (built as `build/mac/video_check`): `selftest`, `cameras`, `run` (with VideoToolbox decode-back geometry/strip check), `ltr`, `warm`, `idle`, `idlecost`, `guard`; `--disclaim` runs any command as its own TCC responsible process |
| `native/audio/mac/main.mm` (hunk) | builds the video backend (`dev` when `--dev`; none with `--no-video` / `--video`), calls `run_helper(backend, video, ...)` |
| `native/audio/build-mac.sh` (hunk) | the five `.mm` in `MAC_BACKEND`, `CoreMedia CoreVideo VideoToolbox`, links `video_check` |

## Acceptance (this Mac: MacBook Air, macOS 27.2.1)

Unless noted, numbers come from `build/mac/video_check` in a private export.
"Undetermined" means the run used `--disclaim`: the child is its own TCC responsible process and its camera
permission reads `undetermined` (the agent host, T3 Code, has camera access granted since C1).

| Criterion | Result | Command, numbers |
|---|---|---|
| `cameras` lists "MacBook Air Camera" with native formats and `preferred` | **Pass** | `video_check cameras` and helper `cameras`: key `6C707041-05AC-0011-0004-000000000001`, `builtin`, `640x480, 1280x720, 1760x1328, 1328x1760, 1552x1552, 1920x1080, 1080x1920 @30`, `preferred: true`, permission read without a prompt, 0 capture-device objects |
| Synthetic 720p30: encode p95 <= 8 ms | **Pass** | `run --test synth --seconds 10 --disclaim`: capture -> AU p95 **5.8 ms** (p50 5.4); VT submit -> output p95 5.5 ms |
| 1 s bitrate within ±10 % | **Pass** | same run: max deviation of full 1 s windows **3.0 %** (2.4-7.1 % over all runs, incl. scaled sizes) |
| Helper CPU < 4 % of a core | **Pass** (see note) | Through the real helper (`beamer-audio --dev --backend null:`, 30 s sending to a second helper over 127.0.0.1): helper `cpu_pct` **2.14 %** mean (1.99-2.53), **synthetic drawing included**. `video_check` alone: pipeline 1.24 % plus 5.7 % for drawing the synthetic picture (6.9 % total); the drawing itself is 0.53 ms/frame at full clock (`selftest`), the rest is low clock speed at 30 bursts/s. Drawing is the camera stand-in, not pipeline (C4 counted it apart). |
| Native 1920x1440, 1760x1328, 1080x1920, 640x480 -> exactly 1280x720 and 1920x1080, crop per §4.11 within 2 px | **Pass** | `run --test synth:<native> --size <out> --verify --disclaim`, 8 combinations (+1552x1552, 1280x720, 1920x1080 also checked): every AU at the output size, decoded back with VideoToolbox: 0 decode errors, strip read on 100 % of pictures, **geometry max error 0-1 px** (frame edges and centre cross). The check is sensitive: in `selftest` a picture drawn 6 px off reads 36 px of error, the right one 0. Crop arithmetic: all §4.11 examples exact in `selftest` |
| Scaler p95 <= 2 ms | **Pass** | `scale_ms` p95 **0.91-1.88 ms** over the 8 scaled combinations |
| `request_ltr_refresh` after `acknowledge_ltr` -> non-IDR AU <= 40 KB (720p); without acks an IDR | **Pass** | `video_check ltr --disclaim`: before any ack -> IDR; after acks, with 3-4 frames lost at the receiver: refresh **18.2 KB** and **21.7 KB** (720p), **13.5 KB** at 1080p, non-IDR, each decoded to a newer, correct picture (strip) by a decoder that never got the lost frames |
| `on_frame` numbers map to the right tokens across `set_input_paused` and `set_format` | **Pass** | same run: 0 AUs during a 0.5 s pause, refresh after the pause decodes correctly; `set_format` 720p -> 1080p: next AU is an IDR at 1920x1080, no old-size AU after it, a refresh request before any new ack gives an IDR (old tokens forgotten), the refresh after new acks decodes correctly at 1080p; 0 bad refreshes of 6, 0 decode errors |
| Warm start: with the standby, `open_camera` -> first AU p95 <= 50 ms, `encoder` = `standby` | **Pass** | `warm --n 20 --disclaim`: standby **p95 36.7 ms** (p50 35.5), 20/20 `standby:0.0` |
| First session of a fresh helper reported (`cold`, ~150-175 ms) | **Pass** | `cold:` 97-155 ms session create (first AU 158-205 ms after open); `warm:` 1.0-5.0 ms create, first AU p95 53 ms |
| First-AU times after 1, 10 and 60 min idle, standby and pre-warming | **Done** | table below |
| Idle standby cost over 10 min (helper + `VTEncoderXPCService` RSS and CPU) and the variant chosen by §4.11 | **Done: standby ships** | table below, and Contract issue 1 |
| `stop` -> encoder invalidated <= 100 ms | **Pass** | handle destruction (session invalidated) **10-14 ms**; helper `stop` -> `stopped` event in the same millisecond as the reply |
| `set_bitrate` effective within 300 ms | **Pass** | `run --bitrate-at 3:<bps>`: 3 consecutive AUs within 30 % of the new per-frame budget **90-148 ms** after the call (2.5 -> 1.25 / 5 / 0.65 Mbit/s); the next 1 s carried 88-102 % of the new target |
| `set_format` -> IDR at the new size | **Pass** | `ltr` above |
| macOS 13 API level, `-Wunguarded-availability` clean | **Pass** | the six files with `-mmacosx-version-min=13.0 -Wunguarded-availability -Werror=unguarded-availability`, arm64 and x86_64: no diagnostic (the build itself keeps `-Werror=unguarded-availability-new`) |
| Universal binary | **Pass** | `lipo -archs`: `x86_64 arm64` for `beamer-audio`, `video_check`, `backend_check` |
| All of the above with permission `not_determined`, no prompt, 0 capture-device creations | **Pass** | every row above marked `--disclaim` printed `"permission":"undetermined"` and `"capture_device_creations":0` (counter = `AVCaptureDeviceInput` + `AVCaptureSession` objects created). No prompt can appear: neither `requestAccessForMediaType` nor a device input is ever created on these paths |
| `synth;start_ms=800` -> first AU 800 ±40 ms after `start_encoding()` | **Pass** | `run --test "synth;start_ms=800" --standby` x10: **805-823 ms** (camera_ms 801-813, 1 IDR each); 822 ms undetermined |
| `open_camera` on the real camera while not authorized fails `permission`, no prompt | **Pass** | `guard --disclaim`: permission `undetermined`, `open_camera` -> `permission: camera access is undetermined` in 0.00 ms, one `permission_changed` to the listener, 0 capture-device objects, permission still `undetermined` after; the synthetic source opens in the same process |
| Audio does not regress | **Pass** | `acceptance.py list`: 3/3 pass (`backend_check`, `beamer-audio list`, roles). The audible / virtual-mic tests were not run: Alex's running Beamer uses the virtual mic. No audio source file changed |
| Engine <-> backend through the helper | **Pass** | helper pair (`--backend null:`, sender with this backend + `--dev`, receiver `--video null:`): `ready.features` has `video1, camera_capture, camera_synth` (no `camera_synth` without `--dev`, and `send` with `test` is refused `bad_args`), standby adopted (`encoder_ms` 0.0), `running` 40-80 ms after `send`, `camera` events with native/format/fps/effects, clean `stopped` |
| Thread safety | **Pass** | ThreadSanitizer build of the backend + harness: `ltr`, `warm`, a scaled run with a bitrate step: 0 reports |

### Warm session: idle cost and first AU after idle

Idle cost, 10 min, `video_check idlecost --mode none|prewarm|session` (separate processes; the XPC service is
the instance that appeared with the process's session); and inside the real helper (`beamer-audio`, 60 s,
`video_warm on`):

| | process RSS / footprint | `VTEncoderXPCService` RSS / footprint | CPU (process + XPC) |
|---|---|---|---|
| no VideoToolbox use | 6.4 MB / 1.6 MB | none | 0.0002 % |
| pre-warm (session created + invalidated) | 29.7-33.2 MB / 7.2 MB | **still running after 10 min**: 27.7-33.1 MB / 4.9 MB | 0.0068 % + 0.0001 % |
| standby session | 30.1-33.0 MB / 7.1 MB | 27.7-33.5 MB / 5.9 MB | 0.0098 % + 0.0001 % |
| real helper, before `video_warm` | 15.5 MB / 5.1 MB | none | |
| real helper, standby | 31.3-33.4 MB / 7.3 MB | 26.8-33.4 MB / 5.2 MB | helper 0.08 % (its whole idle loop) |
| real helper, pre-warm | 31.8 MB / 7.5 MB | 31.5-31.6 MB / 8.0 MB | |

The standby costs ~0.01 % of a core. Its memory over the pre-warm fallback is ~1 MB footprint and ~0 RSS. Over
a helper that never touched VideoToolbox it is ~18 MB RSS / 2.2 MB footprint in the helper plus the XPC
service's ~30 MB RSS / 5-6 MB footprint (RSS is mostly shared framework pages). **Pre-warming keeps the same
XPC service and framework pages resident**, so it does not remove that cost. **Choice: the standby session ships**
(`standby=session`, default); `standby=prewarm` / `BEAMER_VIDEO_STANDBY=prewarm` remain selectable. Reasoning in
Contract issue 1.

First AU after idle (`video_check idle`; `open_camera` -> first AU, synthetic `start_ms=0`, `start_encoding()`
right after open; the process had streamed once before):

| idle | standby (shipped code) | pre-warm (shipped code) | standby, before the encoder wake | pre-warm, before the encoder wake |
|---|---|---|---|---|
| 1 min | 19.2 ms (`standby:0.0`) | 40.4 ms (`warm:1.2`) | 33.5 ms | 65.0 ms (`warm:4.2`) |
| 10 min | 40.4 ms (`standby:0.0`) | 38.1 ms (`warm:1.7`) | 25.3 ms | 69.6 ms (`warm:5.0`) |
| 60 min | IDLE60S | IDLE60P | OLD60S | OLD60P |

### Real camera (agent-shell identity, T3 Code's grant; not H1)

The agent host has camera access (granted after C1). I took four short runs, about 40 s of camera time in all,
measuring times only; no picture was stored. Beamer.app's own numbers stay H1.

| | 720p (before the format fix) | 1080p | 720p | 720p |
|---|---|---|---|---|
| capture start -> first frame (`camera_ms`) | 1218 ms | 723 ms | 742 ms | 749 ms |
| `start_encoding()` (+30 ms) -> first AU | 1044 ms | 646 ms | | 670 ms |
| capture -> AU (camera PTS to encoder output) p50 / p95 | 48 / 60 ms | 49 / 62 ms | | 46 / 56 ms |
| fps | 15.4 | 16.0 | 16.2 | 16.1 |
| last stream stopped -> capture session stopped | 210 ms | 37 ms | 20 ms | 35 ms |
| process CPU | 12.8 % | 9.9 % | 8.8 % | 9.1 % |

- The camera ran at ~15 fps (evening, dark room): auto exposure, as §4.11 / §11 open point 3 expected.
- **The macOS session reset the chosen format at `startRunning`** (it delivered 1920x1080 for a 1280x720
  choice, so the scaler ran). It is fixed by keeping the device locked until `startRunning` returns (`c6b5938`);
  after that it delivers 1280x720 and no scaler runs.
- CPU with the real camera is ~9 % of a core, most of it in AVFoundation/CoreMediaIO threads in our process
  (a dozen `ANEServicesThread`s: the Neural Engine is in use, likely reaction-gesture detection, on for T3
  Code). This is far above §11 risk 11's "+2-3 %". Check it in Beamer.app, where
  `NSCameraReactionEffectGesturesEnabledDefault = false` (CW9) applies: Needs Alex.

## Design notes (what the code does beyond the letter of §4.11)

- **Encoder wake.** An idle hardware encoder takes 35-65 ms longer for its first frame, whatever the session's
  age: the standby saves the session creation, not this. Measured with `run --standby --start-after-ms
  100..5000`: first-IDR encode 33-67 ms before the fix, 4-11 ms after. From `open_camera` until the stream's
  first real frame is submitted (≤ 20 times, i.e. ≤ 4 s), the handle encodes a black frame every 200 ms and
  drops its output inside `Encoder`. No AU reaches the engine, no frame number is used, and the first real
  frame is still a forced IDR. The first wake runs off the caller's thread (`open_camera` stays ~1 ms).
  Trade-off: when the first frame arrives within ~30 ms of the open (synthetic `start_ms=0` with an immediate
  `start_encoding`), it waits behind the wake frame, which is why `warm` shows a standby p50 of 35 ms instead of 17.
- **One frame -> many streams.** Every stream on a device gets the same captured `CVPixelBuffer`; the scaled
  picture is made once per frame and output size and shared.
- **Locks.** Every `EncoderSink` call is serialised under one recursive lock that no `CameraHandle` method
  takes. The engine may therefore call `request_*`, `acknowledge_ltr`, `set_bitrate`, `set_fps` and
  `set_input_paused` from inside `on_frame` / `on_camera_event` (its hard-cap rule calls `request_ltr_refresh()`
  in `on_frame`). It must not destroy the handle, or call `set_format`, from inside `on_frame`: `set_format`
  flushes the old session, whose output callback is the caller. The handle destructor returns only after the
  last sink call.
- **Events emitted:** `native` and `encoder` at open, `running` and `camera_ms` at the stream's first captured
  frame (`camera_ms` from capture start, or from the attach to an already running camera), `fps` every 1 s
  (captured frames in the last tick), `effects` (comma list, `""` = none) and `by_other` on change,
  `interrupted` / `resumed`. A lost camera goes to the backend: a following stream (`device ""`) switches to the
  new preferred camera with an IDR; otherwise `lost` + `camera_lost(handle, "gone")`.
- **Synthetic picture.** C1's content, drawn in native pixels (an earlier version drew the text panel in output
  pixels; upscaled, it aliased so badly that VideoToolbox dropped 40 % of frames at 1080p / 5 Mbit/s). Black
  outside the crop, 3-output-px white frame and centre cross, and the strip on the crop's first rows with 80
  blocks across the crop width, so after crop + scale it is C1's strip at the output size.

## Contract issues

1. **§4.11 standby rule.** Read literally against a VideoToolbox-free helper, the standby's RSS (~18 MB helper
   + ~30 MB XPC) exceeds "30 MB beyond the helper's resident set". Its footprint (~7.5 MB) and CPU (~0.01 %)
   do not. The rule's fallback assumes pre-warming has "no idle cost", but measured, pre-warming keeps the
   same XPC service and framework pages resident (XPC still alive after 10 min; same RSS in the real helper),
   and it is 20-50 ms slower to the first AU. What the standby session itself costs over the fallback is ~1 MB.
   I read the rule as "the cost of keeping the session" and ship the standby; pre-warm is one option away if
   the operator reads it otherwise. Only releasing all VideoToolbox state when `warm` clears would save the
   memory: the XPC service exits only with its client process.
2. **`camera_synth`** is added by the engine (CW2, `helper.cpp`) when `--dev` and `camera_capture` are present.
   The backend's `features()` returns only `camera_capture`, as §2.4 lists; otherwise the word would be
   duplicated. The backend still refuses `test` without its `dev` option, as a second check behind the
   engine's `bad_args`.
3. **`start_ms` reference.** §2.4 / §4.11 count it from capture start (`open_camera`), the CW3 acceptance from
   `start_encoding()`, and CW2's null backend `cam_start_ms` from `start_encoding()`. I implemented capture
   start, as a real camera behaves. With the standby, an immediate `start_encoding()` lands at 805-823 ms. In a
   real flow the two references differ by the `send` -> PLI time (10-30 ms).
4. **`systemPreferredCamera` is nil on this Mac** (one built-in camera, no user choice yet). "Preferred" falls
   back to `defaultDeviceWithMediaType:` and then to the first camera. Both the `preferred` key and the
   `preferred` flag report that device.
5. **Helper-level stream tests on one Mac**: CW2's null video sink cannot decode real H.264 (it expects the null
   encoder's AUs). A real-backend sender therefore sees `decoder` PLIs and one IDR per second, which inflates
   the through-helper bitrate (+8 %) and CPU. A clean real-AU receiver exists only on the PC (CW4/CW11).
6. **`set_format` inside `on_frame`** would deadlock (see Locks). The engine does not do it today; CW2 should
   keep it that way.

## Needs Alex

H1, after CW9's packaging and an operator deploy of Beamer.app:
- "Allow Camera Access…" -> `permission` goes `undetermined -> granted` without a helper restart (the backend
  polls every 2 s and re-reads after the answer; untested with a real prompt).
- In Beamer.app: camera start -> first AU (`camera_ms`, the agent shell saw 0.72-1.22 s), `stop` -> camera light
  off ≤ 500 ms (the agent shell saw 20-210 ms), and **helper CPU with the real camera** (9-13 % in the agent
  shell; is it lower with reaction gestures off?).
- Low-light frame rate (15-16 fps seen in the dark room): §11 open point 3.
- Whether video effects (Portrait, Studio Light, Center Stage) appear in the frames, and the `effects` list
  while they do.
- With a second camera (iPhone Continuity or USB): the preferred-camera switch in place, the `continuity` /
  `usb` transport words, and unplugging during a stream (`lost` / follow). Not testable here: one built-in
  camera.

## Notes for CW9 (packaging)

- What CW9 did matches what the backend needs: `com.apple.security.device.camera` on the app, its `python`
  and `beamer-audio`; `NSCameraUsageDescription`; `NSCameraReactionEffectGesturesEnabledDefault = false`.
  VideoToolbox, CoreMedia and CoreVideo need no entitlement; the inventory and the authorization status need
  no permission.
- The helper now links `CoreMedia`, `CoreVideo` and `VideoToolbox` (system frameworks, nothing to ship).
- Agent shells are attributed to T3 Code, which has camera access on this Mac. To test Beamer's own
  undetermined state without a deploy, use `video_check <cmd> --disclaim`.

## Notes for CW11 (E2E)

- Synthetic source: `{"test":"synth[:WxH][;start_ms=N][;noise=N]"}` (default native = the output size,
  `start_ms` 5, `noise` 3). The strip is C1's at the output size (block = width / 80, rows 0..block) for every
  native size. Its fields: the synthetic captured-frame counter (0 = first frame after capture start; frames
  before `start_encoding()` are consumed, so the first encoded strip is usually > 0, and it is **not** the
  engine's frame number), and `capture_ns / 100000` on the engine's `now_ns()` clock (= mach host time).
- Synthetic content at native ≠ output size is drawn in native pixels: upscaled sources look softer, as a
  real camera would.
- Expect `camera_ms` ≈ `start_ms`, the first IDR 4-11 ms after its frame (encoder wake), `encoder` `standby:0.0`
  while `video_warm` is on, and `warm:1-5` / `cold:100-160` otherwise.
- Real camera from an agent shell: first frame 0.7-1.2 s after `send`, 15-16 fps in the dark.
- `video_check` covers the Mac backend alone: `selftest` (pure rules, clock, strip, geometry sensitivity),
  `run --verify` (decode-back), `ltr`, `warm`, `idle`, `idlecost`, `guard`.
