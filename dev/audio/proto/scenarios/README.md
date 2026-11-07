# Audio scenarios, end to end (Phase 1)

The three prototypes (`../mac`, `../win`, `../transport`) wired into the four real uses, each run
for >= 60 s between the MacBook Air and `lebig` (both on Wi-Fi, LAN 192.168.1.x), with a known
test signal injected at the source and found again at the sink. Nobody was at either machine.
2026-11-07. Raw runs are in `runs/` (git-ignored); every number below comes from a
`runs/NAME/result.json` written by `analyze.py`.

## Files

| File | What |
|---|---|
| `run.py` | runs one scenario: writes the PC side as `.cmd` files + a PowerShell driver, starts it over ssh, starts the Mac side, fetches the PC's recordings, calls `analyze.py` |
| `pair.py` | `peer.py send`, unchanged, that also saves its ping exchange (for the clock offset) |
| `tap.py` | stdio helper on both machines: `src` (wraps a capture tool, can mix in the test signal, forwards to the transport, records), `rec` (records a capture tool), `sink` (records the transport's output, can forward it scaled to a player), `gen` (paced test signal into a player); Windows/Mac exclusion helpers |
| `sig.py` | test signals and the analysis: matched filter, pilot lock-in glitch detector, stream timeline, clock fit |
| `analyze.py` | latency, glitches, transport counters, CPU, endpoint state, loop test |

```
uv run --python 3.13 --with pynacl --with numpy run.py s3 --duration 75
```
Scenarios: `s1` `s1ac` `s2` `s2noise` `s3` `s4` `s5` `s5loop` `s5loopoff`. PC side:
`C:\Users\Alex\audio-proto\scenarios` (`tap.py`, `sig.py`, a copy of `beamer-audio.exe` from
the `pc/audio-proto` worktree, so a run never locks the worktree's build), transport and venv as in
`../transport/README.md`. The Mac connects; the PC punches first (no firewall rule).

## Method

**Test signal.** Two signatures so both directions can share a sink and a loop shows up:
A (Mac -> PC) = pilot 1025 Hz + 40 ms up-chirp 2-5 kHz; B (PC -> Mac) = pilot 625 Hz + down-chirp
5-2 kHz. Chirps (markers) start every ~1 s with an uneven 37 ms x (k mod 7) offset, so a wrong
source/sink pairing cannot pass unnoticed. Mic scenarios mix the signal into the live mic stream
at the sender (`tap.py src --mix`): the real device path runs, and the injection instant is
known to the sample.

**Timeline.** Every helper records the stream it sees and, per chunk read from the tool's pipe,
`time.monotonic()` (mach_absolute_time on the Mac; QueryPerformanceCounter on Windows, Python
3.13). A chunk can only be read late, so a sample's time is the lower envelope (+-1 s) of
`read time - index/48000`: "when this sample became available to software". Markers are found
with a band-limited matched filter (cosine similarity, sample-accurate) and paired one-to-one with
the first sink detection 0-600 ms later.

**One-way latency = sink time - source time, Mac and PC clocks joined by the transport's own ping
exchange** (4 pings/s, NTP formula). The lowest-RTT 30 % are fitted with offset = a + b t
(slope 15-28 ppm, as the transport README found). Fit residual 0.16-0.33 ms; the absolute error
is bounded by path asymmetry, at most RTT_min/2 = 1.5-2.3 ms in these runs. Cross-check without
any clock: in the loop control (below) a burst went round Mac -> PC -> Mac in 146.7 ms, and the
ping-timed legs of that same burst were 96.4 + 50.3 ms, matching scenarios 3 and 4 run separately
(95.6 and 51.1-55.6 ms).

**Where latency starts and stops.** Source = the moment the source's capture delivered the
samples (mic, tap or loopback); sink = the moment the sink side's capture delivered them: the
PC app reading CABLE Output (1), the Mac process tap of the player (2, 4) or the G733 loopback (3).
Device hardware/DSP (mic front end, USB, headset radio, speaker DSP) is outside, except in the
acoustic rows.

**Glitches.** Pilot lock-in per 10 ms window (Hann); a window is bad if the phase steps by > 0.3
rad or the level drops > 6 dB below the 1 s median. Pilots hold 10.25 / 6.25 cycles per 10 ms,
so a lost or repeated 10 ms block steps the phase by 90 degrees. Windows a marker overlaps are
skipped (the codec spends its bits on the chirp there). Self-test: a 10 ms drop, a 5 ms hole and
a 10 ms repeat all found, clean signal 0 (a 1-sample slip is not found, and is inaudible).
Events = runs of bad windows; per minute of judged pilot. They line up with the receiver's
`expand`/`underrun`/`drop` counters (Wi-Fi delay spikes), listed too.

**CPU** = % of one core over the run: transport (`peer.py`, Python) from its per-second stats;
device tools from their exit events (Mac rusage; Windows cycle counts). The harness (`tap.py`)
is listed apart: 0.7-1.9 % of a core per helper.

Transport settings: Opus 10 ms; mic mono VOIP 32 kb/s with FEC; system sound stereo AUDIO 96 kb/s
without FEC; adaptive jitter buffer. Mac `play --buffer-frames 128 --prefill-ms 15`, Mac capture
512-frame tap / 128-frame mic; PC `play --live-drop-ms 60 --queue-ms 20` (10 ms period, 2 periods).

## Things found on the way that shaped the runs

- **The PC's Wave:3 hears the MacBook's speakers at about -11 dB** (a -20 dBFS chirp from the Mac
  arrived at -31 dBFS). The first scenario-2 trial played the PC mic on the Mac speakers at unity
  and howled (6.9 kHz, up to -1 dBFS, ~10 s). Scenario 2 therefore records the transport's output
  as a file sink and plays it 40 dB down (loop gain far below 1). Beamer will meet this whenever
  both machines sit on one desk: **mic forwarding to a machine whose speakers the mic hears needs
  echo cancellation or a headset.**
- **The Mac mic hears the PC's G733 headset** (-68 dBFS, 66 ms after the G733 loopback, which
  includes the Mac mic's input path).
- **The Wave:3 gates itself** to ~-165 dBFS in a quiet room; room sounds come through at -40 to
  -26 dBFS. A -40 dBFS pilot mixed into it gave false glitches, so mic scenarios use a -14 dBFS
  pilot (digital only, never played at unity).
- **Windows process loopback ignores endpoints**: it captures every non-excluded process on every
  render device. Here it also picked up the tone another agent was playing into VB-CABLE.
- **A Python venv launcher breaks Windows' process tree** (see scenario 5).
- During scenarios 2-5 another agent was sweeping VB-CABLE on the PC (its tool at times in a
  CPU-load test); scenario 1 ran after it had finished (19:11 PC time, cable restarted 19:05, ids
  unchanged). A first scenario-1 attempt (`s1_a`) failed because of a mistyped CABLE Input id in
  `run.py`, now fixed.

## Results

| # | Scenario (run) | Works | One-way latency p50 / p95 (n) | Glitches / min (sink) | CPU % of a core |
|---|---|---|---|---|---|
| 1 | Mac mic -> PC virtual mic (VB-CABLE) (`s1_b`; real mic `s1ac_a`) | yes | Mac mic delivery -> app reading CABLE Output: **125.6 / 137.4 ms** (70 of 71 markers). Real-mic run (chirps from the Mac speakers, 12 markers, 20 s): write to `play` -> Mac mic delivery 71.0 / 73.5 (same clock), mic delivery -> CABLE Output 112.8 / 123.6, total 185.7 / 193.9 | 61.7 (68 events; PC JB 19 expand, 14 underrun, 17 drop in 72 s; CABLE capture 1 discontinuity, at start) | transport Mac 7.1, PC 4.5; capture-mic Mac 0.9, CABLE Output 0.12; play PC 0.21 |
| 2 | PC mic (Wave:3) -> Mac (`s2_b`) | yes | mic delivery -> Mac jitter-buffer output (file sink): **29.5 / 37.1 ms** (68); -> Mac speaker mix (tap of `play`): **55.4 / 63.2 ms** | 10.2 at JB output (11 events; JB: 4 expand, 3 underrun, 2 drop in 72 s) | transport Mac 11.0, PC 2.4; capture-mic PC 0.14; play Mac 0.4 |
| 3 | Mac system sound -> PC G733, muted on Mac (`s3_a`) | yes | Mac tap delivery -> G733 loopback: **95.6 / 103.8 ms** (69) | 1.9 (2 events; PC JB 2 expand) | transport Mac 4.1, PC 5.0; capture-system Mac 0.4; play PC 0.10 |
| 4 | PC system sound -> Mac speakers, muted on PC (`s4_b`; `s4_a`) | yes | PC loopback delivery -> Mac speaker mix: **55.6 / 64.1 ms** (67); `s4_a` 51.1 / 71.4. App write -> PC loopback (local, same clock): 42.5 / 43.3 | 4.0 (4 events; Mac JB 4 expand) | transport Mac 14.9, PC 1.7; capture-system PC 0.19; play Mac 0.4 |
| 5 | 3 + 4 at once, players excluded (`s5_b`) | yes, no loop | Mac -> PC **100.1 / 107.0 ms** (65); PC -> Mac **62.0 / 71.5 ms** (67) | PC 3.1, Mac 8.0 (JB: Mac 6 expand, 3 drop; PC 3 expand, 2 drop) | transport Mac 18.8, PC 6.1; capture-system Mac 0.4, PC 0.43; play Mac 0.4, PC 0.11 |

Detection: every marker sent while the transport ran was found at the sink (similarity >= 0.97 on
system paths, >= 0.46 on the mic path through Opus VOIP), none unpaired except at session edges.

### What the numbers are made of

- **Mac -> PC is ~40 ms slower than PC -> Mac** (3: 96, 5: 100 vs 4: 56, 5: 62). The PC receiver
  sees 10-15 ms of jitter on packets from the Mac (the Mac sees ~2 ms from the PC: the transport
  README saw the same asymmetry), so its jitter buffer holds 33-37 ms against 20-26 ms on the
  Mac; add the Mac tap's 512-frame (10.7 ms) delivery, Opus 6.5 ms, framing 10 ms, the PC's
  2-period render queue (20 ms) and its loopback (~10 ms).
- Scenario 2's 29.5 ms (mic delivery -> JB output) is the transport alone: framing + Opus +
  network + JB. Mac `play` + its tap add ~26 ms (55.4).
- Glitches are almost all receiver underruns on Wi-Fi delay spikes (RTT max 14-31 ms in runs with
  RTT p50 5 ms), concealed by Opus PLC/expansion; no packet was lost in any run.

### Scenario 2, real mic without a test signal (`s2noise_a`, 20 s)

Quiet room: the Wave:3 delivered -165 dBFS (its own gate), the Mac received the codec's floor,
-107.7 dBFS, every second without a gap. A room sound at 12-16 s came through with the same
envelope: PC -38.0 / -45.6 / -44.3 dBFS, Mac -38.6 / -46.2 / -45.3 dBFS.

### Local mute

- **Mac (3):** during the run the Mac mic heard the markers only from the PC's headset (-68 dBFS,
  166 ms after the Mac tap); in the 3 s after the tap was destroyed, while `afplay` kept playing,
  it heard them from the Mac speakers at -43 dBFS (25 dB louder). So the speakers were silent while
  tapped and came back by themselves.
- **PC (4, 5, loops):** G733 endpoint before mute=false vol=0.80; half way mute=**true** vol=0.80;
  after mute=false vol=0.80 (tool's `endpoint_restored` event and a fresh `list --json`). The
  same check passed in every PC-muting run (`s4_a`, `s4_b`, `s5_a`, `s5_b`, `s5loop_a`,
  `s5loopoff_a`). Mute lasted the run plus ~5 s.

## Scenario 5: the feedback loop and `--exclude-pid`

Each capture now leaves out its own machine's player:
- Mac `capture-system --exclude-pid PID` (repeatable): PID -> process object
  (`kAudioHardwarePropertyTranslatePIDToProcessObject`) into the `CATapDescription` exclusion list;
  a PID with no process object yet is added to the live tap when it appears. `tap.py` finds the
  transport's `play` sibling and passes it.
- Windows `capture-system --exclude-pid PID`: process loopback,
  `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`, one tree only (a second PID is refused).

**How Beamer arranges its processes on Windows:** the Beamer process that runs the audio starts
both the player and the capture as its own children and passes `--exclude-pid` = its own PID
(`os.getpid()`). Grandchildren are covered (cmd -> cmd -> play: excluded, -102 dBFS) and so are
children started after the capture. **But a Python venv launcher is not followed**: the first
scenario-5 run (`s5_a`) excluded the topmost `venv\Scripts\python.exe` and the PC still captured
its player (A found 65 times in the PC capture: a live loop). Excluding the base interpreter that
spawned the player fixed it (`s5_b`). Check the same for a PyInstaller one-file bootloader (the
real Beamer runs as its child): always pass the PID of the process that spawns the helpers.
On the Mac there are no trees; Beamer passes each player PID (or plays from the capture process
itself, which the tap always excludes).

**Loop test**: one A burst from the Mac (`afplay`), 7 s later one B burst from the PC, no pilots,
both captures muting locally, 24 s.

| | exclusion on (`s5loop_a`) | exclusion off, control (`s5loopoff_a`) |
|---|---|---|
| A seen by the Mac capture | once | 88 times, every **146.7 ms** |
| B seen by the PC capture | once | 73 times, every 146.9 ms |
| A in the PC capture / B in the Mac capture | **0 / 0** | 89 / 73 |
| level of the returns | - | -1.8 dB after 11 laps (1.6 s): about -0.15 dB per lap |
| energy in the 3 s after a burst, vs the burst | **-96 to -99 dB** (Mac capture: digital zero) | **-3.5 to -6.8 dB** |

In the 75 s run with pilots and markers both ways (`s5_b`) the PC's signal never appeared in
the Mac's outgoing stream and the Mac's never in the PC's (0 of 67 and 0 of 65 markers).
Without exclusion the loop is silent locally (both players were muted by the local mute) but
would be heard on the far side of any unmuted path, and sends a constant stream of old audio.

## Open issues

- **Scenario 1 is the slowest path and the glitchiest**: 126 ms, of which VB-CABLE in shared mode is
  the largest part (registry at the run, 19:11: `VBAudioCableWDM_SR = 48000`,
  `VBAudioCableWDM_Latency = 7168`, `VBAudioCableWDM_LoopBack = 1`; the tuning agent found no gain
  from lowering it on the shared-mode path). The PC jitter buffer had ~50 expand/underrun/drop
  events in 72 s (Mac -> PC jitter), giving 62 glitches/min at CABLE Output. The Windows tool has no
  WASAPI exclusive mode, so the exclusive cable path (~55 ms per the tuning agent) was not tried.
- **Mic forwarding and acoustic echo:** with the machines on one desk the Wave:3 hears the Mac
  speakers at -11 dB, so PC mic -> Mac speakers howls at unity gain. Beamer needs AEC (or a
  headset / no mic forwarding to a machine whose speakers the mic hears).
- **Windows local mute is endpoint-wide:** in scenario 5 the G733 endpoint mute also silences the
  Mac's audio that the PC plays there. With both directions on the same headset the PC user hears
  nothing. Options: play the remote audio on another endpoint, or a different silencing method
  (session mute kills loopback, see `../win/README.md`).
- **Windows process loopback has one exclusion tree and ignores endpoints**: the PC's "system
  sound" includes audio sent to any device (VB-CABLE included) unless excluded.
- Mac -> PC jitter (10-15 ms at the PC vs 2 ms the other way) costs ~15 ms of buffer; worth a
  look at the Mac's Wi-Fi power saving or wired Ethernet.
- 2-10 glitches/min on Wi-Fi (underruns on delay spikes). Same as the transport README.
- Not covered: device hardware time on the system paths (USB, G733 radio, speaker DSP), the
  Mac mic's own front end except in the acoustic row, hot-plug during a session, runs > 75 s.
