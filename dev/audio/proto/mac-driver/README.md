<!-- SPDX-License-Identifier: GPL-3.0-only -->
# BeamerAudio.driver: the Mac virtual microphone

A Core Audio AudioServerPlugIn (HAL plug-in, runs inside coreaudiod) that makes a microphone
received from another machine show up on the Mac as a real input device that Zoom, Discord,
FaceTime... can pick.

Status: built, signed, staged and tested **out of process only**. Not installed yet (it needs
the `beamer-hal` root helper, which does not exist yet).

```
./build.sh   # universal bundle -> build/BeamerAudio.driver, copied to ~/Developer/beamer-hal-staging/
./test.sh    # build.sh + drives the bundle with test/test_host (plus ASan/UBSan and TSan builds)
```

Command Line Tools only (clang, lipo, codesign); no Xcode project.

## Design

One plug-in, two devices, one shared ring buffer (the classic loopback "virtual cable"):

| | Beamer Microphone | Beamer Mic Feed |
|---|---|---|
| Object ID (stream) | 2 (3) | 4 (5) |
| UID | `com.beamer.audio.mic` | `com.beamer.audio.micfeed` |
| Direction | input only | output only |
| `kAudioDevicePropertyIsHidden` | 0 | 1 |
| Can be default device | yes (input) | no |
| Can be default system device | no | no |
| Used by | apps that record | Beamer, which plays the received mic into it |

Common: manufacturer `Beamer`, model UID `com.beamer.audio.model`, transport type `virt`,
Float32 mono packed, 48 kHz (default) or 44.1 kHz, clock domain `'BEMR'` on both, no controls
(no volume/mute), latency 0, safety offset 0, zero-timestamp period 16384 frames.
Bundle: `CFBundleIdentifier com.beamer.audio.driver`, factory UUID
`D590F59B-06C8-4FA2-B549-9F9A6F1756BD` -> `BeamerAudio_Create` (the only exported symbol),
registered for `kAudioServerPlugInTypeUUID`.

**Clock.** Like Apple's NullAudio sample, the plug-in is its own clock, derived from
`mach_absolute_time`. Unlike NullAudio, the anchor host time is plug-in wide (set when IO starts
on either device while the other is idle) and `GetZeroTimeStamp` is a pure, lock-free function of
(anchor, now, rate). Both devices therefore report bit-identical zero timestamps: same timeline,
same clock domain, no drift, no resampling between feed and mic.

**Loopback.** A 16384-frame mono ring (64 KiB, static) indexed by absolute sample time:

* Feed `WriteMix` at output time `T` stores the frames at `ring[t & mask]`, then publishes
  `writeEnd = T + n` (release). If `T` jumped past `writeEnd` (first write, feed restarted,
  overload) the hole is zeroed first (at most one lap).
* Mic `ReadInput` at input time `S` returns a frame only if its time is inside
  `[writeEnd - 12288, writeEnd)`; anything else (not fed, feed stopped, gap, older than a lap)
  is silence. 4096 frames at the old edge are never trusted, so a writer running ahead cannot
  tear what the reader copies.
* Because the HAL's output time runs ahead of "now" and its input time behind, data written by
  the feed is read by the mic a little later: loopback latency is roughly
  feed lead + mic lag (in the harness, 2 x 512 frames = 21 ms at 48 kHz).
* When IO starts from idle on both devices the ring and `writeEnd` are reset.

**Change from the brief: no "zero what was read".** Stale replay is prevented by the
`writeEnd` window plus gap zeroing on the write side instead. Zeroing on read would make the mic
IO thread a second writer of the ring, i.e. a data race with the feed IO thread. With this design
the ring has exactly one writer; samples are relaxed 32-bit atomics (plain loads/stores on
arm64/x86_64) and `writeEnd` is acquire/release, so the code is race-free under TSan.

**Real-time safety.** `GetZeroTimeStamp`, `WillDo/Begin/Do/EndIOOperation`: no locks, no
allocation, no syscalls besides `mach_absolute_time`. A pthread mutex guards only non-IO state
(StartIO/StopIO counters, pending rate, `DeviceIsRunning`), as in NullAudio.

**Sample rate.** Changing the rate on either device (nominal rate or stream format) requests a
configuration change on *both* devices via `RequestDeviceConfigurationChange`; each device
applies it in `PerformDeviceConfigurationChange`. The two must share a rate because they share
the ring. Clients at other rates are resampled by the HAL as usual.

## How Beamer feeds it

1. `kAudioHardwarePropertyTranslateUIDToDevice` on `kAudioObjectSystemObject` with the qualifier
   `CFSTR("com.beamer.audio.micfeed")` -> `AudioDeviceID` (hidden devices are still translatable).
2. Open it as an output device: an AUHAL with `kAudioOutputUnitProperty_CurrentDevice`, or
   `AudioDeviceCreateIOProcID` + `AudioDeviceStart`. Render the received microphone (mono Float32,
   any rate; the HAL converts to the device rate).
3. Whatever Beamer renders appears on "Beamer Microphone". When Beamer stops, the mic outputs
   silence after the few frames already queued.

Beamer should never make the feed the default output; the driver reports
`CanBeDefaultDevice = 0` for it so macOS will not offer it.

## Test results (2026-11-07, macOS 27.0, Apple silicon)

`./test.sh` runs `test/test_host.c` three times (signed universal bundle, ASan+UBSan build, TSan
build); all three: **PASSED, 1526 checks, 0 failures** (about 26 s total).

What the host does, in coreaudiod's order:

