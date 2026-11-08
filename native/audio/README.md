# native/audio: Beamer's audio helper

One portable C++17 engine (`engine/`) shared by one helper executable per OS (`beamer-audio`,
`beamer-audio.exe`). Beamer starts the helper and drives it with JSON lines on stdio
(`dev/audio/PHASE2.md` §3). The media plane (packets, AEAD, Opus, jitter buffer, drift, UDP flows
with link fallback) is §4. This file covers the engine; the OS backends (`mac/`, `win/`) and their
build scripts have their own notes.

## Layout

| Path | What |
|---|---|
| `engine/backend.h` | the backend contract, verbatim PHASE2 §2.4 (no includes: include `engine/engine.h`) |
| `engine/engine.h` | std headers + `backend.h` + version/platform constants: what backends include |
| `engine/sources.txt` | the engine's sources (relative to `engine/`; `.cpp` C++17, `.c` C: Monocypher), for every build script |
| `engine/crypto.*` | RFC 8439 §2.8 ChaCha20-Poly1305 on Monocypher's chacha20_ietf + poly1305 |
| `engine/packet.*` | 24-byte header, `Sealer`, 128-packet `ReplayWindow`, `open_packet` |
| `engine/codec.*` | `codec` object (§4.4), Opus encoder/decoder wrappers |
| `engine/resampler.*` | 16-tap Kaiser sinc, 512 phases; `DriftController` (PI + catch-up) |
| `engine/jitter.*` | `JitterBuffer`: port of `proto/transport/media.py` (identical in reference mode; two engine additions, see below) |
| `engine/spsc.h`, `levels.h` | lock-free SPSC rings; level meters (IO thread posts, control thread drains) |
| `engine/net.*`, `flow.*` | UDP sockets; `Flow` (probe, ping, RR, replay, link mode) and the network thread |
| `engine/streams.*` | `SendStream`, `RecvStream`, `LoopStream`, `TestInput` |
| `engine/helper.*` | `run_helper` (command line) and `run_control` (the §3 control loop) |
| `engine/aec.*` | echo cancellation (§7): `EchoCanceller` (speexdsp MDF + residual echo suppression), `HostClock`, `AecPath` (reference capture, alignment, drift), `engine_features()` |
| `engine/null_backend.*` | `make_null_backend` (§11.3) |
| `engine/test_signal.*` | signatures A/B of `proto/scenarios/sig.py`, sine, noise (null devices, dev `test` source) |
| `tests/` | `engine_tests.cpp`, `golden_control.txt`, `vectors/` (+ `gen_vectors.py`), `null_main.cpp`, `two_helpers.py`, `jb_crosscheck.py` |
| `third_party/` | Opus 1.6.1, Monocypher 4.0.2, nlohmann/json 3.11.3, speexdsp 1.2.1 (see its README) |

## Build and test

macOS/Linux (clang; Command Line Tools are enough):

```
native/audio/engine/build-tests.sh [--asan|--tsan] --run    # build + engine_tests
python3 native/audio/tests/two_helpers.py --bin native/audio/build/tests/beamer-audio-null
```

Windows (MSVC; finds `vcvars64.bat` through `vswhere` when `cl` is not on PATH):

```
native\audio\engine\build-tests.cmd run
py -3 native\audio\tests\two_helpers.py --bin native\audio\build\tests\beamer-audio-null.exe
```

All builds pass `-ffile-prefix-map=<repo>/=` so no home-folder path ends up in a binary (the
helper build of the Mac app must do the same: WP8 refuses such binaries). Outputs go to
`native/audio/build/` (git-ignored): `tests[-asan|-tsan]/engine_tests`,
`beamer-audio-null`, objects cached under `obj/<variant>/`. A helper build compiles the files of
`engine/sources.txt` (`.c` as C) with include paths `engine/`, `third_party/opus-1.6.1/include`,
`third_party/monocypher-4.0.2/src`, `third_party/nlohmann-json-3.11.3/single_include`, and links
libopus built from its `.mk` lists exactly as `build-tests.sh` does (Windows: also `ws2_32 bcrypt
winmm`; `bcrypt`/`winmm`/`ws2_32` are also requested by `#pragma comment`).

