# beamer-audio (macOS): Phase-1 audio prototype

A standalone command-line tool that proves the Mac side of carrying audio between Beamer
machines: device inventory and hot-plug, microphone capture, system-sound capture through a
Core Audio process tap (with local mute), and low-latency playback. It does not touch the
Beamer app. The Windows tool mirrors this interface.

Swift + a small C real-time core. The C code holds the lock-free ring buffers and the two
`AudioDeviceIOProc`s, so nothing on the HAL IO thread touches the Swift runtime. It needs
macOS 14.2+ (process taps) and builds with the Command Line Tools only.

```
./build.sh                 # -> .build/beamer-audio  (.build/ is gitignored)
```

## Interface

```
beamer-audio list [--json] [--verbose]
beamer-audio watch
beamer-audio capture-mic    [--device UID|NAME] [--rate 48000] [--channels 1]
beamer-audio capture-system [--device UID] [--mute-local] [--rate 48000] [--channels 2]
                            [--exclude-pid PID]... [--exclude-wait 5]
beamer-audio play           [--device UID|NAME] [--rate 48000] [--channels 2]
beamer-audio measure props|buffers|tap-latency|loop-latency|drift [...]
```

- Audio on stdout (capture) and stdin (play) is raw **f32le interleaved** at the requested rate
  and channel count. When the device differs, the channels are mapped (downmix to mono is an
  average, mono to N is a copy) and the rate is converted with `AudioConverter`.
- Diagnostics go to stderr as **JSON lines**: `start` (device, formats, granted buffer, latency
  properties), `first_callback`, `stats` every `--stats S` seconds (default 5; 0 = off; includes
  RMS/peak dBFS of the captured audio), `overrun`, `underrun`, `epipe`, `signal`, `cleanup`, and
  `exit` (CPU %, RSS, frame counters).
- Shared options: `--buffer-frames N` asks for an IO buffer size (the granted size is reported).
  For `play`: `--buffer-ms 80` (ring capacity) and `--prefill-ms 20` (fill level before starting,
  and again after an underrun).
- `list --json` gives `default_input`, `default_output`, `default_system_output` and, per device:
  uid, name, manufacturer, transport, nominal_rate, input and output channels, available rates,
  clock domain and alive status. Add `--verbose` for latency and safety offset, buffer range,
  stream latencies and formats.
- `watch` prints a `snapshot` line, then `devices_changed` (added and removed devices),
  `default_input_changed`, `default_output_changed`, `default_system_output_changed` and
  `rate_changed`. It uses property listeners on `kAudioHardwarePropertyDevices`,
  `DefaultInput/Output/SystemOutputDevice` and each device's nominal rate.
- `capture-system`: a global stereo tap of all processes **except this one**
  (`CATapDescription(stereoGlobalTapButExcludeProcesses: [self])`), or with `--device UID` a tap
  of that output device's stream 0. The tap is private and wrapped in a private aggregate device.
  `--mute-local` sets `muteBehavior = .mutedWhenTapped`. Extra flags: `--muted-always`
  (`.muted`), `--include-self`, and `--clocked` (add the output device as clock sub-device; see
  "Tap aggregate" below).
- `--exclude-pid PID` (repeatable) also leaves those processes out of the tap, e.g. Beamer's own
  `play`, so received audio is not captured and sent back (the echo loop when both machines share
  system sound). Each PID is translated with `kAudioHardwarePropertyTranslatePIDToProcessObject` and
  added to the `CATapDescription` exclusion list. A process only has a Core Audio process object
  once it has made a Core Audio call, so each PID is waited for up to `--exclude-wait` seconds; one
  that is still missing is added later: a listener on `kAudioHardwarePropertyProcessObjectList`
  rewrites the live tap's `kAudioTapPropertyDescription` when it appears (`exclude_added` event).
  Core Audio has no process trees: every process to exclude must be named. Excluded processes are
  not muted by `--mute-local` (they are not tapped). Tested with a -40 dBFS tone in `play`: a plain
  tap read -43 dBFS RMS, the tap excluding the player -200 dBFS (digital zero); with the player
  started 2 s after the tap, the exclusion was added at 2.07 s, before its first sound.
