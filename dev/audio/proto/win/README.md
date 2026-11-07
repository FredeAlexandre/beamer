# beamer-audio (Windows prototype, Phase 1)

A standalone WASAPI command-line tool that shows what Beamer's audio feature can rely on on Windows.
PCM on stdin/stdout is f32le interleaved. Diagnostics on stderr are one JSON object per line.

**Language: C++ with MSVC, no dependencies.** WASAPI, process loopback and IMMNotificationClient are
COM APIs that C++ reaches directly, including the parts `sounddevice` does not expose (process
loopback, endpoint mute, IAudioClock). The result is a 470 KB static exe with no runtime, so it can
ship next to Beamer.exe as a PyInstaller data file and be driven over stdio. The audio threads run
outside the Python GIL, which keeps Beamer's input path and audio from stalling each other.

## Build

```bat
dev\audio\proto\win\build.cmd
```

This finds the newest MSVC with vswhere and writes `build\beamer-audio.exe`.

## Usage

```
beamer-audio list [--json] [--all]
beamer-audio watch [--seconds S]
beamer-audio capture-mic [--device ID|NAME] [--rate 48000] [--channels 1] [--period-ms P] [--seconds S]
beamer-audio capture-system [--device ID|NAME] [--mute-local] [--rate 48000] [--channels 2] [--seconds S]
                            [--no-keepalive] [--process-loopback include|exclude --pid PID] [--exclude-pid PID]
beamer-audio play [--device ID|NAME] [--rate 48000] [--channels 2] [--period-ms P] [--pad-periods 2]
                  [--queue-ms 40] [--live-drop-ms N] [--seconds S]
beamer-audio restore-endpoint
beamer-audio measure periods|mute-matrix|latency|latency-stdio|glitch|drift|level|tone|gen|pcm-stats|burn|default-swap
```

- `--device` takes an exact endpoint id, else a case-insensitive name substring, else the default
  (console role) when omitted.
- Exit codes: 0 ok, 1 failure, 2 usage or device not found, 3 device invalidated mid-stream (restart
  the stream).
- `capture-*` stops when stdout breaks (tested). `play` drains and stops at stdin EOF.
- `play` blocks the producer when its queue is full. Pass `--live-drop-ms N` for a live source, to
  discard backlog above N ms instead.
- `--process-loopback exclude` without `--pid` excludes this process's own tree.
- `--exclude-pid PID` captures every process except the tree rooted at PID (process loopback,
  `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`), so audio that Beamer's own player renders is
  not captured and sent back (the echo loop when both machines share system sound). Windows takes one
  target tree only, so a second `--exclude-pid` is refused: Beamer starts its player(s) and this capture
  as children of one parent (the Beamer process that spawns them) and passes that parent's PID.
  Tested (-40 dBFS tone in the player, level in the capture):
  - player started before the capture, child of the excluded PID: -128 dBFS (included: -40);
  - player started 2 s after the capture: -168 dBFS;
  - grandchild (cmd -> cmd -> play, outer cmd excluded): -102 dBFS;
  - **a Python venv launcher is not followed**: excluding `venv\Scripts\python.exe` left the player that
    its base interpreter spawned audible (-40 dBFS), excluding the base interpreter silenced it
    (-95 dBFS); `--process-loopback include` of the launcher captured nothing. So pass the PID of the
    process that really spawns the helpers (`os.getpid()` inside Python), not a launcher's; check the
    same for a PyInstaller one-file bootloader.
  - with `--mute-local`: the endpoint is muted as usual (checked mid-run) and restored after.
  Process loopback is not tied to the endpoint: it captures every non-excluded process on every
  render device (here it also picked up a test tone another program was playing into VB-CABLE).

### Format handling

| Situation | Handling |
|---|---|
| Capture, mix channels > 2 | Stream every mix channel and fold down here (LFE dropped). WASAPI's own 7.1->stereo matrix lost 13.5 dB on stereo content. |
| Render, mix channels > 2 and differ from the request | WASAPI conversion. Stereo written directly into the 8-channel G733 mix came out 3.98 dB low at every level, cause unknown; through WASAPI conversion it is unity. |
| Rate differs from the mix | WASAPI SRC (`AUTOCONVERTPCM`). Checked at 16 kHz and 44.1 kHz. |

## Results (lebig, 2026-11-07, Windows 11 26200, session 0)