`beamer-audio-null --control ... --backend null:<options>` is the real control loop over virtual
devices (`engine/null_backend.h` documents the options, e.g. `ppm=+80;mic=A;out=record:/tmp/o.wav`).
`--backend null:` also works on the real helpers (run_helper swaps the backend), and
`beamer-audio list|restore|version` print JSON diagnostics.

## Threads (§3.1)

main = dispatcher (commands and internal events from one queue); a stdin reader thread; the network
thread (`poll`/`WSAPoll`, 2 ms, all flows: reception, decryption, replay windows, hello/ping/RR
timers, state machine, `link_out` batching every 20 ms); one encoder thread per send stream (woken
by the capture callback through a mach semaphore / Windows event once a frame is buffered); a 50 ms
timer (levels, 1 s stats, `in_use` off-delay, test inputs); the backend's IO threads, which only
touch SPSC rings and atomics. A recv stream's jitter buffer, decoder and resampler live entirely on
the render IO thread: the network thread hands authenticated packets over through an SPSC ring that
the render callback drains before each pull.

## Notes for the backends (WP3, WP4)

- Include `engine/engine.h` (not `backend.h` alone).
- `*err` from `open_capture`/`open_render`: start it with an error code of §3.3 (`"no_device: ..."`,
  `"device_busy"`, `"permission: ..."`, `"blocked_device"`, `"no_virtual_mic"`, `"unsupported"`);
  anything else is reported as `internal`.
- `vmic_in_use`: report the raw state and call `devices_changed()` promptly when it flips (no
  300 ms debounce needed for that). The engine applies the §3.3 "off after 3 s" delay itself.
- `host_ns` must be the host time of the block's first sample, in one time base for every capture
  of the backend (Mac: `mach_absolute_time` in ns, Windows: QPC in ns): the AEC aligns the reference
  to the mic with it. The jitter buffer times pulls with the engine's own steady clock.
- Feature `aec` from a backend means "can capture a `CaptureKind::Reference`"; the engine keeps it
  only when the canceller is compiled in (`engine_features()`).
- `channels` in the callbacks may be anything; the engine maps (mono = average, stereo into the
  first two channels). Render callbacks of any size are fine (pulled in chunks of up to 2048).
- `stream_lost` may be called from any thread; the stream is then reported `failed` (`no_device`).

## Jitter buffer: port and additions

`JitterBuffer` reproduces `media.py` exactly with `merge_expand = false, jitter_recalc_ms = 0`:
`tests/jb_crosscheck.py` replays the arrival traces that `engine_tests` dumps (`BA_SIM_REFERENCE=1
BA_SIM_DUMP=<prefix>`) through the Python class and finds 0.000 ms difference in level and target
at every pull (225,001 pulls, 10 min, 2 % loss, 0-20 ms jitter, +100 ppm; and a Gilbert trace), with
identical counters. Defaults add two things:
- `merge_expand`: a frame concealed by expansion (nothing newer had arrived) that then proves lost
  (a newer frame came, it never did) is not concealed a second time: the expansion was its
  concealment. In media.py every such loss adds a frame of latency that catch-up (<= 2000 ppm)
  takes ~5 s to remove; with bursty loss the buffer sat 10-20 ms above target. Late frames keep
  the reference behaviour.
- `jitter_recalc_ms = 20`: the p97 transit statistic is recomputed at most every 20 ms instead of
  after every packet (it was most of the render thread's CPU).

## Protocol details and extensions (beyond the §3 tables)

- `flow_open` takes an optional `host` (the peer's IP from the link). With `host` and `port` 0 (the
  side that must not listen: the Mac), the socket is **not bound**: it is `connect()`ed towards
  `host:24823` and gets an ephemeral port, like `proto/transport/peer.py`'s `UdpClient`, which Phase 1
  (V3) showed raises no macOS firewall dialog. `flow_remote` re-connects it to the answered port and
  starts probing. With a `port` (the answerer) the socket binds that port (then +1..+7, then any) on
  the interface that routes to `host`/`remote.host`. With neither, it binds `0.0.0.0:0`: tests on
  127.0.0.1 only, never on the Mac's LAN.
- `flow_open` `"udp": false`: a link-only flow (no socket; state `link` at once) for the loopback/no
  host case of §4.2, instead of waiting 2 s for the fallback.
- Every flow emits `flow_state` `probing` at open. `rtt_ms` is set on `up` events, else null.
- In `link` state the sender queues media for `link_out` **and** keeps sending it on UDP (and RR,
  hello 1/s): the receiver's replay window drops duplicates, and UDP recovery is seen at once (back
  to `up` after 1 s of received UDP: 1.0 s measured). A side that receives `link_in` packets (peer
  in link mode) also switches to `link`, so one-way UDP blockage works in both directions.
