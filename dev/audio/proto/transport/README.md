# Audio transport prototype (Phase 1, T6)

Standalone proof of the media plane chosen in `../../notes/beamer-link.md` (architecture B):
control over the Beamer link (stood in for here by a pre-shared key file), **media over a
separate UDP flow on port 24823**, one AEAD per packet, Opus, an adaptive jitter buffer with
PLC/FEC, and clock-drift compensation by resampling. Nothing in the Beamer app changes.
All numbers below come from 2026-11-07 runs between the MacBook Air (Apple M5, macOS 27) and
`lebig` (Ryzen 5 7600X, Windows 11 26200). **Both machines are on Wi-Fi.** Raw results
(`out/*.jsonl`, `out/*.summary.json`) are git-ignored; `report.py` turns them into rows.

## Files

| File | What |
|---|---|
| `opus.py` | ctypes binding to libopus (encoder, decoder, PLC, in-band FEC, LBRR probe) |
| `media.py` | packet format, ChaCha20-Poly1305 per packet + replay window, Opus/PCM codecs, windowed-sinc variable resampler, `JitterBuffer` (adaptive target, PLC/FEC, expand, drop/flush, PI drift control) |
| `peer.py` | CLI: `serve` (PC), `send` (Mac, always initiates), `local` (both ends in one process), `devices`, `keygen`; sources/sinks; loss and jitter injection; stats |
| `bench.py` | offline: codec CPU/bitrate/algorithmic delay, resampler SNR, JB cost, crypto self-test |
| `devloop.py` | device loop latency (e.g. CABLE Input -> CABLE Output) |
| `run_pair.sh` | one Mac<->PC session (starts the PC side over ssh) |
| `report.py` | summary rows, `--drift NAME` per-minute buffer/ppm table |
| `jprobe.py` | raw UDP one-way-delay probe, both directions at once (no audio, no crypto): `server` / `client` / `analyze` (see "Mac->PC jitter") |
| `build_opus_win.bat` | builds `opus.dll` on the PC |

Run (no global installs):

```
uv run --python 3.13 --with pynacl --with numpy peer.py keygen out/test.key      # 32 bytes hex
# PC (copied to C:\Users\Alex\audio-proto\transport, own venv ..\venv with pynacl numpy sounddevice)
..\venv\Scripts\python.exe peer.py serve --key-file test.key --source sd:45 --sink sd:35
# Mac
uv run --python 3.13 --with pynacl --with numpy peer.py send --to 192.168.1.134:24823 \
    --key-file out/test.key --source clicks --sink analyze
```

Sources: `tone[:HZ]`, `clicks[:PERIOD_MS]`, `voice` (analysable voiced-speech stand-in),
`silence`, `stdin`, `cmd:COMMAND` (e.g. `cmd:../mac/.build/beamer-audio capture-mic --channels 1
--buffer-frames 128`), `sd:DEVICE`, `echo`. Sinks: `null`, `stdout`, `cmd:COMMAND` (e.g.
`beamer-audio play`), `wav:PATH`, `sd:DEVICE`, `analyze` (click latency), `analyze:voice` /
`analyze:HZ` (glitch detector), `echo`. Options: `--codec opus|pcm --frame-ms 10|20 --app
voip|audio|lowdelay --[no-]fec --bitrate --loss PCT --burst N --jitter-ms J --jb-min/max/fixed
--max-ppm --catchup-ppm --drop-ms --jb-peak-hold --rr-ms --pkt-log PREFIX --transport udp|tcp|auto`
(see `--help`). `--pkt-log PREFIX` writes per-packet traces at the end: `PREFIX.tx.npy` (ts,
time fed, time sent), `PREFIX.tx.opus` (the encoded frames), `PREFIX.rx.npy` (ts, arrival),
`PREFIX.pull.npy` (jitter-buffer pulls: time, level, target, events).

### opus.dll on Windows

Built from the official BSD-3-Clause xiph tarball `opus-1.6.1.tar.gz` (sha256
`6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`, the same release and hash
Homebrew uses on the Mac), with MSVC 2022 Build Tools and the CMake bundled with them:
`build_opus_win.bat` (`-DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF`,
NMake, Release, x64, the default runtime-CPU-detected SSE4.1/AVX2 paths). Result 474,624 bytes,
copied next to `opus.py`; never committed. `opus_encoder_ctl` is variadic: ctypes passes the
surplus arguments as variadic (needed on Apple arm64); checked by reading the bitrate back.

## Packet and session

24-byte header, sent in clear and authenticated as AEAD associated data:
`version u8 | ptype u8 (media, hello, ping, pong, stats, bye) | flags u8 (fec, marker, silence) |
codec u8 | stream id u32 | seq u64 | ts u32 (sender samples at 48 kHz) | nsamp u16 | channels u8 |
reserved u8`, then ChaCha20-Poly1305-IETF ciphertext + 16-byte tag (PyNaCl `nacl.bindings`).
Nonce = stream id || seq; each direction has its own random stream id, and the stream key is
derived from the key file and the stream id (production: a random 32-byte key in `audio_open` on
the link). The receiver keeps a 128-packet sliding replay window. Self-test (`bench.py`): in
order, reordered-within-window accepted; replays, older-than-window, tampered payload,
tampered header and wrong key rejected. Media frames are ordered by `ts`, not by `seq` (which
also counts control packets). Opus 10 ms mono at 32 kb/s is ~38 B of payload, 78 B on UDP.