Devices: mic Elgato Wave:3, render Logitech G733 (mix 48 kHz, 8 channels). Raw data is in `results\`.

### Periods and buffers

| Measure | Render | Mic | Loopback |
|---|---|---|---|
| GetDevicePeriod default / minimum | 10 / 3 ms | 10 / 3 ms | 10 / 3 ms |
| IAudioClient3 engine default = fundamental = min = max | 480 frames | 480 frames | not used |
| Period actually granted, buffer | 10 ms, 1056 frames | 10 ms, 1056 frames | 10 ms, 1056 frames |
| GetStreamLatency | 0 ms | 0 ms | 0 ms |
| Event interval mean / SD / max | 10.00 / 0.24 / 10.67 ms | 10.00 / 0.07 / 10.39 ms | 10.00 / 0.24 / 10.63 ms |
| Packet age on receipt (now - packet QPC) | n/a | +10.2 ms | -9.9 ms |

- **The 3 ms minimum is not available.** IAudioClient3 offers only 480 frames on both drivers, so
  10 ms is the floor in shared mode on these devices.
- **GetStreamLatency reports 0 here**, so it cannot be used as a latency input.
- **Loopback packet timestamps are about 10 ms in the future**, so they are not a capture time.

### Latency

| Path | Queue | Median | p95 |
|---|---|---|---|
| Render write -> loopback packet in the same process | 2 periods | 40.0 ms | 40.6 ms |
| Render write -> loopback packet in the same process | 1 period | 30.0 ms | 30.6 ms |
| stdin of `play` -> stdout of `capture-system` | 2 periods, `--queue-ms 40` | 53.0 ms | 53.6 ms |
| stdin of `play` -> stdout of `capture-system` | 1 period, `--queue-ms 40` | 36.4 ms | 37.2 ms |
| stdin of `play` -> stdout of `capture-system` | 1 period, `--queue-ms 15` | 42.2 ms | 42.5 ms |

The last two rows differ by about 6 ms for no reason the queue setting explains, so differences of
that size between runs are probably start-up phase, not an effect.

**Not measured: acoustic end to end.** Loopback taps the engine, so USB, radio and DAC time are
outside every number above. The mic does not hear the headset (tone at -72 to -117 dBFS across runs,
with no relation to whether the endpoint was muted), so there is no acoustic path to time. The estimate for mic -> engine
output on this machine is about 30-40 ms: 10 ms mic packet age plus 20-30 ms of render queue and
engine, plus unknown device time. A person with a click and a microphone can close this gap.

### Glitches

Sine rendered and checked sample by sample on the loopback, 2.86 M frames per run:

| Queue | Idle, 60 s | All 12 logical cores spinning, 60 s |
|---|---|---|
| 2 periods | 0 holes, 0 jumps | 0 holes, 0 jumps |
| 1 period | 0 holes, 0 jumps | 0 holes, 0 jumps |

- **Negative control:** stalling the render thread 60 ms every 5 s for 21 s gave 4 holes and 5 jumps,
  so the detector does see missed deadlines.
- **Limit:** this sees the engine only, not glitches later in USB or the radio link.
- **One period leaves no margin:** every render wake found the buffer empty, and the stream survives
  only by writing within the 10 ms before the next engine pass. `play` therefore defaults to 2
  periods; 1 saves 10 ms but deserves a longer soak first.

### CPU

Cycle counts scaled by the nominal clock, 30 s, % of one logical core of a Ryzen 5 7600X.
`GetProcessTimes` read 0 for these short wake-ups, which is why cycles are used.

| capture-mic | capture-system (with keep-alive) | play |
|---|---|---|
| 0.09 % | 0.15 % | 0.10 % |

### Clock drift (360 s, `results\drift.jsonl`)

| Clock, by linear fit against QPC | ppm |
|---|---|
| Mic (Wave:3), packet position | -25.98 |
| Render (G733), IAudioClock | -26.24 |
| Loopback of the G733, packet position | -26.15 |
| **Mic minus render** | **+0.26** (0.93 ms per hour) |

- **The 0.26 ppm is a ceiling, not a rate.** The cumulative figure was still falling as 1/t at the end
  (37, 9.6, 4.2, 2.5 ... 0.26), which is the start-up transient fading. Most 30 s windows read about
  -26 ppm on both clocks, so a real difference is too small to resolve here.
- **Probable cause, not established:** two USB audio devices this close may both be following the
  USB host's frame clock.
- **Do not generalise this.** It says nothing about drift between two machines. 26 ppm, the offset
  against QPC here, would be 94 ms per hour, so the network path needs its own drift handling and
  the Mac-to-PC figure still has to be measured.

Single 30 s windows are noisy (mic -35 to +10 ppm, render -692 to -6 ppm, the -692 being the first
window), so use the whole-run fit and not a short window.

### Can system sound be silenced locally while still being captured?

**Yes: endpoint mute plus loopback.** A 997 Hz tone was sent at -13.98 dBFS from a child process with
its own session, and measured on each capture path. Same result on the G733 and the DualSense, two
different hardware-mute drivers (`results\mute-matrix*.jsonl`).

| Silencing method | Endpoint loopback | Process loopback, include tone | Process loopback, exclude other |
|---|---|---|---|
| None | -13.98 | -13.98 | -13.98 |
| **Endpoint mute** (IAudioEndpointVolume) | **-13.98** | **-13.98** | **-13.98** |
| Endpoint volume 0 | -13.98 | -13.98 | -13.98 |
| Endpoint volume 50 % (loopback only) | -13.98 | | |
| Session mute (ISimpleAudioVolume) | silent (< -170) | silent | silent |
| Session volume 0 | silent (< -170) | silent | not run |

Excluding the tone's own process also gave silence, which confirms exclude mode does what it claims.
Exclusion covers the target's process tree, so Beamer should exclude Beamer.exe's PID to cover a `play`
helper. Excluding a helper's parent was not tested as such: the first matrix run hit it by accident,
when excluding the harness also silenced its child tone.

**Options compared:**

| Option | Verdict | Trade-offs |
|---|---|---|
| Endpoint loopback + endpoint mute | **Works, used by `--mute-local`** | Mutes everything on that endpoint, including anything `play` sends there. Shows as muted in the tray, and a user unmuting there lets sound through. Needs a keep-alive stream. |
| Process loopback (exclude self) + endpoint mute | **Works, recommended for Beamer** | Same mute side effects, but does not capture Beamer's own playback, so remote audio cannot echo back. Needs Windows 10 2004+. |
| Endpoint volume 0 | Works, worse | Same effect as mute, but moves the user's volume slider. |
| Session mute or volume | **Does not work** | Applied before every loopback tap, so the capture goes silent too. |
| Virtual sink (VB-CABLE as default) | Not tested | Needs a driver and a default-device change. Left alone as the brief asked. |

**What `--mute-local` does about a crash.** Leaving a machine muted is the worst failure, so it saves
mute and volume to a journal and starts a guard process. Tested:

| Event | Result |
|---|---|
| Normal end | Restored |
| `Stop-Process -Force` of the capture | Guard restored it within 1 s |
| Capture and guard both killed | Stays muted. `restore-endpoint` restored it (tested). The next `--mute-local` run also restores from the journal first (in the code, not tested). |

**Still open on muting:**

- **Silence itself is inferred, not heard.** The API reports muted and the driver claims hardware
  mute, but nobody listened and the mic cannot hear the headset. A person should confirm on the G733.
- **Software-only mute is untested.** Every endpoint I could use reports hardware mute. The only
  software-mute ones here are Elgato's virtual devices, which were off limits. `--mute-local` warns
  on such an endpoint.
- **The endpoint meter is no evidence of silence:** it reads -13.98 under mute too.

### Other findings

- **Keep-alive is needed.** With nothing rendering to the DualSense, loopback delivered 0 frames in
  3 s without keep-alive and 144000 silent frames with it. The G733 kept delivering without one, so
  something else holds a stream open on it.
- **Process loopback is not tied to an endpoint.** It captured a tone on the DualSense while the
  G733 was the default. Capturing several endpoints at once was not tested.
- **`watch` needs de-duplicating.** Restoring the communications default also fired `default_changed`
  for console and multimedia with no actual change (state compared afterwards), so consumers should
  compare against the current default. Property events arrive too.
- **The Wave:3 is dual mono** (identical channels), so the mono mix-down loses nothing.
- **`capture-mic | play`** ran over pipes for 5 s with 0 underruns.

## Not verified

- **Hot-plug.** `watch` logged 12 real events (`default_changed`, `property_changed`) but no
  `added`, `removed` or `state_changed`, because nothing was plugged in during the run and VB-CABLE
  was installed before it started. They use the same callback interface, but a person should
  replug a device.
- **Device loss mid-stream.** The exit-code path (3) has not been exercised.
- **The user session.** Everything ran in session 0. Beamer runs in the user's session, so repeat a
  subset there. Default-device swap used undocumented IPolicyConfig, only as a test hook.
- **`default-swap` restores with no crash guard**, unlike `--mute-local`. It is a test hook; do not
  ship it.
- **The cause of the 3.98 dB render loss** when writing directly into the 8-channel mix.

## Reproducing

From this directory, with nothing else using the audio devices:

```powershell
build\beamer-audio.exe measure mute-matrix          # also --device DualSense
build\beamer-audio.exe measure drift --seconds 360
powershell -ExecutionPolicy Bypass -File results\run-experiments.ps1
powershell -ExecutionPolicy Bypass -File results\run-final.ps1
```

`results\level-test.jsonl` records the 3.98 dB finding on the build before the fix. Since then a plain
`tone` uses the WASAPI path, so its `direct_8ch` labels no longer describe what `level-test.ps1` runs.
These scripts play audible test tones at low level and mute endpoints briefly, restoring them after.