- Cleanup: the IOProc is stopped, then the aggregate and the tap are destroyed. This runs on
  SIGINT, SIGTERM, SIGHUP, on stdout EPIPE (for example `| head -c N`) and at stdin EOF for
  `play`. If the process is SIGKILLed, the HAL removes the private tap and aggregate itself.
  This was checked: the local mute lifts at the moment of the kill (see below).
- `play` blocks on stdin when its ring is full. A producer faster than real time is therefore
  paced by the device. The ring fill and underruns are reported.

Examples:

```
beamer-audio capture-mic --rate 16000 > mic.f32
ffmpeg -i x.wav -f f32le -ar 48000 -ac 2 - | beamer-audio play --device BuiltInSpeakerDevice
```

## Results (MacBook Air, macOS 27.0 build 26A428, 2026-11-07)

Devices: `MacBook Air Microphone` (`BuiltInMicrophoneDevice`, 1 ch in, 48 kHz) and `MacBook Air
Speakers` (`BuiltInSpeakerDevice`, 2 ch out, 48 kHz). Both are built-in, in the same clock domain
(1835100526), and support 44.1/48/88.2/96 kHz. All IO runs as float32 interleaved. No
permission dialog appeared for the microphone, the process tap or playback.

### IO buffer sizes (`measure buffers --sizes 128,256,480,512`)

| requested | granted (mic / speakers) | callback frames observed | callback period |
|---:|---|---|---:|
| 128 | 128 / 128 | 128 | 2.67 ms |
| 256 | 256 / 256 | 256 | 5.33 ms |
| 480 | 480 / 480 | 480 | 10.00 ms |
| 512 (default) | 512 / 512 | 512 | 10.67 ms |

The allowed range is 15 to 4096 frames on both devices. The tap aggregate honours the same sizes.

### Latency properties (`measure props`), in frames at 48 kHz

| device | device latency | safety offset | stream latency | total |
|---|---:|---:|---:|---:|
| Microphone (input) | 26 | 48 | 1919 | 1993 (41.5 ms) |
| Speakers (output) | 60 | 48 | 1280 | 1388 (28.9 ms) |

The large stream latencies come from the built-in DSP (microphone array processing and speaker
protection).

### Measured latencies (`measure tap-latency` / `measure loop-latency`)

The tool plays a click every 500 ms through its own output IOProc, using the same ring and
prefill path as `play` (prefill 10 ms, ring kept at the prefill level the way a live stream
would be). It detects the click in a capture IOProc and then maps the click's position in the
ring and the detected frame back to host time, using the per-callback `AudioTimeStamp`s and the
`inNow` of each callback. Each value is the median of 8 to 12 clicks; the spread was at most
±0.1 ms except where noted.

| path | buffer 512 | 256 | 128 |
|---|---:|---:|---:|
| **Tap**: output IOProc renders click -> tap IOProc delivers it (`render_to_deliver`) | 21.3 ms | 13.3 ms | 8.0 ms |
| Tap: write into `play` ring -> tap delivery (`push_to_deliver`, includes 10 ms prefill) | 32.0 ms | 24.0 ms | 18.6 ms |
| Tap: tap frame timestamp minus output presentation timestamp | +0.67 ms (clocked aggregate), -1 ms (tap-only) | same | same |
| **Acoustic loop** speakers -> air -> mic: render -> mic delivery | 66.8 to 72.8 ms (*) | 55.5 ms | 51.4 ms |
| Acoustic loop: write into `play` ring -> mic delivery (`push_to_deliver`) | 77.4 ms | 66.2 ms | 62.1 ms |
| Acoustic loop: mic capture timestamp minus speaker presentation timestamp | 44.2 ms | 44.2 ms | 44.1 ms |
| Output: callback time -> HAL presentation timestamp | 17.7 ms | 8.3 ms | 4.3 ms |
| Input: mic capture timestamp -> delivery to our IOProc | 4.9 ms | 3.7 ms | 2.9 ms |

