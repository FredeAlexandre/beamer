# WP2 report: portable media engine (branch `audio/wp2`)

**Done.** `native/audio/engine/` (C++17): `backend.h` byte-identical to §2.4 (first commit, unchanged since; `engine.h` = std includes + contract), RFC 8439 AEAD on Monocypher primitives, packets + 128 replay window, Opus, `JitterBuffer` + resampler + drift (port of `media.py`), levels, SPSC rings, flows (UDP, probe, ping, RR, replay, link fallback), send/recv/loop/test_input streams, `run_helper`/control loop, null backend. Vendored Opus 1.6.1 / Monocypher 4.0.2 / nlohmann-json 3.11.3 (checksums equal to published; json GPG signature good; all GPL-3.0-compatible; `third_party/README.md`). `build-tests.sh` (clang, no CMake; `--asan`/`--tsan`) and `build-tests.cmd` (MSVC via vswhere), `engine_tests`, `beamer-audio-null`, `tests/two_helpers.py` (acceptance driver, stdlib only), `tests/jb_crosscheck.py`, `native/audio/README.md`, `.gitignore`. Builds use `-ffile-prefix-map=<repo>/=`: no `/Users/` string in the binaries.

**Acceptance (measured)**
| Test | Mac (M5) | PC (7600X, MSVC) |
|---|---|---|
| `engine_tests` 16 cases / 1883 checks | PASS plain, ASan+UBSan, TSan (0 sanitizer reports) | PASS, no /W4 warning in engine code |
| AEAD RFC 8439 + 26 PyNaCl vectors, tamper/replay/window/wrong key | PASS | PASS |
| Resampler SNR at 1 kHz, -500..+500 ppm | PASS (>= 80 dB) | PASS |
| JB sims (2 % loss, Gilbert, 0-20 ms jitter, 0.7 s stall, +-100 ppm), worst 10 s mean abs(level-target) | 2.7 ms (10 min in 0.6 s), Gilbert 5.0, stall flushed 69 frames, back on target < 2 s; 0 frames skipped undecoded | same |
| Port fidelity: reference mode vs `media.py` on the same traces | level/target diff 0.000 ms at all 225,001 pulls, identical counters | - |
| RR timing (rr_ms 10) | 90-106/s receive-only, 0-8/s while sending | 97/s, 58-75/s while sending (bursty null sender) |
| Golden control session (all 13 §3.2 commands + error codes, 2 helpers) | PASS | PASS |
| 2 helpers, 120 s, mic+system both ways, 2 % loss, +80 ppm: level p99 (run) - target | 11.3-13.4 ms PASS | 8.6-13.0 ms PASS |
| decode errors | 0 PASS | 0 PASS |
| fallback to `link` after UDP drop / back to `up` | 2.00 s / 1.00 s PASS | 2.00 s / 1.02 s PASS |
| CPU per helper (4 streams) | **FAIL**: 2.5-7.8 % across runs (5.7 % in the final) | **FAIL**: 3.0-4.9 % |
| Full Windows helper (WP4 `build-win.cmd` + this engine) | - | links, static x64; `version`, `list`, control smoke PASS |

CPU: Opus is ~1.1 % of a hot P-core per helper. In the helper the same encode takes 118-160 us/frame vs 46 us hot: each 10 ms burst runs cold, at low clock, often on E-cores (QoS user-interactive and a time-constraint policy did not change it). Wakeup cuts did not change instructions much: deadline-driven net poll (<= 10 ms, 2 ms in link mode), no per-loop allocations, p97 recompute <= every 20 ms. NEON intrinsics give no gain on arm64 (measured). Windows process time is 15.6 ms tick-quantised. Single mic send (WP3's < 1 % target): 2.2-2.8 % with the null mic (encoder ~1.4 %); not met.

**Deviations from PHASE2, with reasons**
- JB `merge_expand` (default on): a frame concealed by expansion that proves lost is not concealed again. In `media.py` each such loss adds a frame of level, and bursty loss kept the buffer 10-20 ms above target. `jitter_recalc_ms` 20 (CPU). Both off = exact reference.
- Mic Opus complexity default 5, not 9 (operator note). Encoder CPU halves; masked log-spectral distance 2.59 vs 2.35 dB (c6 2.42) on voiced speech. Optional `codec.complexity` 0..10.
- `flow_open.host` (operator note): with `port` 0 the socket is connected towards host:24823 with no bind (peer.py's Phase 1 V3 way), then re-connected by `flow_remote`; `remote:null` without `host` binds 0.0.0.0 (tests only). Also `flow_open.udp:false` (link-only flow).
- In `link` state media go over the link and also over UDP (deduplicated by the replay window), so recovery is seen at once. Receiving `link_in` also puts a side in `link` (one-way blockage).
- `stats`: `{"t","cpu_pct","streams":{id:{...}}}`, counters per interval, plus `jb_p99_ms`, `run_jb_p99_ms`, `enc_us`, etc. `permission` `suspect` is reported for the mic only.
- `dev` command (`--dev`): `drop_udp`, `loss_pct`, `reset_stats`, `null:{vmic_in_use,remove,add}`. Monocypher is listed in `sources.txt` as a `.c` (both OS build scripts then skip their own copy).
- Engine applies the 3 s `in_use` off-delay; backend `*err` "<code>: msg" is mapped to the §3.3 code; role != Normal is filtered.

**Needs from other WPs / operator**
- PHASE2 §3 (not mine): document `host`, `udp:false`, `dev`, `complexity`, the stats shape. WP1: send `host` in `flow_open`; read stats counters as per-interval.
- WP3: pass `-ffile-prefix-map` in `build-mac.sh`. The vmic pilot glitches are not in the receive path. 3 of 7 detector events sit 40 ms after chirp markers (`acceptance.py` calls `pilot_glitches` without `markers=`). The rest follow a single startup expansion: the Phase 1 Opus decoder-state effect on later chirps, invisible in WP3's check because it read only the last 1 s stats interval. With markers excluded and no expansion: 0 events (null-backend replay, -26 ppm).
- WP4: define `PACKAGE_VERSION="1.6.1"` for Opus (`version` says "libopus unknown"); `features` advertises `aec` before WP9 exists. `backend.h` kept verbatim: I did not add a guard/includes (nothing for the operator to reconcile).
- Operator: decide whether the CPU criterion is a P-core budget (met: ~2.6 % at 4.4 GHz by cycles) or a time budget (not met on this Mac).

**Commits**: 634daf3 backend.h; 93e6c55 third_party; 84a50f0 engine + tests; the final commit adds this report and the fixes above.
