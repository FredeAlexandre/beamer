# WP9 report: echo cancellation (branch `audio/wp9`)

**Done.** speexdsp 1.2.1 vendored (`third_party/speexdsp-1.2.1/`, the echo-canceller subset only, files identical to the official tarball; sha256 matches xiph's SHA256SUMS; GPG signature good, key D89B52F4…5B47, Tristan Matthews; BSD-3-Clause, recorded in `third_party/README.md`). `engine/aec.{h,cpp}`:
- `EchoCanceller` interface. speexdsp MDF: 10 ms frames, 100 ms filter. Plus the preprocessor's residual echo suppression only (-40 dB, -15 dB in double talk; noise floor 0 dB; no AGC, VAD or dereverb).
- `HostClock` and `AecPath`: the Reference capture goes into an SPSC ring with host-time marks. Reference and mic are aligned by host timestamps. The engine `Resampler`, steered by its own PI loop, corrects drift. A >20 ms jump re-aligns. Missing or stalled reference: no wait, silent reference. Max wait 30 ms.
- Wiring: `send.aec` (mic only) is cancelled on the encoder thread before Opus. If the reference cannot be opened, the mic still runs, without AEC. `loop` ignores `aec` (§6.4). New per-stream `stats.aec`: `atten_db mic_db ref_db echo_ms err_ms ppm resync ref_short no_ref waits us cpu_pct`. `--dev` adds `aec.lead_ms/max_wait_ms/exclude_pids`.
- `engine_features()`: `aec` only when the canceller is compiled and the backend can capture a reference. Mac backend: `aec` with process taps. Null backend: a `ref` reference group.
- `beamer-audio version` now says `libopus 1.6.1` (Mac and PC) and `aec: speexdsp 1.2.1`.
- Build lists: `sources.txt`, `build-tests.sh/.cmd`, `build-mac.sh`, `win/build.ps1` (`PACKAGE_VERSION` for Opus in the last two). Docs in `native/audio/README.md`.

## Acceptance
| Test | Result |
|---|---|
| §11.2 synthetic echo (-11 dB, 44 ms + room IR, -65 dBFS noise, speech-like) | **PASS**: ERLE 22.0 dB over 5-12 s (17.3 dB at 1-3 s) |
| §11.2 near end only | **PASS**: -0.32 dB fresh, -0.46 dB after 5 s of echo adaptation |
| Alignment/drift (AecPath, timestamped blocks): same clock; 0.5 ms jitter; ±100 ppm; restart losing 60 ms | **PASS**: ERLE 25.9 / 26.4 / 28.9 / 30.1 / 28.0 dB, echo found at the placed delay ±0.2 ms, 1 resync (2 for the restart) |
| Engine tests, Mac plain / ASan+UBSan / TSan | **PASS** 21 cases, 1915 checks (TSan found one race on the reference handle, fixed) |
| Engine tests, PC (`C:\Users\Alex\audio-wp9`, MSVC) + `build-win.cmd` | **PASS** 21 cases / 1915 checks; `beamer-audio.exe` links (static x64) |
| Python `core/tests/test_audio_*` with the new null helper | PASS, 132 tests |
| **Live Mac**, audio-lab lock held: far-end speech (`say`, -20 dBFS) by afplay on the MacBook speakers at the user's volume (56); built-in mic → real helper (mic send) → link → null helper decoding to WAV | **Echo -31.8 → -53.6 dBFS: -21.8 dB, PASS (≥ 15)**. `echo_ms` 44.9 (matches Phase 1's 44 ms), err 0.0 ms, 1 resync, 0 short |
| Live near-end speech level change. Near-end voice played by a second afplay on the same speakers, left out of the reference (`exclude_pids`), so the canceller treats it as a talker in the room | **-0.3 dB, PASS (< 2)** |
| Double talk (both voices) | Near end alone in that window: -34.0 dBFS. With far end + AEC: -36.1; without AEC (echo + near): -30.3. So the echo is removed and the near end loses about 2 dB (RES "active" -15 dB on echo-dominated bins). No listening check |
| AEC CPU | Mac: **2.0-2.9 % of a core, PASS (< 3), little margin**: 200-300 µs per 10 ms frame in the helper vs 41 µs hot; the same cold-burst effect as Opus (WP2). The helper went 3.6 → 6.3 %. `-O3`/`-ffast-math` gave nothing. PC: 1.1-1.2 % |
| Windows rule: PC default output G733 → `form: headset` → planner gives `aec` off (`test_aec_by_form_override_and_mode`) | PASS (by rule; no stream needed) |
| Windows canceller live: far end = test signal A rendered on the DualSense speakers (a speakers endpoint, not the default); reference = endpoint loopback of that endpoint; mic = Wave:3 (default) | **Partial**. Reference captured at -35 dBFS. The loop locks across the two devices' clocks (err ≤ 0.03 ms, ppm -5…+13, 0 waits/short, 1 resync). The Wave:3 delivered digital silence or exact zeros most of the time (muted?), so no echo reduction could be measured. I stopped there because someone may have been at the PC. Defaults untouched |

Notes: the RES gain floor lowers very quiet noise: white noise at -80 dBFS -6.7 dB, at -65 dBFS -0.8 dB; none at speech level. MDF's DC notch removes content below about 100 Hz. Live, the Mac noise floor came out about 10 dB lower with AEC.

Machine state: volume 56/50/100 unmuted, built-in defaults, no leftover process, lock released.

## Deviations
- `lead_ms` default -1 ms. The reference fed with mic sample t is the one stamped t - 1 ms, so a mic block need not wait for the next reference block. The filter then sees acoustic-by-timestamp - 1 ms.
- Drift uses its own PI loop (wn 0.2 rad/s), not `DriftController`. Tuned for buffer levels, that one took ~25 s and left 2.5 ms of error (ERLE 8-12 dB at ±100 ppm).
- Hunks are in `mac/backend_mac.mm` (2 lines: the `aec` feature, and Reference honouring `exclude_pids`), not in `tap_mac.mm`. Nothing in `loopback_win.cpp`: the WP4 reference works as is. `golden_control.txt`: the null backend now lists `aec`.

## Needs from other WPs
- WP1: the planner should set `aec` only when the holder's features include `aec`. Otherwise the UI may say "Beamer cancels the echo" with a helper that cannot.
- WP1/WP6: `stats.aec` exists (`on`, `ref_db`, `atten_db`) if the UI wants to show it working. On the Mac, the reference is a process tap: without the System Audio Recording permission, `ref_db` stays at -100 and AEC does nothing.
- Operator/PHASE2 §3: document the `aec` stats and the `--dev` fields. Also: `build-mac.sh` and `build-tests.sh` share `build/obj`, and a flag change in `build-mac.sh` wipes the test objects (they rebuild; harmless).

## Commits
abcb39a AEC engine, vendoring, wiring, builds, tests; 01b6a08 comment and C4267 fix; 2dcff80 mic/ref levels in stats; plus this report.
