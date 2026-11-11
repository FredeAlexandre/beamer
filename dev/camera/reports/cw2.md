# CW2 report: engine video transport

Branch `feat/camera`. Commits: `e0a4ef5` (contract header `video_backend.h`, verbatim §2.4, first commit),
`8444f79` (engine, null video backend, helper, tests), `434437d` (decoder re-creation, link-state seed, fine
timers, harness, MSVC test build). Needs Alex: nothing.

## Files

New (all `SPDX-License-Identifier: GPL-3.0-only`):

| File | What |
|---|---|
| `engine/video_backend.h` | §2.4 verbatim (unchanged since `e0a4ef5`) |
| `engine/video_packet.{h,cpp}` | 12 B video header (incl. `enc_delay`, `vflags` bit3), `nack`, `pli`, `time_map`, 28 B video RR, `video_ts` |
| `engine/sync.{h,cpp}` | `OffsetEstimator` (min-RTT of 10 pongs, §4.5.3 sign), `TimeMapBook`, `PlayoutTarget` (p99 + 8, [floor,150], rise at once, fall 15 ms/0.5 s, 70 ms until 20 samples), `PairPublisher`, `LipSyncHold` hook (CW10), `present_delay_ms` |
| `engine/pacer.{h,cpp}` | per-flow pacer: 32-packet bucket at 25 Mbit/s, IDRs at `idr_pace_x` x bitrate on an absolute schedule, retransmissions first |
| `engine/video_send.{h,cpp}` | `SenderRecovery` (pure §4.3 policy with explicit time) + `VideoSendStream` (numbering, queue, backpressure, hard cap, fragment/seal/history, NACK, PLI, verified LTRs, rate ladder, link state, camera-on guards) |
| `engine/video_recv.{h,cpp}` | `VideoRecvCore` (pure §4.5 rules with explicit time) + `VideoRecvStream` (network / video / decoder threads) |
| `engine/null_video.{h,cpp}` | null video backend (§10.3): fake encoder (frame numbers, refs, LTR every 30, refresh/IDR, pause, fps, format, `cam_start_ms`, standby), reference-checking fake decoder (coded 1088 vs display 1080, `size_rejected`), vcam state, dev controls and report |
| `tests/video_tests.cpp` | 20 engine video cases (§10.2), linked into `engine_tests` |
| `tests/two_helpers_video.py` | two null helpers, 11 acceptance cases |

Hunks: `packet.{h,cpp}` (ptypes 7/8/9, codec 2, `ReplayWindowN<N>`, `ReplayWindow` alias, `open_packet` template),
`flow.{h,cpp}` (dispatch, `send_video`, `send_feedback`, time map, video timers in `next_deadline`, `Waker`
socket pair, offset estimator, pacer, link events, `NetLoop` comment, sub-ms wait), `streams.{h,cpp}` (`sr`, time-map
pair, recv time map + render ts), `helper.{h,cpp}` (`run_helper` overload, `--video null:`/`--no-video`, all §3
commands/events, dev keys), `sources.txt`, `build-tests.{sh,cmd}`, `tests/null_main.cpp`, `tests/golden_control.txt`,
`tests/engine_tests.cpp` (see contract issue 1).

## Acceptance (CW2)

