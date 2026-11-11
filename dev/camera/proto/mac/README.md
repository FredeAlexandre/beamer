# beamer-camera (macOS): Phase-1 camera prototype (C1)

A standalone command-line tool that proves the Mac side of "Beamer Camera". It covers camera
inventory and permission, AVFoundation capture, a synthetic test source, and VideoToolbox H.264 in
low-latency mode to an Annex-B stream plus a per-frame trace. It also covers decode-back with PSNR and
simulated loss with LTR (long-term reference) recovery. It does not touch the Beamer app. Results and
verdicts are in `../../reports/c1.md`.

ObjC++ (AVFoundation and VideoToolbox are ObjC/C APIs, and the audio helper's Mac backend is ObjC++
too), built with the Command Line Tools only:

```
./build.sh                 # -> build/beamer-camera (universal, ad-hoc signed, hardened runtime,
                           #    com.apple.security.device.camera); ARCHS=arm64 ./build.sh for one arch
```

## Interface

```
beamer-camera list [--json]          cameras, every format (size, fps ranges, subtype, which video
                                     effects it supports), effect state, camera permission
beamer-camera auth [--request]       camera permission; --request asks, waits at most --auth-timeout S
beamer-camera encoders               H.264 encoders, the properties each session type supports, presets
beamer-camera run [options]          capture -> encode -> Annex-B + trace
```

`run` options (defaults in brackets):

| option | meaning |
|---|---|
| `--source synthetic\|camera` [synthetic] | test pattern, or an AVFoundation camera (`--device UID\|name`, default the system-preferred camera) |
| `--size 1280x720` `--fps 30` `--seconds 10` | format; the camera format is the one with that size, that rate and subtype `--pixfmt` [420v], or `--format-index I` |
| `--bitrate 2500000` | `AverageBitRate`; `--bitrate-at f:bps,...` changes it live at those frames |
| `--out FILE\|-` `--trace FILE` | Annex-B stream (SPS+PPS before every IDR); per-frame JSON lines |
| `--profile chigh\|high\|main\|cbaseline\|baseline` [chigh] | `ProfileLevel` (`*_AutoLevel`) |
| `--no-lowlatency` | without `kVTVideoEncoderSpecification_EnableLowLatencyRateControl` |
| `--no-realtime` `--speed` `--preset vc\|hs\|bal\|hq` `--keyint N` `--limit F` `--max-qp N` | other VT knobs (`--limit`: `DataRateLimits` = F x bitrate per 1 s) |
| `--force-key-every N` `--force-key-at a,b` | `kVTEncodeFrameOptionKey_ForceKeyFrame` |
| `--ltr` `--ltr-refresh-at a,b` `--ltr-ack-delay N` [3] | `EnableLTR`; a simulated receiver acknowledges each LTR token N frames later if it decoded that frame cleanly; `ForceLTRRefresh` at those frames |
| `--verify` `--verify-drop a,b` `--verify-from N` | decode every frame back in-process (VTDecompressionSession) and compute PSNR-Y/UV against the source; withhold frames from that decoder (simulated loss; the simulated receiver's LTR acks follow it) |
| `--hw` `--encoder-id ID` `--prop Key=Value` | require a hardware encoder; force an encoder; set any VT property by name (used for the private refresh keys) |
| `--pin-fps` | camera: `activeVideoMaxFrameDuration` = min = 1/fps (no frame-rate drop in low light) |
| `--cycles N --gap S` | camera: start/stop the session N times in one process (warm restarts) |
| `--no-encode` `--fast` `--noise N` [3] `--effects` `--stats S` [1] `--tag T` | capture only; synthetic not paced; synthetic sensor noise; probe video-effect control |

stderr carries JSON-line events: `start` (format, encoder id, every property set with its OSStatus,
read-back of what the session uses), `permission`, `started`, `first_frame`, `cam_stats` /
`enc_stats` every `--stats` s, `bitrate_set`, `cam_drop`, `session_*`, `stopped`, and `summary`
(distributions of encode time, bitrates over 1 s and 200 ms windows, IDR/P sizes, PSNR, CPU, RSS).

### Trace (one JSON line per encoded frame, in output order = order in the Annex-B file)

`f` frame number, `cap_ns` capture time (camera: PTS converted to the host clock; synthetic: the
moment the frame was finished), `arr_ns` delivered to us, `in_ns` handed to the encoder, `out_ns`
output callback (all `mach_absolute_time` in ns), `size` Annex-B bytes of that frame (SPS/PPS included
on IDRs, so a reader splits the file by consecutive sizes), `key`, `forced`, `dropped`, `ltr_token`
(-1 = none), `ltr_refresh`, `enc_ms`, `cap_to_out_ms`, and with `--verify` `psnr_y`, `psnr_uv`, `dec_ms`.

### Synthetic source and its code strip

NV12 video range, content meant to cost an encoder roughly what a webcam does: a moving gradient, a
moving textured disc, a scrolling text-like panel and per-pixel temporal noise (±3 luma steps).
Everything but the code strip is a function of the frame number. Rows `0..B-1` (B = width / 80: 16 px
at 720p, 24 px at 1080p) hold 80 blocks of B x B pixels, Y = 235 for 1 and 16 for 0, most
significant bit first:

| bytes | content |
|---|---|
| 0 | sync `0xA5` |
| 1-4 | frame number, u32 big-endian |
| 5-8 | host time of capture in units of 100 µs (`mach_absolute_time` ns / 100000), u32 big-endian, wraps |
| 9 | XOR of bytes 1-8 |

Read it by sampling the centre pixel of each block of the decoded Y plane (threshold 128):
`experiments/check_stream.py` does it, and every frame of the committed recordings decodes with the
right number and timestamp at 2.5 and 5 Mbit/s.

## Recordings (`recordings/`, synthetic, MacBook Air M5, macOS 27.2.1)

All low-latency mode, Constrained High (ffprobe: High, level 3.1 / 4.0), one IDR at frame 0, LTR on with
a simulated receiver acknowledging after 3 frames.

| file | content |
|---|---|
| `synth_720p30_2500k_30s.h264` + `.trace.jsonl` | 900 frames, 2.49 Mbit/s, 9.3 MB |
| `synth_1080p30_5000k_8s.h264` + `.trace.jsonl` | 240 frames, 4.99 Mbit/s, 5.0 MB |
| `synth_1080p30_5000k_30s.trace.jsonl` | trace only of a 30 s 1080p run (the stream, 18.6 MB, is not committed: `run --size 1920x1080 --bitrate 5000000 --seconds 30 --ltr`) |
| `synth_720p30_ltr_recovery_6s.h264` + `.trace.jsonl` | 180 frames. The encoder's simulated receiver lost frames 90 and 91, so frame 94 is an LTR refresh (14 KB, P) referencing a frame it had acknowledged; frame 150 is a forced IDR. A decoder that skips frames 90-91 must show a clean picture again from frame 94 on (libavcodec does, bit-exact; `check_stream.py ... --drop 90,91`) |

## Experiments

```
experiments/runner.py OUT.jsonl TAG -- run ...      # one run + CPU of VTEncoderXPCService/VTDecoderXPCService
experiments/intra_refresh.sh "MaxRefreshFrameIntervalDuration=1" ...   # private refresh keys under loss
experiments/check_stream.py STREAM TRACE [--drop a,b]                   # ffmpeg decode, code strip, loss heal
experiments/camera_suite.sh [OUTDIR]     # real camera: cold/warm start, latency, fps, sharing, effects (~3 min)
experiments/low_light.sh [OUTDIR]        # needs a person covering the lens
```