Session: the Mac sends `hello` every 200 ms from a `connect()`-ed socket until the PC answers
to the source address; then `ping` every second (RTT and clock offset, NTP-style 4 timestamps),
`hello` every 3 s as keepalive, `bye` at the end. A side that receives media but sends none sends a
**receiver report** (`RR`, 6-byte payload: newest ts received, buffer level) every `--rr-ms` (10)
ms; it exists to keep that machine's Wi-Fi radio out of power save (see "Mac->PC jitter"). The PC drops the peer after 3 s of silence.
Both directions share one 5-tuple. The PC side sends its per-second stats to the Mac inside
the flow, so one JSON-lines file holds both sides.

## Jitter buffer and drift control (media.py `JitterBuffer`)

Pull model: the playback clock (a device callback, or a paced thread standing in for an ideal
device) calls `pull(n)`. The controlled quantity is the **level** just before each pull: audio
from the playout point to the end of the newest frame received (holes included).

- Target = pull block + frame/2 + network jitter (p97 - min of transit delay over 10 s, ignoring
  spikes > 150 ms, which are outages) + playback-clock jitter (same statistic on pull times;
  added after the 15-min run showed PortAudio's WASAPI callbacks arriving in pairs) + 2 ms,
  clamped to [5, 120] ms; it rises at once and decays over ~2 s. `--jb-fixed` pins it.
- Missing frame with newer ones present: Opus in-band FEC from the next packet if present
  (LBRR), else PLC. Nothing newer present (late, underrun): PLC **without advancing the
  timeline** (expand); after 3 frames, re-buffer.
- Too much audio: drop the next frame when it is silent (sender flag) and the level is more than
  max(5 ms, 0.75 frame) above target; otherwise **catch up by resampling faster**, up to
  `--catchup-ppm` 2000 (0.2 %, 3.5 cents) when the level is > 4 ms above target (removes a 10 ms
  expansion in ~5 s); drop a non-silent frame only after 2 s above max(`--drop-ms` 30, frame);
  if the level exceeds target + max(40 ms, target), flush the oldest frames down to target at once
  (after a Wi-Fi stall, 1.4 s of late packets otherwise sat in the buffer: seen in the first chain
  run, fixed). Dropped and flushed frames are **decoded and discarded**, never skipped: an Opus
  decoder that misses a frame stays out of step with the encoder (see "Mac->PC jitter").
- Spike-aware target (`--jb-peak-hold` 10 s): when two delay spikes higher than
  max(15 ms, p97 + 5 ms) (and below 150 ms) arrive within 10 s, the target provisions for the
  highest until 10 s after the last one (NetEq's "peak mode", shortened).
- Drift: level EWMA (1 s) minus target -> ppm = 40 ppm/ms x error + integral (0.1 ppm/(ms.s),
  clamped +-200 ppm), total clamped to **+-500 ppm** (0.86 cent, inaudible). The resampler is a
  16-tap Kaiser-windowed sinc with 512 phases: SNR vs ideal fractional delay 89 dB at 440 Hz,
  82 dB at 1 kHz, 63 dB at 10 kHz, 58 dB at 16 kHz, the same at 100 and 500 ppm (`bench.py`),
  far below Opus coding noise. Group delay 0.17 ms.
- Drift is also measured independently from the per-second minimum of transit delay
  (sender samples vs receiver clock, robust linear fit) and from the ping clock offset.

## Results

### 1. End-to-end latency (round trip, clicks generated on the Mac, detected on the Mac)

The Mac source is an ideal device (a click's time is its sample time on the Mac monotonic
clock); the Mac sink is an ideal device (sample time = pull time). Latency = detected peak
minus generated peak, median of 57-58 clicks per minute-long run (n = 840 in the 15-min run).
Opus 10 ms frames, mono, 32 kb/s, FEC on, adaptive jitter buffer, LAN (Wi-Fi), 60 s per run.

| PC side of the loop | round trip p50 | p95 | max | JB level PC / Mac (mean) |
|---|---:|---:|---:|---:|
| `echo` (decoded audio re-encoded at once; network + codec + 2 JBs) | 60.6-67.6 ms | 68-70 | 68-81 | 22-24 / 20-23 ms |
| same, PCM s16 (no codec) | 54.3 | 54.7 | 54.7 | 23.0 / 21.0 |
| same, Opus RESTRICTED_LOWDELAY, no FEC | 52.0 | 70.7 | 72.1 | 22.0 / 21.6 |
| same, Opus 20 ms frames | 88.5 | 102.0 | 103.2 | 36.2 / 25.5 |
| **full chain**: play into CABLE Input (PortAudio WASAPI, 10 ms), capture CABLE Output, send back | 157.7 / 164.2 | 159 / 177 | 169 / 193 | 24-28 / 21-24 |
| full chain with the Windows `beamer-audio` tool (`play` IAudioClient3 128-frame period, `capture-mic`) | 152.5 | 156.0 | 156.3 | 22.1 / 23.4 |
| full chain, 15 min | 175.6 | 186.2 | 226.4 | 23.4 / 24.8 |
| full chain, 20 ms frames | 192.3 | 198.7 | 233.7 | 34.0 / 36.5 |

Split of one direction (Mac -> PC, Opus 10 ms, LAN Wi-Fi):

| stage | ms | how |
|---|---:|---|
| frame accumulation + jitter buffer | 20-24 | mean JB level at pull (it includes the frame being filled: latency = network + level) |
| Opus algorithmic delay (VOIP / AUDIO) | 6.5 | lookahead 312 samples; measured 6.4 by cross-correlation; RESTRICTED_LOWDELAY 2.5 |
| encode + decode CPU | 0.1-0.4 | `bench.py`; live 0.3 ms (Mac efficiency cores) |
| network one way | 2.7 (p95 4) | RTT/2, RTT p50 5.1-5.7 ms, p95 6.6-8.9 ms |
| resampler | 0.2 | 8 taps of group delay |
| **transport total** | **30-34** | = echo loop / 2 (PCM: 27, lowdelay: 26) |
| PC: our playback into CABLE + VB-CABLE + capture of CABLE Output | **90-100** | chain minus echo; `devloop.py` alone: 100 ms (PortAudio block 0) to 110 ms (480) |
| Mac mic capture (128-frame buffer) | 3 + DSP | T2: capture timestamp -> delivery 2.9 ms; hardware+DSP of mic+speaker together 44 ms |
| Mac playback (128-frame buffer) | 4.3 + prefill + DSP | T2 |

**VB-CABLE dominates**: `HKLM\SOFTWARE\VB-Audio\Cable\VBAudioCableWDM_Latency = 7168` (its
maximum internal buffer, 149 ms); the cable alone costs ~100 ms here. Our own path is 30-34 ms
one way.

### 2. Network: LAN vs Tailscale (both Wi-Fi, 60 s runs, pings every 100 ms)

| path | RTT min / p50 / p95 / max | media jitter p97 at PC / Mac | echo loop p50 / p95 |
|---|---|---|---|
| LAN 192.168.1.134 | 3.0 / 5.1-5.7 / 6.6-8.9 / 16-48 ms | 5.4-6.7 / 2.1-3.1 ms | 60.6-67.6 / 68-70 ms |
| Tailscale 100.71.12.47 (direct path) | ~3 / 5.6-5.8 / 8.3 / 16-22 ms | 6.0-6.9 / 2.8-3.0 ms | 66.0 / 80.9 ms |

Tailscale costs nothing measurable here (WireGuard direct over the same Wi-Fi). Datagrams are
<= 1000 B (PCM) or ~80-200 B (Opus), so the 1280-byte Tailscale MTU is never hit. Wi-Fi gives
occasional stalls: RTT max 1.46 s once, 48-119 ms a few times per 15 min; the jitter buffer
treats > 150 ms as outages (expand, then flush the backlog) instead of provisioning for them.
Jitter at the PC (receiving from the Mac) is consistently twice that at the Mac: the PC's Wi-Fi
power save, see "Mac->PC jitter".

### 3. Opus vs PCM, 10 vs 20 ms

Offline (`bench.py`, voiced-speech-like signal, per frame; Mac M5 / PC 7600X):

| codec | frame | payload kb/s | on the wire kb/s | encode us | decode us | CPU % of a core | delay |
|---|---:|---:|---:|---:|---:|---:|---:|
| Opus voip mono 32k FEC | 10 ms | 30.2 | 84.6 | 82 / 86 | 9 / 15 | 0.9 / 1.0 | 6.5 ms |
| Opus voip mono 32k FEC | 20 ms | 29.5 | 56.7 | 154 / 160 | 15 / 26 | 0.85 / 0.93 | 6.5 |
| Opus voip mono 32k no FEC | 10 ms | 27.1 | 81.5 | 65 / 77 | 9 / 15 | 0.7 / 0.9 | 6.5 |
| Opus lowdelay mono 32k | 10 ms | 32.8 | 87.2 | 24 / 31 | 7 / 9 | 0.3 / 0.4 | 2.5 |
| Opus audio stereo 96k | 10 ms | 96.8 | 151.2 | 43 / 55 | 10 / 15 | 0.5 / 0.7 | 6.5 |
| Opus audio stereo 96k | 20 ms | 96.4 | 123.6 | 77 / 95 | 18 / 25 | 0.5 / 0.6 | 6.5 |
| Opus lowdelay stereo 128k | 10 ms | 128.8 | 183.2 | 47 / 59 | 12 / 17 | 0.6 / 0.8 | 2.5 |
| PCM s16 mono | 10 ms | 768 | 822 | 2 / 4 | 1 / 2 | 0.03 / 0.06 | 0 |
| PCM s16 stereo | 5 ms | 1536 | 1645 | 2 / 4 | 1 / 2 | 0.06 / 0.13 | 0 |

"On the wire" adds 68 B per packet (24 header + 16 tag + UDP + IPv4): at 10 ms that is 54 kb/s,
more than the Opus payload; irrelevant on a LAN. Per-frame cost of the Python receive path
(open + push + pull incl. decode and resample): 75 us Mac, 149 us PC. Whole prototype process:
Mac 10-17 % of a core, PC 4-7 % (Python, threads, stats; the codec is ~1 %). Live, 10 ms vs
20 ms frames: echo loop 60.6-67.6 vs 88.5 ms, full chain 158-164 vs 192 ms: **20 ms frames
cost ~10-14 ms per direction** (frame/2 in the JB target, a 20 ms pull vs packet phase, and
larger level steps), and save 28 kb/s of overhead. PCM saves exactly the 6.5 ms Opus lookahead
per direction and costs 10x the bandwidth with no usable concealment (below).

### 4. Loss and jitter (PC sends `voice`, Mac analyses; 45 s, ~4470 windows of 10 ms)

`voice` = 12 harmonics of 180 Hz with 3 Hz AM, which SILK treats as active speech (LBRR on
~75 % of frames, like voiced speech). A 10 ms window is a glitch when the residual after
fitting the known harmonic model exceeds -20 dB of the window power, or its power falls 20 dB
below the running median. Loss is injected at the PC sender (Bernoulli, or Gilbert with mean
burst length 3), seed 1. Opus 10 ms unless noted.

| loss | concealment | lost | recovered by FEC | PLC | glitch windows | glitch events |
|---|---|---:|---:|---:|---:|---:|
| 0 % | FEC on | 0 | 0 | 0 | 0 | 0 |
| 1 % | FEC on | 55 | 55 | 0 | 4 | 4 |
| 1 % | FEC off (PLC) | 55 | 0 | 55 | 13 | 12 |
| 5 % | FEC on | 229 | 212 | 17 | 48 | 42 |
| 5 % | FEC off (PLC) | 229 | 0 | 229 | 60 | 33 |
| 10 % | FEC on | 432 | 379 | 53 | 73 | 65 |
| 10 % | FEC off (PLC) | 430 | 0 | 430 | 87 | 46 |
| 5 %, bursts of ~3 | FEC on | 136 (*) | 70 | 66 | 123 | 39 |
| 5 % | PCM, repeat-and-fade | 229 | - | 229 | 103 | 84 |
| 5 % | Opus 20 ms, FEC on | 99 | 96 | 3 | 33 | 24 |

(*) noisy Wi-Fi during that run (RTT p95 23 ms): part of the burst losses became underruns.
FEC recovers 92-100 % of isolated losses and lowers glitch windows by 15-70 %; it cannot cover
bursts (LBRR only carries the previous frame). On this periodic test signal Opus PLC is already
good, so the detector understates FEC's benefit for real speech. Opus PLC is ~2x better than
naive PCM concealment. FEC costs ~3 kb/s of the 32 kb/s budget.

Jitter injection (uniform 0..J ms per packet at the PC sender, reordering allowed):

| injected | JB | JB level | underruns | glitch windows |
|---|---|---:|---:|---:|
| 0 | adaptive | 27.5 ms | 3 | 0 |
| 0-20 ms | adaptive | 34.3 ms | 0 | 0 |
| 0-50 ms | adaptive | 49.2 ms | 0 | 3 |
| 0-20 ms | fixed 20 ms | 29.6 ms | 3 | 1 |

### 5. Clock drift Mac vs PC and buffer boundedness

| estimate | ppm |
|---|---:|
| Mac monotonic vs PC monotonic (QPC), ping offset slope over 15 min (best ping per 10 s) | **+17.9** (Mac slower) |
| Mac sample clock seen by the PC receiver (transit-envelope fit, 15 min) | -18.1 |
| PC CABLE capture clock seen by the Mac receiver (15 min) | +15.7 |
| same estimates over the 60 s runs (n = 12) | 13-20 |
| T2: Mac mic = Mac speakers (0 ppm), +2 ppm vs Mac host clock | |
| T4: PC Wave:3 mic and G733 render, about -26 ppm vs QPC | |

So the host clocks differ by ~17 ppm (61 ms per hour) and real device pairs by tens of ppm
(Mac audio ~ -16 ppm vs QPC, PC endpoints -26 ppm: ~10 ppm between Mac mic and PC endpoints;
VB-CABLE follows QPC). Without compensation a buffer would gain or lose a 10 ms frame every
10-17 minutes.

15-minute full-chain run (`out/drift_chain_900`, v1 controller), per-minute JB level:

| side | level mean range | level max | target mean | underruns / min | drops / min | flushes |
|---|---|---:|---:|---:|---:|---:|
| Mac (receives PC capture of CABLE) | 21.7-29.8 ms | 58.5 ms | 19.8-28.1 ms | 0-6 | 0-4 | 3 (once) |
| PC (sounddevice CABLE Input callback) | 20.7-26.0 ms | 55.0 ms | 21.4-24.1 ms | 2-8 | 0-7 | 0 |

The buffers stay bounded and on target for 15 minutes; no trend. Weakness found: the v1
integral (1 ppm/(ms.s), clamp 500) wound up to +500 ppm on the PC side, driven by the frames
that expansions add after underruns, not by drift (real drift ~17 ppm). The underruns came
from PortAudio's callbacks pulling two periods back to back. v2 (current code) adds the
playback-clock jitter to the target and makes the integral slow and clamped to +-200 ppm;
10-minute rerun (`out/drift_chain_600b`):

| side | level mean range | level max | target mean | underruns / min | integral at end of each minute |
|---|---|---:|---:|---:|---|
| Mac | 29.2-32.8 ms (40.0 in the outage minute) | 93.3 ms (outage, flushed) | 29.5-30.2 ms | 0-5 (10 in the outage minute) | +7 .. +22 ppm (fit: +18.1) |
| PC | 27.7-35.9 ms (40.8 in the outage minute) | 91.4 ms (outage) | 26.9-36.4 ms | 0-5 (10) | -6 .. +15 ppm (fit: -18.1) |

The integral now stays in the drift range instead of saturating, and the buffers remain bounded
(one Wi-Fi outage at minute 3, RTT 119 ms, was flushed within a second). But the extra
playback-jitter provision raised the level by ~6 ms per side (loop p50 172.8 vs 157-164 ms)
while underruns only fell from 3.9 to 3.4 per minute: most underruns are Wi-Fi delay spikes
above the p97 provision, not PortAudio. The Windows tool's IAudioClient3 path (`chain_wintool`)
had 0 underruns on both sides in its 60 s run. Recommendation: provision playback jitter only
for callback-driven paths that need it, and accept a few PLC expansions per minute on Wi-Fi
(inaudible on the test signal) rather than +10 ms of buffer.

## Mac->PC jitter

2026-11-07 evening. Scenario 1 (Mac mic -> PC VB-CABLE) had ~62 glitches/min, the PC jitter
buffer underran ~50 times in 72 s, and every Mac->PC path looked "jittery" (5-15 ms) and ~40 ms
slower than PC->Mac. **Root cause: the PC's Wi-Fi adapter (MediaTek RZ616, driver setting
"Power Saving: Auto") dozes when the PC only receives; the access point then holds the Mac's
packets for 20-130 ms and releases them in a burst.** It was not the Mac: not AWDL, not Python,
not the capture. Fix in the transport: the receiving side sends a tiny authenticated receiver
report every 10 ms (any transmission keeps the radio awake), plus jitter-buffer changes for the
remaining Wi-Fi spikes. Scenario 1 over 10 min: **2.1 glitches/min (0.9 episodes/min), one-way
p50 108 ms**, against 50 glitches/min and 118 ms for the old code in the same conditions.

### Method

- **`jprobe.py`**: raw UDP, no audio and no crypto. The Mac and the PC each send 80-byte
  packets every 10 ms (or not at all: `--interval-ms 0`), stamped with the scheduled and actual
  send times. The other side logs arrival times; pings give the clock offset (lowest-RTT 30 %,
  linear fit). Reported per direction: delay relative to the 1st percentile, spikes (runs of
  packets > 10 ms late) per minute, peak height, spectrum. 60 s per run, LAN unless noted.
- **`peer.py --pkt-log`** in the real scenario: per frame, the Mac logs when its capture
  delivered the samples (`tap.py` chunks), when `Sender` got them and when it sent them, and the
  encoded frame itself. The PC logs each arrival and each jitter-buffer pull (level, target,
  events). Joined with the ping clock, every frame's delay splits into
  capture -> send (Mac) / send -> arrival (network + PC receive) / buffer.
- **Offline replay** (scratch scripts, not kept): the recorded arrival and pull times plus the
  recorded Opus frames, fed through `JitterBuffer` on the Mac. It reproduces the live run
  exactly (same expand/drop counts, same detector events), so buffer policies can be compared on
  identical network traces.

### Evidence

**1. The Mac sends on time.** In the scenario, capture delivery -> `sendto` is 0.58 ms p50 and
0.85 ms p99 (s1_t1, 7570 frames). There is one > 10 ms event per run, at start-up. The
`capture-mic` chunks (128 frames) arrive 2.67 ms apart with < 0.1 ms p99 lateness. A paced
Python sender oversleeps `time.sleep` by a steady 3.0-3.2 ms (max 5.0: timer slack). That is a
constant offset, not jitter. The transport does not depend on it, because it sends as audio
arrives. Hypothesis 2 is ruled out.

**2. The delay is added between the Mac's send and the PC's receive, in bursts.** Before
the fix, 15 spikes/min. Typical pattern: arrivals 10, 10, 10 ms apart, then a **36 ms gap
followed by 3 packets within 0.1 ms**, delays 29/18/8 ms (s1_t1). Every jitter-buffer expand
lined up with one of these spikes, and none came from the Mac side.

**3. Dose-response on what the PC transmits** (`jprobe.py`, Mac sends every 10 ms, PC as listed):

| PC transmits | Mac->PC spikes/min | p97 / p99.9 / max (ms) | runs |
|---|---:|---|---|
| nothing (receive only, like scenario 1) | 20, 60, 59, 46 | 2.4-26.8 / 27-88 / 30-129 | `rxonly1-4` |
| to the Mac every 100 ms | 19 | 2.3 / 25.2 / 28.4 | `srv100` |
| every 50 ms | 8 | 2.3 / 18.2 / 30.3 | `srv50` |
| every 20 ms | 6 | 2.3 / 9.5 / 18.4 | `srv20` |
| every 10 ms | 7, 3, 2 | 2.3-2.5 / 6.5-10.7 / 14-29 | `base1-3` |
| **nothing to the Mac, 16 B every 10 ms to the router** | **1** | 2.3 / 6.3 / 11.4 | `rxonly_ka` |

The last row decides it. The Mac's traffic is identical across rows; the spikes depend only on
whether the PC transmits, even to a third host. That points to the PC radio's power save,
which the receive-only scenario triggered. In the other direction (PC->Mac) the Mac receives
cleanly: p97 1.4-1.6 ms. The Mac sends pings 4/s and its driver does not doze the same way.
`Get-NetAdapterAdvancedProperty`: "Power Saving: Auto", "U-APSD: off". The setting was **not**
changed (rule: no system settings; the transport must cope).

**4. Other hypotheses, measured:**
- *AWDL / Mac radio*: with the PC awake, the residual Mac->PC spikes (0-7/min, mostly 8-15 ms)
  are not periodic (no peak in the delay spectrum, phase locking R < 0.3 at 0.1-2 s periods).
  Only the largest (22-29 ms) coincide with a spike in the other direction, i.e. a medium-wide
  event. AWDL is not a visible contributor here.
- *WMM voice marking* (Mac `SO_NET_SERVICE_TYPE` = VO, or `IP_TOS` EF 0xb8): 1 and 0 spikes/min
  vs 2-7 unmarked. That is within run-to-run noise on this uncongested channel (5 GHz ch 52,
  160 MHz, -52 dBm), so it is not adopted. It is harmless and may matter on a busy network.
- *Tailscale vs LAN*: receive-only over Tailscale 47 spikes/min (LAN 46-60), 12/min with the
  PC sending (LAN 2-7). Same mechanism; the path does not matter.
- *Packet size*: 400 B instead of 80 B, receive-only: 49/min. No effect.
- *PC receiver*: pull thread on time (p99 1.0 ms late), `play` queue never ran dry, CABLE capture
  continuous. With the PC awake the same code had 0 expands in 75 s (s1_rr1).
- *Why scenario 3 (Mac sound -> PC) looked better*: the Mac's 512-frame tap delivers in
  10.67 ms chunks, which reads as ~10 ms of "jitter" (p97 10.7 ms). The buffer then sat at
  ~31 ms, which happened to cover most of the 20-30 ms spikes. The "10-15 ms Mac->PC jitter"
  of the scenario README was mostly tap framing; the extra ~15 ms of buffer was its cost.

**5. A measurement trap found on the way: Opus decoder state.** With every transport event
removed, the pilot detector still reported 5-11 glitches/min at the jitter-buffer output.
Offline, the live bitstream decoded from frame 0 gives 0. Decoded from frame 1, 2, 5, 10 or 50,
or with a single PLC frame at 30 s, it gives **9-16/min for the rest of the run**, right after
the -10 dBFS marker chirps. A decoder that has missed one frame renders the pilot with 6 dB dips
after later chirps (encoder mode transitions). The buffer used to skip frames when flushing at
start-up, so the PC decoder was never in step. Fixes: `_flush_to_target` now decodes the frames
it discards, and the mic scenarios use -20 dBFS chirps. At that level the out-of-step floor is 0
and all 66 markers are still found offline. Not a transport fault, but it hid one, so every
earlier mic-path glitch figure includes some of it. Real speech was not tested.

### Fix (`peer.py`, `media.py`)

1. **Receiver reports (`RR`, `--rr-ms 10`)**: while media arrives and this side has sent
   nothing for 10 ms, send a 46-byte sealed packet (newest ts, buffer level) to the peer.
   Cost: 100 packets/s (~60 kb/s on the air), < 0.5 % CPU. Sent only when needed: none when the
   side already sends media (scenario 5), none over TCP. Effect alone (75 s, s1_rr1): network
   spikes 15/min -> 4.8/min, jitter-buffer expands 9 -> 0.
2. **Catch up instead of dropping**: after a late burst, resample up to 2000 ppm instead of
   dropping a non-silent frame after 2 s (drop only if > 30 ms over target for 2 s). Replayed on
   the same traces: drops 6 -> 0 (s1_t1) and 2-7 -> 0-2 (5-min runs), same latency.
3. **Spike-aware target (`--jb-peak-hold 10`)**: after two spikes > 15 ms within 10 s, provision
   for the highest until 10 s after the last. Replayed: with RR, events 1.4 -> 1.2/min at +1 ms
   median level. Without RR (the safety net if another adapter dozes anyway), 5.5 -> 1.6/min at
   +10 ms. A 20 s hold cut no more events and doubled the level p95 after a Wi-Fi episode
   (100 vs 56 ms).
4. **Decode, don't skip, flushed frames** (point 5 above).

### Before / after, scenario 1 (Mac mic -> PC CABLE Output), same Wi-Fi, same test signal

| run | settings | length | glitches/min (episodes/min) | one-way p50 / p95 | JB underruns / drops / flushed | network spikes > 10 ms /min | JB level mean |
|---|---|---:|---|---|---|---:|---:|
| `s1_b` (W2) | old code, -10 dBFS chirps | 72 s | 61.7 | 125.6 / 137.4 ms | 14 / 17 / 0 | - | 36.0 ms |
| `s1_before300` | old behaviour (`--rr-ms 0 --catchup-ppm 500 --drop-ms 15 --jb-peak-hold 0`) | 300 s | 50.3 (22.6) | 118.5 / 130.1 ms | 69 / 65 / 0 | 53.7 | 35.8 ms |
| `s1_after600` | defaults (RR + catch-up + peak hold 10 s) | 600 s | **2.1 (0.9)** | **107.7 / 115.8 ms** | 8 / 0 / 73 | 1.4 | 22.8 ms |
| `s1_after` | same, peak hold 20 s | 300 s | 2.9 (1.3) | 119.2 / 196.1 ms | 7 / 0 / 14 | 10.0 (one 3.4 s episode) | 32.0 ms |

Episodes = detector events closer than 0.5 s merged. One expand often shows as 2-4 events
while the pilot settles. All 13-19 events in the "after" runs lie at jitter-buffer events, none
elsewhere. Latency fell ~11 ms because the buffer no longer carries the expansions (level 36 ->
23 ms). The remaining events are Wi-Fi weather that no buffer setting short of +50-100 ms would
cover. s1_after600 had 8 short underruns plus one 0.66 s stall (73 frames flushed). The 5-min
runs each had one 2-3.4 s episode of 45-130 ms delays, about every 100 ms, with RR still flowing
(a background scan or radio event; not identified). The new scenario rows are in
`../scenarios/README.md`.

### Recommendations for Beamer

- Keep the receiver report (or any feedback at the frame rate) in the product whenever a side
  only receives. It is the main fix and costs nothing in latency. The PC's power-save setting is
  the user's and may come back with any driver.
- Send/receive pacing does not matter (the Mac's send path is clean); DSCP/WMM marking is
  optional.
- When judging codec-path glitches with a pilot, start the decoder in step with the encoder
  and keep it there, or the codec's own state mismatch will be counted.

## Firewalls

- **Mac**: the client never listens: a `connect()`-ed UDP socket that sends first. To make sure
  the result does not depend on an existing exception (the mise Python 3.13.18 that `uv` uses
  is already "Allow incoming" in `socketfilterfw --listapps`), one full session ran from a
  fresh copy of the interpreter, re-signed ad hoc with a new identifier
  (`org.beamer.alftest`, never seen by the firewall): connected in 216 ms, full echo loop
  (64.9 ms p50), **no dialog, no `socketfilterfw` log entry about it, not added to
  `--listapps`**. Firewall stayed on (state 1, stealth on). `log show --predicate 'process ==
  "socketfilterfw"'` only shows Tailscale flows during all runs.
- **PC**: inbound rule `Beamer-dev-audio-udp` (UDP 24823, Private, program
  `C:\Users\Alex\AppData\Roaming\uv\python\cpython-3.13.11-windows-x86_64-none\python.exe`, the
  base interpreter that actually owns the socket; the venv's `python.exe` is a launcher).
  Without it, the Mac's first datagram is dropped (auto mode fell back to TCP after 2 s).
  **With the rule disabled, `serve --punch MAC:PORT` works**: the PC sends a few junk datagrams
  from 24823 to the Mac's port first (the port the link would carry in `audio_open`), WFP's
  UDP state then admits the Mac's packets: connected in 216 ms, echo loop 66.3 ms p50. The
  Mac drops the junk (authentication fails) and its firewall said nothing. **So production
  needs no new Windows rule if the PC also probes**; a `Beamer Media (UDP-In)` rule is still
  worth adding for robustness.
- TCP fallback (`--transport auto`): UDP blocked -> fallback decided after 2.0 s, TCP up 216 ms
  later. Echo loop over TCP on the same LAN: 65.3 ms p50, 72.4 p95, 82.4 max
(UDP: 60.6-67.6 / 68-70): without loss TCP costs nothing; a lost segment would stall for an RTO
(not exercised: loss injection happens above the transport). The first run exposed a spin
loop in the abandoned UDP receive thread (113 % CPU), fixed.

## Latency budget for voice in video calls

ITU-T G.114: mouth-to-ear one way < 150 ms is comfortable for nearly all users, 150-400 ms
acceptable with impact. A video-call app already spends 50-100 ms of its own (capture buffers,
its encoder and jitter buffer, network, playout), plus internet transit. **Beamer's hop (Mac
mic -> virtual mic on the PC, or the reverse) should therefore stay <= 50 ms, 80 ms at worst.**

| stage (Mac mic -> PC app) | budget | measured now | config |
|---|---:|---:|---|
| Mac mic buffer | 3 | 2.9 | 128-frame IO buffer (T2) |
| Opus framing + lookahead | 6.5 | 6.4 | 10 ms frames, VOIP; framing itself is inside the JB figure |
| network one way (LAN) | 3 | 2.7 (p95 4) | UDP 24823 |
| jitter buffer | 20 | 20-24 | adaptive: block + frame/2 + p97 jitter + 2 ms; wired LAN would be ~15 |
| PC render into virtual mic + cable + app capture | 15 | **90-100** | VB-CABLE at 7168 samples; see open issues |
| **total (excluding mic DSP)** | **~48** | **~125-135** | |

Reverse direction (PC system sound -> Mac speakers): WASAPI loopback 30-40 ms after render
(T4) + transport 30-34 ms + Mac playback (prefill 10 ms + 4.3 ms at 128 frames + speaker DSP)
~ 80-95 ms: below the ~125 ms where late audio against video becomes detectable (ITU-R
BT.1359), so no video delay is needed.

**Recommended configuration**
- Codec: Opus 48 kHz. Mic: mono, `OPUS_APPLICATION_VOIP`, 32 kb/s, **10 ms frames**, in-band
  FEC on with `PACKET_LOSS_PERC` 10, DTX off, complexity 9. System sound: stereo
  `OPUS_APPLICATION_AUDIO` 96-128 kb/s, 10 ms, FEC off (FEC=1 forces SILK/hybrid even at 96k
  stereo, bad for music; use 0 or 2), or RESTRICTED_LOWDELAY when lip-sync matters (-4 ms).
  PCM only as a LAN-only debug option.
- Jitter buffer: adaptive, target = playback period + frame/2 + p97(network transit) +
  p97(playback-clock jitter) + 2 ms, min 10 ms, max 120 ms, spikes > 150 ms treated as outages
  (expand, re-buffer, flush the backlog). Typical result: 20-25 ms on Wi-Fi.
- Drift: PI on the buffer level -> windowed-sinc resampling, +-500 ppm total, integral clamped
  to +-200 ppm; drop silent frames for small excesses, flush for large ones. **The receiver
  must be pulled by the playback device's callback** (in process, or by `play` reporting its
  queue level): with a stdio pipe into `play`, the pacing clock is the host clock, not the
  device's, and `play` has no drift correction (its queue moved between 6.7 and 14.7 ms in 70 s).
- Transport: UDP 24823, Mac connects; PC also probes towards the Mac's announced port (no rule
  needed); fall back to Opus frames on the link after 2 s without UDP.