Builds: Mac in a private `git archive` export (`native/audio/engine/build-tests.sh [--asan|--tsan]`), PC in
`C:\Users\Alex\camera-cw2\` (`native\audio\engine\build-tests.cmd`, removed afterwards). Harness:
`python3 native/audio/tests/two_helpers_video.py --bin <beamer-audio-null> --seconds 120 [--case ...]`.

| Criterion | Result |
|---|---|
| Engine tests Mac plain | **51 cases, 2536 checks, 0 failures** (31 audio + 20 video) |
| Mac ASan/UBSan | full suite: 51 cases, 0 failures, no ASan/UBSan report (on `8444f79`); final code: video + golden + flow cases 24/24, clean |
| Mac TSan incl. PLI/NACK storm | full suite 51/51, no TSan report (`halt_on_error=1`, on `8444f79`); final code: video + golden + flow 23/23, clean. `video_storm` (20 % loss, decoder error every 50 ms, `video_drop_ms` bursts): 0 seal-thread violations (receiver-direction seals on one thread, media seals on the sender thread), 0 incorrect kept pictures |
| PC (plain) | final code: 31 audio cases 0 failures (20 + 12 in two runs; one video case is in the first filter), 20 video cases 0 failures |
| Existing audio tests, golden lines, `two_helpers.py` unchanged | audio 31/31 on both machines; golden session passes with both helpers on the null video backend (only additions, contract issue 2). `two_helpers.py --seconds 30` on the Mac, CW2 engine vs the pre-CW2 engine (`b7c5852`) back to back: JB p99 - target 6.5/6.5/10.6/10.3 vs 6.5/6.5/10.8/10.3 ms, dec_err 0 vs 0, fallback 2.0 s / return 1.0 s both, CPU 7.6 % vs 7.7 % per helper: identical. Both fail the script's own CPU mark (< 3 %) on this Mac tonight (load ~2), so that mark is environmental, not a CW2 regression |
| Audio loss identical with `sr:true` vs false (lossless) | harness `sr`: loss_pct sum 0.0 and 0.0, late/plc 0/0 both (Mac, PC) |
| 720p30 2.5 Mbit/s, 120 s, mic on the same flow: 0 frames missed, first frame <= 300 ms, 1 IDR | PC 120 s: first frame **107.6 ms**, 1 IDR, lost 0, skipped 0, missed 1 (= in flight at the end), frozen 0 |
| `cam_start_ms=800`: 1 IDR, first frame <= 1.1 s | PC 120 s: **898 ms**, 1 IDR (engine test: start PLIs every 100 ms, 8 coalesced, 1 start, 0 extra IDRs) |
| 1080p 5 Mbit/s: 0 size PLIs, 1 IDR, 0 missed | PC 120 s: size_rejected 0, 1 IDR, 1920x1080 presented, lost 0, first frame 116 ms |
| Receiver never started -> `no_receiver` at 3 s | 3.015 s (PC), 3.011 s (Mac); engine test 3.0 s +-50 ms incl. handle closed; no feedback for 5 s -> failed within [4.9, 5.1] s; a 1.5 s gap -> nothing |
| 1 % iid: frozen <= 200 ms/min, 0 IDRs after start | PC 120 s: frozen 0, 1 IDR (NACK repaired all; refreshes 0) |
| 0.5 % Gilbert (mean 6): frozen <= 400 ms/min | PC 120 s: 82.5 ms/min, 1 IDR, 2 refreshes |
| 1.5 s drop: clean again <= RTT + 2 frames, by refresh | PC: 46 / 43 / 40 ms (limit 72.6), 1 refresh each, no IDR; Mac: 52 / 51 / 67 ms (limit 71.7) |
| `video_drop_au`, forced queue overflow -> refresh | 5 refreshes, 1 IDR; backpressure stall: q_drop 0; hard cap: 11 dropped after numbering -> holes -> refresh, back live |
| 0 incorrect kept pictures, every run | 0 in every harness case and engine test (`incorrect_kept` = not-correct kept pictures never revoked, contract issue 9) |
| 10 ms window <= 4x bitrate x 10 ms + 32 packets; IDR within +-5 % | engine test with 0.35 ms injected overshoot per wake: Mac 95.7 / 103.0 ms vs 95.0 / 102.2 ms schedule; PC 96.6 / 104.0 ms; max 10 ms window 18.0 / 36.0 KB (limit 50.9 / 63.4 KB) |
| Audio late+plc within +1/min of the run without video | PC 120 s: 0.0 vs 0.0 per min |
| Link state: video <= 1.1 Mbit/s, frame-tagged `link_out`, feedback ordinary, refreshes answer PLIs; opened in link state -> provisional offset, plays within D | PC 120 s: 513 kbit/s on the link, 0 malformed video events, 8386 feedback events, 1 IDR, refreshes for each dropped AU, D 50 ms, provisional offset 17 ms (true 0), first frame 116 ms |
| Engine video CPU < 1 % of a core per helper (null encode/decode excluded) | measured as (mic + video) - (mic only) CPU of the same helper, **null codec included** (not separable here): PC 120 s +0.46 % (A) / +0.83 % (B); PC 30 s +0.31 / +0.93; Mac 60 s +0.81 / **+1.04** (marginal miss on the Mac receiver, includes the null decoder thread) |
| Tick latency (wake) / timers | posted PLI on the wire p95 0.15 ms (Mac), 0.09 ms (PC); 50 ms RR timer lateness mean 0.35 / 0.37 ms, p95 0.77 / 0.75 ms |
| Real helpers | the real Mac helper (CW3 backend + this engine, `build-mac.sh --no-driver` in an export) builds universal and its `ready` announces `video1`, `camera_capture`, `camera_synth`. CW4's `main.cpp` calls the new `run_helper` overload (`b8a526c`); CW2 did not build the Windows helper |

Other §10.2 items, all in `video_tests.cpp` and passing on both machines: header/NACK/PLI/time-map/RR round trips
and limits; `ReplayWindowN<1024>`; AUs 1 B..300 KB reassembled in any order; sender recovery (refresh, duplicate,
3 failed refreshes -> IDR, 2 s class-2 limit, IDR clears verified LTRs, IDR from a refresh counts, class-1 window
drop not served later); verified LTRs from clean runs only; playout target; offset estimator and the provisional
seed +-3 h both directions (seed within 20 ms, present times within 25 ms of completion + D); **30 min at +-25 ppm:
present-time error < 2 ms, D unchanged**; bootstrap (presented on decode, no sample, 70 ms until 20 samples,
unseen-frame present time); lost vs late; presenter (late shown on decode, never backwards, burst -> newest, never
keep=false, IDR at once, re-convergence, `starting` until the first IDR, hold -> 2 s -> `reconnecting` -> live,
decoder error revokes); decoder re-creation -> PLI start; time map only for covered streams, first entry twice
~20 ms apart; size rejected 3 times -> `failed decoder` + `error` placeholder, never a 4th.

## Contract issues

1. **`tests/engine_tests.cpp` hunk (not in §2.2).** "built into engine_tests" needs it: shared check counters, the
   video case list, `InProc` with an optional video backend (golden session). Also MSVC aliases for `popen`/
   `pclose`/`strdup`: the howl clip reader (in `b7c5852`, before CW2) did not compile with MSVC, so the existing
   suite could not be built on the PC at all.
2. **Golden session**: both helpers run the null video backend; A's `ready.features` gains `video1`,
   `camera_capture`, `virtual_camera`, `camera_synth` and `permission` gains `camera`. New lines for every §3
   command. Without a video backend (`--no-video`, old `run_helper`) every event is exactly as before.
3. **`camera_synth`** is added by the engine (with `--dev`, when the backend has `camera_capture`): the backend
   does not know `--dev`.
4. **IDRs are exempt from the backlog rule** (§4.5.4 "backlog older than D + hold + 150 ms is decoded without being
   kept" vs "an IDR is shown at once"): a first IDR paced over the link (~240 ms) would otherwise never be shown.
5. **Provisional offset**: §4.5.3 takes it from the stream's first completed frame, which is always the IDR, whose
   delay is the deliberate pacing (measured 83 ms off on the link). It is taken from the first frame that is
   neither an IDR nor behind one (the frames already excluded from the p99), refined over the first 30 such frames
   by keeping the least delayed (largest) estimate, like the min-RTT pick. Measured: 16-17 ms off.
6. **NACK "<= 3 tries"** = 3 transmissions in total (immediate + 2 retries), then give up = loss evidence.
7. **Decoder reopen**: `VideoSinkHandle` has no reopen call, so the engine applies the 3-errors-in-10-s rule itself
   (revoke, `started` reset, PLI `start`) and expects the sink to reopen on the same rule. CW4 note (1): on
   `decoder_hw`/`decoder_sw` the engine also asks for an IDR, **but only if AUs were handed since the last
   (re)start**: otherwise the coming IDR reaches the new decoder anyway, and asking would make a second IDR at every
   start. Requests are coalesced with a pending start/size request (its IDR may already be arriving).
8. **`size_rejected`**: the sink need not call `on_decoded` for that frame; the engine revokes everything handed and
   waits for an IDR. `device_lost` does the same.
9. **Null decoder oracle**: decoding is asynchronous, so frames handed with keep=true before an earlier frame's
   decoder error is known are revoked afterwards (§4.5.2); the engine now `drop()`s them even when their result
   arrives after the revocation. `incorrect_kept` counts not-correct kept pictures that were never revoked
   (including any presented); revoked ones are reported apart (`kept_wrong_revoked`).
10. **Drops after numbering** (hard cap, `video_link dropped`, `video_drop_au`): "`request_ltr_refresh()` at once"
    goes through the class-2 policy (refresh when a verified LTR exists, else an IDR under the 2 s limit),
    because a refresh with nothing acknowledged is an unlimited IDR (§4.3, last paragraph). Class-2 PLIs are also
    ignored while an IDR is pending.
11. **Backpressure release rate** = `idr_pace_x` x current bitrate (so a normal start IDR, 100-107 ms, does not pause).
12. **`vcam_kill_beat`** (CW4 note 3): no backend hook exists; the engine's video thread stops calling the sink.
13. **Heartbeat**: the video thread calls exactly one of `present`/`hold`/`show_placeholder` every 33 ms display tick,
    including `hold()` while a placeholder is shown. `frozen_ms` = display-tick time while > 1.5 frame periods
    passed without a new picture (and while `reconnecting`); `held_ms` the same while live.
14. **Audio time-map pair**: capture time of a frame = now - buffered samples at encode (no device latency);
    `RecvStream` render ts = newest ts - JB level, `render_latency_ms` 0 until set. CW10 refines both.
15. **Sub-ms timers**: `poll()` takes whole ms, so `NetLoop::run` waits the sub-ms rest with `select()` (in
    `flow.cpp`; `net.cpp` is not CW2's). Pacer, sender and video threads call `enable_fine_timers()` themselves
    (on Windows without it the pacer ran 30 % slow in bursts).
16. **Extra dev keys / fields** (tests only): `video_stall_ms`, `video_stall_nopause_ms`, `null_video {...}`
    (report, perm, vcam_in_use, want, install, decoder_errors, dec_err_ms, mark, reset), `send.dev_no_pli_ms` /
    `dev_no_feedback_ms`. `stream_state failed` may carry `detail` (`size_rejected WxH`). Extra stats fields:
    send `pli_dup`, `pli_window_drop`, `rr_rx`, `dev_drop_au`; recv `presented`, `d_samples`, `broken`,
    `clean_since`, `acked`, `shown`, `rr_tx`, `video_present_delay_ms`.
17. Pre-existing, not changed: the audio tests' `open_pair` and golden session bind 24823 (the answerer's default
    port, the next free one when Beamer holds it). CW2's tests and harness use 24833+.

## Notes

**CW3 (Mac backend).** Every `CameraHandle` call (`start_encoding`, `request_idr`, `request_ltr_refresh`,
`acknowledge_ltr`, `set_*`) comes from the stream's sender thread; the handle is destroyed on the control thread
(`stop`) or the sender thread (`no_receiver`). `EncoderSink` callbacks may arrive before `open_camera` returns.
`acknowledge_ltr` may name a frame older than the newest IDR: ignore unknown tokens. Event details parsed: `fps`
"30.0", `effects` "a,b", `by_other` "1"/"true", `encoder` "standby:<ms>", `camera_ms` "<ms>". `camera_lost` ->
stream ends `no_camera`. Backend errors "<code>: msg" (`no_camera`, `permission`, ...).

**CW4 (Windows sink).** `decode`/`present`/`hold`/`drop`/`show_placeholder` come only from the engine's video
thread, at least one per 33 ms tick: beat there. `on_decoded`/`on_sink_event` may come from any thread (CW4
note 2): they only post to a mailbox, nothing is sealed there. `drop()` can name a picture already decoded
(revoked late). On `size_rejected`, `on_decoded` for that frame is optional. `decoder_hw`/`decoder_sw` = "a new
decoder is ready".

**CW10 (lip sync).** Hook: `LipSyncHold` in `sync.h` (only a dev override now) is added to every present time;
data: `RecvStream::time_map_pair`, `render_ts`, `render_latency_ms`, video stats `video_present_delay_ms`,
`present_delay_ms()`. `dev {"log_time_map": true}` events: `{"ev":"time_map","flow","dir":"tx"|"rx","t_ns",
"entries":[{"stream","ts","sender_ns"}]}`.

**CW1/CW6.** `send` video: `source {"camera": key|null}` or `{"test":"synth..."}` (`--dev`), `codec`, `link
{bitrate,fps}`; audio `send` takes `sr`; `stream_state starting` at `send`, `running` at the first sent AU; the
`camera` event comes within 50 ms of a change (helper tick) and `stopped` just before the stream's
`stream_state stopped`.
