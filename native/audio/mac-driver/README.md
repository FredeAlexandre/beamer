<!-- SPDX-License-Identifier: GPL-3.0-only -->
# BeamerAudio.driver: the Mac virtual microphone

A Core Audio AudioServerPlugIn (HAL plug-in, runs inside coreaudiod) that makes a microphone
received from another machine show up on the Mac as a real input device that Zoom, Discord,
FaceTime... can pick.

Status (2026-11-08): **installed and validated on the Mac** through the `beamer-hal` root helper
(see "Installing"). "Beamer Microphone" shows up in macOS, the hidden feed loops back to it
bit-exact, and the mic has an input volume and mute. Live results: "Live test results" below.

```
./build.sh   # universal bundle -> build/BeamerAudio.driver, copied to ~/Developer/beamer-hal-staging/
./test.sh    # build.sh + drives the bundle with test/test_host (plus ASan/UBSan and TSan builds)
sudo -n /usr/local/libexec/beamer-hal install   # copy the staged bundle into the HAL + restart coreaudiod
swiftc -O test/ca_util.swift -o build/ca_util && (cd test && uv run --python 3.13 --with numpy live_check.py)
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
Float32 mono packed, 48 kHz (default) or 44.1 kHz, clock domain `'BEMR'` on both, latency 0,
safety offset 0 (answered in **both** directions, see below), zero-timestamp period 16384 frames.

**Microphone controls** (input scope, owned by "Beamer Microphone"; the feed has none):
volume control object 6 (scalar 0..1, linear in dB over -64..0 dB, scalar 0 = silence) and mute
control object 7. Apps and System Settings see them as the device's input volume/mute
(`kAudioDevicePropertyVolumeScalar` / `kAudioDevicePropertyMute`, settable). The gain is applied
in the mic's ReadInput only, read lock-free; at full volume nothing is multiplied, so the loopback
stays bit-exact. Values are not persisted by the driver (start at 1.0 / unmuted after each
coreaudiod start); Beamer sets them if it needs to.

**Unknown properties are logged** (`os_log`, subsystem `com.beamer.audio.driver`, category
`property`) for GetPropertyData/DataSize, never on the IO path. That is how the install blocker was
found: coreaudiod's own log redacts the selector.
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

## Installing (beamer-hal)

`/usr/local/libexec/beamer-hal` (root:wheel 755) with a NOPASSWD sudoers rule
(`/etc/sudoers.d/beamer-hal`, only `install`, `uninstall`, `restart`). Source and one-time setup in
`helper/` (`sudo helper/setup-beamer-hal.sh <user> <staged bundle path>`; installed 2026-11-08).

* `install`: copies the staged bundle into a root-only temp dir, refuses symlinks, checks the
  bundle id and `codesign --verify --strict`, chowns root:wheel, moves it to
  `/Library/Audio/Plug-Ins/HAL/BeamerAudio.driver`, restarts coreaudiod (`killall`; launchd
  relaunches it, `launchctl kickstart` is refused under SIP).
* `uninstall`, `restart`; `status` (no root) compares the staged and installed bundles.
* The staged path is fixed at setup time; the helper takes no other argument and ignores the
  environment.

Signature: amfid logs "adhoc signed or signed by an unknown certificate chain" for the self-signed
"Beamer Dev" signature, but the Core Audio driver service loads the plug-in anyway (same with
ad-hoc). A shipped build should still be Developer ID signed and notarized.

**Restarting coreaudiod is disruptive**: every app's audio drops, and on this Mac (macOS 27.0)
coreaudiod sometimes stays at ~100 % CPU after a restart done while apps play or record (same with
the built-in devices only, so not the driver). A second restart with nothing playing clears it.
Beamer should install/update the driver when no audio is running, or tell the user.

## Live test results (2026-11-08, macOS 27.0 26A428, MacBook Air, installed driver)

The install blocker: coreaudiod asks every device for `kAudioDevicePropertySafetyOffset` (and
latency) in **both** directions while adding it, even the direction without a stream; the driver
answered only its own direction, the HAL got `'who?'` and dropped both devices ("Caught exception
trying to add device"). Fixed by answering 0 in input and output scopes; `test_host` now checks it.

`test/live_check.py` drives the real devices with the prototype tool `beamer-audio` (`dev/audio/proto/mac`;
`--feeder native` feeds through the helper's Mac backend, `native/audio/build/mac/backend_check`): a
feeder plays a 1025 Hz pilot (-20 dBFS) with chirp markers into the hidden feed, a recorder
captures "Beamer Microphone" (both 48 kHz mono, 512-frame buffers). Results (all PASS):

| README check | result |
|---|---|
| 1 loads | "Beamer Microphone" in `system_profiler` / device list, virtual, 48 kHz; no unknown-property request from coreaudiod after the fix |
| 2 feed hidden | not in `kAudioHardwarePropertyDevices` (what Sound settings and app pickers enumerate); not checked by hand in Zoom/Discord (not installed here) |
| 3 UID lookup | `TranslateUIDToDevice("com.beamer.audio.micfeed")` from a user process returns it; IO starts (`beamer-audio` now falls back to it for hidden devices) |
| 4 end-to-end | 12/12 clicks: render -> mic delivery **17.5 ms at 512 frames, 4.2 ms at 128**; HAL timestamps identical (0 ms). 10.6 s of pilot: 0 glitches, 0 lost/repeated samples (constant marker offset), **bit-exact** (432000 frames, max error 0.0) |
| 5 start/stop orders | mic alone: digital silence; feed SIGKILLed while recording: audio stops 22-25 ms later, then 3 s of exact silence (no replay); feed restarted: clean; recorder stopped and a new one joins mid-stream: clean. Sleep/wake **not tested** |
| 6 coreaudiod restart | devices come back under the same UIDs (new object IDs), a fresh feed/recorder pair is clean. Clients that were running keep their process but get no more audio: Beamer must re-translate the UID and restart its IO proc. See the CPU caveat above |
| 7 rate change | 44.1 kHz set on the mic while both run -> both devices 44.1 kHz; 48 kHz clients through the HAL's converter: 0 glitches; back to 48 kHz |
| 8 volume/mute | added (see Design). Input volume and mute visible and settable from apps; volume 0.5 -> pilot -52 dBFS (-32 dB as designed), mute -> exact silence, unmute keeps the volume, 1.0 -> -20 dBFS again |
| 9 CPU | driver service process 0.0-0.2 %; coreaudiod 2-3 % while streaming. Idle coreaudiod numbers on this Mac are dominated by the unrelated spikes above |

Out-of-process suite (`./test.sh`) after the changes: **2205 checks, 0 failures** (signed bundle,
ASan+UBSan, TSan), including the controls and the both-scopes latency/safety offset answers.

Still open: sleep/wake; a look at Zoom/Discord/Chrome pickers by hand; latency/safety offset stay
0 (no app complained in these tests; revisit if an app reports A/V sync issues).

## Files

* `src/BeamerAudio.c`: the plug-in (GPL-3.0-only). Structure modelled on Apple's public NullAudio
  sample; code written from scratch, no code from BlackHole or other projects.
* `Info.plist`: bundle and CFPlugIn registration.
* `build.sh`: universal build, export check, signing ("Beamer Dev" if present, else ad-hoc),
  staging copy.
* `test/test_host.c`, `test.sh`: the out-of-process test host.
* `test/live_check.py`, `test/ca_util.swift`: live checks against the installed driver.
* `helper/beamer-hal`, `helper/setup-beamer-hal.sh`: the root install helper and its one-time setup.