## Firewall rules on the PC

`Beamer-dev-audio-udp` (UDP 24823) and `Beamer-dev-audio-tcp` (TCP 24823, fallback test) were
added for the tests and **removed at the end**; none is left. `run_pair.sh` now uses the punch
mode (`--local-port 40123` on the Mac, `--punch MAC:40123` on the PC), verified without any rule
over the LAN (64.2 ms echo loop) and Tailscale (57.5 ms).

## Open issues

- **VB-CABLE latency (~100 ms)** blows the budget. Its max-latency setting (registry value above,
  set by the VB-Audio control panel) can be lowered to 512-2048 samples, but needs a driver
  restart and touches the device the mic agent installed: operator decision. Longer term, a
  Beamer virtual mic on Windows.
- The PC receive path through PortAudio still had 2-8 expansions per minute (callbacks in
  pairs); the Windows tool's IAudioClient3 128-frame path had none in its 60 s run. Phase 2
  should drive the jitter buffer from that tool's render callback (in process or a
  level-reporting pipe), with 2.67 ms periods the JB target drops by ~7 ms.
- Frame drops for excess level are audible on non-silent audio (rare: after outages); a
  WSOLA-style time compression (as WebRTC NetEq's accelerate) would hide them.
- Python CPU (10-17 % of a core on the Mac) is mostly the prototype's threads, stats and
  analysers; the codec is ~1 %. In Beamer, keep encode/decode in C (ctypes releases the GIL) and
  the jitter buffer off the link threads.
- Wi-Fi stalls (up to 1.4 s once) are the main source of glitches; nothing in the transport can
  hide them beyond PLC + quick re-sync.
- FEC benefit on real speech is not measured (synthetic voiced signal only); a speech corpus run
  would quantify it.
- Mic DSP latency on the Mac is only known as part of the 44 ms acoustic loop (T2).