(*) At 512 frames the value depends on the phase between the input and output IO cycles
(two runs gave 66.8 and 72.8 ms).

Interpretation:
- **Capture -> play on this Mac (mic in, speaker out) is 62 ms at 128 frames and 77 ms at
  512 frames**, including the 10 ms prefill of the playback ring. This is the local part of the
  mic -> remote-speaker budget; add the network time and the remote side's playback path.
- **System sound reaches us 8 ms (128) to 21 ms (512) after the app renders it**, which is about
  two to three IO buffers. The tap stamps its frames with practically the output presentation
  time.
- The hardware and DSP latency that the HAL timestamps leave out is a constant **44.2 ms**
  (speaker plus mic plus about 0.1 ms of air). The reported latency + safety offset + stream latency add up
  to 70.4 ms (1388 + 1993 frames), so the reported properties **overstate** the real latency by
  about 26 ms. Measure; do not trust the properties.

### Clock drift (`measure drift --seconds 330`)

The mic was captured and the speakers played silence side by side for 330 s.

| | microphone | speakers |
|---|---:|---:|
| rate from (sample time, host time) regression | 48000.099 Hz | 48000.099 Hz |
| rate from frames delivered vs callback host time | 48000.100 Hz | 48000.099 Hz |
| vs host clock | +2.07 ppm | +2.07 ppm |
| frames in 330 s | 15,840,256 | 15,840,256 |

**Mic vs speakers: 0.000 ppm from timestamps, 0.005 ppm from frame counts, and the frame counts
are identical.** The two devices share one clock, so a local loop needs no drift correction.
Between machines it will: the Mac's audio clock is +2 ppm against its own host clock, and the
remote device's clock will differ by an unknown amount (typically tens of ppm). Phase 2 needs
adaptive resampling in `play`, steered by the ring fill level.

### CPU (`exit` event over 30 s; buffer 512; Apple Silicon MacBook Air)

| command | own process CPU | RSS |
|---|---:|---:|
| capture-system --mute-local (48 kHz stereo) | 0.5 % | 16 MB |
| capture-system --rate 44100 --channels 1 (resampled) | 0.5 % | 16 MB |
| capture-mic (48 kHz mono) | 0.4 % | 15 MB |
| capture-mic --rate 16000 (resampled) | 0.4 % | 16 MB |
| play (48 kHz stereo) | 0.3 % | 15 MB |
| play --rate 44100 --channels 1 (resampled + upmix) | 0.5 % | 16 MB |

coreaudiod CPU (delta of `ps time` over 20 s): about 2.4 % at idle, about 2.6 % with the default
tap-only `capture-system`, **13.9 % with `--clocked`**, and 10.9 % while any client plays silence
to the speakers. The startup time from launching `capture-system` to the first tap buffer is
about 70 ms, including process start and tap creation.

### Local mute (`experiments/mute_test.sh`)

The test plays a 1 kHz tone with `afplay` (another process, so it is inside the global tap) to
the default output at the current system volume (56 %, unchanged). Meanwhile it captures the
tap and the built-in microphone, which hears the speakers acoustically. "Tone band" is the
level within ±20 Hz of 1 kHz.

| run | tap capture, tone band | mic, tone band | mic tone SNR |
|---|---:|---:|---:|
| baseline (no tone) | n/a | -96.8 / -89.0 dBFS | 0 dB |
| tap unmuted | -24.2 dBFS | **-43.8 / -43.0 dBFS** | 44 / 41 dB |
| tap `--mute-local` | **-24.2 dBFS** | **-96.4 / -91.4 dBFS** | -3 dB (noise) |