- On a peer `bye` the flow goes back to `probing` (it closes only on `flow_close`).
- `stats`: `{"ev":"stats","t":ms,"cpu_pct":x,"streams":{"<id>":{...}}}`, counters are per interval.
  recv: `path jb_ms jb_p99_ms jb_max_ms target_ms late expand drop flushed loss_pct fec plc underrun
  dec_err ppm ppm_i rtt_ms kbps rx rx_link ring_full playing run_jb_p99_ms run_target_ms` (the
  last two over the stream's life or since dev `reset_stats`); send: `path kbps bitrate jb_ms` (the
  receiver's level from its RR) `rtt_ms frames overflow dev_dropped rr_rx enc_err`.
- `levels` keys are device keys (`device_in_use()`), streams by id.
- `send.aec` (`{"reference": key|null}`, mic only): see "Echo cancellation" below. `loop` ignores `aec`.
- `permission` is emitted once after `ready` (`mic`, `system_audio`), and `{"mic":"suspect"}` when a
  mic capture delivered only digital zeros for 5 s (never for system sound: a silent tap is zero too).
- `codec.complexity` (0..10, optional): default 5 for `voip` (the mic), 9 otherwise.
- `dev` (only with `--dev`): `{"cmd":"dev","flow"?:id,"drop_udp":bool,"loss_pct":0..100,
  "reset_stats":true,"null":{"vmic_in_use":bool,"remove":key,"add":key}}`: block UDP, drop media at
  send, restart the run statistics, drive the null backend.
- Errors without a `req` are still emitted (without `req`); `ping` is answered by `pong` only.

## Echo cancellation (§7, `engine/aec.*`)

A mic `send` with `aec` opens a second capture, `CaptureKind::Reference` of the output device
(`reference` key, null = the default output, followed by the backend), into an `AecPath`: the
reference IO thread writes mono samples into an SPSC ring plus one `(index, host_ns)` mark per
callback; the mic IO thread adds the same marks for the send stream's ring. The encoder thread then
runs 10 ms mono frames through the canceller before Opus:

- `HostClock` per stream: sample index -> host time (offset smoothed over ~0.5 s; a jump > 20 ms
  restarts it). The reference sample fed with mic sample `m` is the one captured at host time
  `t_mic(m) + lead_ms` (default -1 ms), so the filter sees the echo at the acoustic delay by
  timestamps (MacBook Air: ~44-60 ms) inside its 100 ms.
- Drift: the reference is read through the engine's `Resampler`; a PI loop (wn 0.5 rad/s) on the
  position error steers its step, so devices on different clocks stay aligned (tested at +-100 ppm
  with 0.5 ms timestamp jitter). An error > 20 ms (restart, lost blocks) re-aligns at once.
- A mic frame waits for its reference at most `max_wait_ms` (30) of mic backlog; missing samples are
  zeros. No reference callback for 100 ms of mic time = no reference (the mic runs through the
  canceller with silence, no wait). A reference that cannot be opened leaves the mic without AEC.
- `EchoCanceller`: speexdsp 1.2.1 `speex_echo_cancellation` (frame 480, filter 4800, 48 kHz, int16
  at the API) + `speex_preprocess_run` with only residual echo suppression (denoise stage on with a
  0 dB noise floor, -40 dB echo / -15 dB during double talk; AGC, VAD, dereverb off).
- `stats` of the send stream: `aec {on, ref, error?, atten_db, echo_ms, err_ms, ppm, resync,
  ref_short, no_ref, waits, us, cpu_pct}`: `atten_db` = mic energy in / out over the interval,
  `echo_ms` = position of the strongest filter tap minus the lead (the acoustic delay by
  timestamps), `us`/`cpu_pct` = canceller time per frame / share of a core.
- `--dev` only: `aec.lead_ms`, `aec.max_wait_ms`, `aec.exclude_pids` (processes left out of the
  reference, for the live test's "near-end talker" played through the same speakers).