* **Load**: `CFBundleCreate`, finds the factory through `CFPlugInTypes`/`CFPlugInFactories` in
  Info.plist, `dlopen`s the executable, calls the factory (rejects a wrong type UUID),
  `QueryInterface(kAudioServerPlugInDriverInterfaceUUID)`, `AddRef/Release`, `Initialize` with a
  stub `AudioServerPlugInHostInterface`.
* **Objects**: plug-in class/manufacturer, device list `[2, 4]` (clipped correctly to a short
  buffer), UID -> device for both UIDs and unknown -> `kAudioObjectUnknown`. Per device: name,
  manufacturer, UID, model UID, IsHidden (0/1), transport, can-be-default flags, alive, not
  running, ZTS period, streams only in their own scope, no controls, stereo pair 1/1, rates
  {48000, 44100}, nominal rate settable and name not. Per stream: class, owner, direction,
  starting channel, active, virtual/physical format Float32 mono 48 kHz, 2 available formats.
* **Sweep**: 52 selectors (incl. unsupported ones) x 5 objects x 3 scopes: `HasProperty` agrees
  with `GetPropertyDataSize`/`GetPropertyData`, data never exceeds the announced size, buffers are
  never overrun (256 supported combinations). Bad object, short buffer, StopIO without StartIO,
  StartIO on a stream: all rejected.
* **IO routing**: mic claims only ReadInput, feed only WriteMix, both in place.
* **Clock**: over 3+ periods, sample time advances exactly 16384 per period, host time is in the
  past and within one period, monotonic, mic and feed timestamps identical, measured rate
  48000.0000 Hz (44100.0004 Hz after the rate change).
* **Loopback semantics (synthetic timeline)**: silence when never fed; exact read-back at the same
  sample time; misaligned reads; silence past the write position; after one lap the old slots read
  as silence (no stale replay); feed stopped -> silence over 3 laps; randomised model check
  (20000 random writes with jumps, gaps > 1 lap and small rewinds, plus random reads):
  220016 frames compared, 0 mismatches.
* **Real-time loopback, two threads** (like coreaudiod's two device IO threads), 512-frame cycles,
  2 s, a 440 Hz sine indexed by absolute sample time: mic alone is silent; then 94720/96256 frames
  bit-exact at 48 kHz (87040/88576 at 44.1 kHz; the remainder is the silence read before the first feed write reached the mic, i.e. start-up
  latency), 0 corrupt, 0 dropouts; after StopIO on the feed the frames already written ahead
  drain, then 0 non-silent frames over 0.6 s (more than one ring lap).
* **Sample-rate change**: 22050 rejected without a config request; 44100 on the mic -> host asked
  to reconfigure both devices -> after Perform both report 44.1 kHz, stream formats follow; then
  clock and loopback re-tested at 44.1 kHz; back to 48 kHz.

Mutation check (done once by hand, not part of `test.sh`): removing gap zeroing, removing the
`writeEnd` window, or giving the two devices different anchors each makes the suite fail.

Not covered: the x86_64 slice is built and its exports checked, but not executed (Rosetta is not
installed on this Mac).

## What remains to verify once installed

Install = copy `~/Developer/beamer-hal-staging/BeamerAudio.driver` to
`/Library/Audio/Plug-Ins/HAL/` and restart coreaudiod (`beamer-hal install`).

1. coreaudiod loads it: `system_profiler SPAudioDataType` / Audio MIDI Setup shows
   "Beamer Microphone"; `log stream --predicate 'process == "coreaudiod"'` free of plug-in errors.
   Check that the self-signed "Beamer Dev" signature is accepted by the Core Audio driver service
   (ad-hoc signing is the fallback).
2. "Beamer Mic Feed" is hidden: absent from Sound settings output list and app pickers (Zoom,
   Discord, Chrome). Some apps enumerate all devices without honouring `IsHidden`; record which.
3. `TranslateUIDToDevice("com.beamer.audio.micfeed")` from a user process returns the hidden
   device, and Beamer can start IO on it.
4. End-to-end: Beamer plays a tone into the feed, an app records from the mic; measure latency
   (expect about 2 HAL buffers plus any safety offset). Tune `kAudioDevicePropertyLatency` /
   `SafetyOffset` (currently 0, as NullAudio) if apps report glitches or bad A/V sync, and check
   behaviour with large buffers (the ring tolerates feed lead + mic lag up to 12288 frames).
5. Feed and mic started/stopped in every order, apps joining mid-stream, Beamer quitting while
   Zoom records (expect silence, no loop of old audio), sleep/wake.
6. coreaudiod restart (`killall coreaudiod` via the helper) while Beamer feeds and an app records:
   both must reconnect; Beamer must re-translate the UID (object IDs can change) and restart its
   IO proc on `kAudioHardwarePropertyDevices` / `kAudioDevicePropertyDeviceIsAlive` changes.
7. Sample-rate change from Audio MIDI Setup on the mic while running: both devices follow.
8. Apps that want an input volume control (Zoom "automatically adjust microphone volume", the
   Sound settings input slider) see none; decide whether to add a volume/mute control.
9. CPU use of coreaudiod with the plug-in idle and running (should be negligible).

## Files

* `src/BeamerAudio.c`: the plug-in (GPL-3.0-only). Structure modelled on Apple's public NullAudio
  sample; code written from scratch, no code from BlackHole or other projects.
* `Info.plist`: bundle and CFPlugIn registration.
* `build.sh`: universal build, export check, signing ("Beamer Dev" if present, else ad-hoc),
  staging copy.
* `test/test_host.c`, `test.sh`: the out-of-process test host.