The two values in each cell come from two runs: first with the clocked aggregate, then with
tap-only. **With `--mute-local` the capture still carries the full signal, bit-for-bit the same
level as unmuted, while the speakers are silent:** the mic level stays at its noise floor,
about 50 dB below the unmuted run. The mute only applies while the tap exists. When
`capture-system --mute-local` was SIGKILLed 3 s into a 6 s tone, the mic heard the tone again
from the next second on (-92 to -88 dBFS while muted, then -43 dBFS).

## Tap aggregate: tap-only by default

Two findings while building the tap:
1. `kAudioAggregateDeviceTapAutoStartKey = true` makes `AudioDeviceStart` wait for the first
   tapped audio. With it, the IOProc got **zero callbacks while the system was silent** (the
   stream started only when `afplay` played). It is set to false, so the stream (silence
   included) runs from the first ~70 ms.
2. With the output device as the aggregate's main sub-device (`--clocked`, the usual sample
   code layout), our IOProc keeps the speakers running even when nothing plays. That costs
   coreaudiod about 11 points of CPU (the speaker DSP) and keeps the amplifier on. A **tap-only**
   aggregate (no sub-device) runs on the host clock (`mRateScalar` 1.0) with the same latency
   and the same capture and mute results, at about 0.1 to 0.4 points of coreaudiod CPU. It is the
   default.

## Experiments and how to reproduce

```
./build.sh
.build/beamer-audio list --json --verbose
.build/beamer-audio measure props
.build/beamer-audio measure buffers --sizes 128,256,480,512
.build/beamer-audio measure tap-latency --seconds 6 --mute-local --buffer-frames 128   # silent
.build/beamer-audio measure loop-latency --seconds 6 --buffer-frames 128               # audible clicks (amp 0.1)
.build/beamer-audio measure drift --seconds 330
experiments/mute_test.sh /tmp/mute                                                      # audible 1 kHz tone, 2 x 4 s
swiftc -O experiments/hotplug_sim.swift -o .build/hotplug_sim   # simulated hot-plug for `watch`
```

`watch` was checked with `hotplug_sim`. It creates a public aggregate device (built-in mic),
makes it the default input for 1 s, restores the original default input (verified) and destroys
the aggregate. `watch` printed `devices_changed` (added), `default_input_changed` twice and
`devices_changed` (removed). Real USB or Bluetooth hot-plug was not tested, because nobody was at
the machine.

## Caveats and open points

- **Feedback:** by default `capture-system` excludes only its own process, so another
  `beamer-audio play` process on the same machine is captured and `capture-system | play` loops,
  even with `--mute-local`. `--exclude-pid` of the player closes that loop; the two-machine test is
  in `../scenarios/README.md` (scenario 5).
- **TCC:** everything ran without a dialog because the permission is attributed to the parent
  app. If system-audio recording were denied, a tap typically delivers silence instead of an
  error. The `stats` RMS (-200 dBFS = digital zero) is the way to notice. This was not tested.
- **No drift compensation in `play`** yet. With a remote clock the ring slowly fills or drains;
  an underrun re-primes to the prefill level and is reported. A producer that is too fast is held
  back by stdin backpressure, so latency grows up to `--buffer-ms`.
- The resampler is not flushed at stdin EOF: about 18 frames (0.4 ms) of tail are lost when
  `play` resamples. This does not matter for streams.
- `--device UID` for `capture-system` taps stream 0 of that device only.
- Latency numbers are medians of 8 to 12 clicks per configuration, from one machine on one day;
  the acoustic loop ran at 56 % system volume with -20 dBFS clicks.
- In the clocked mode the tap is drift-compensated into the device clock. In tap-only mode it
  follows the host clock (the device is +2 ppm from it), so the drift is tiny but non-zero.
