# WP1 report: Python control plane (`audio/wp1`)

**Done.** `core/audio/{__init__,model,wire,planner,store,helper,service,fake_helper}.py`, `core/tests/test_audio_{model,wire,planner,service,cluster,helper,helper_native}.py`, `core/tests/audio_cluster_harness.py`. Milestone 1 (`model.py` as in §8.3, with its test) went in first: 4e8efa2.
The service covers flows (§4.2: offerer/answerer, want, close), streams (open/opened/close, retry when the receiver's list, the route or the flow changes), the §4.5 fallback (`link_out` becomes `audio_frame`, dropped while `outbox_depth` > 64; `audio_frame` becomes `link_in`), the §5.3 flood (device lists, route merge with answer), §5.4 helper restart, holds, the local loop, watch/stats/levels (≤ 20 Hz), `audio.json` (saved 500 ms after a change) and the dev command files.

**Acceptance (measured).**
- All `test_audio_*` on the Mac: 81 tests OK, 2 skipped (native). On the PC (`C:\Users\Alex\audio-wp1`, win_app venv Python 3.13.11): 81 OK, 2 skipped. PASS.
- Cluster convergence: 3 machines, one without `audio`, 200 seeded edit/partition/crash/unplug sequences. Worst case 5 message rounds after healing (limit 50). Also 60 sequences with 3 audio machines, and 3000 extra seeds offline: 0 failures, max 5 rounds. After convergence, what each service shows running equals what its helper runs. PASS.
- No `audio_*` sent to a peer without the cap: the harness records every post. 0 in all runs, and the 1.5.x node receives nothing. PASS.
- Service never blocks a link thread: a threaded service whose host `post` blocks and records the calling thread. 4 threads × 200 `link_up`/`link_message`/`snapshot` calls, slowest call < 50 ms while the worker is stalled, every post on `Beamer-audio`. PASS.
- `test_audio_helper_native`: NOT RUN. WP2's `beamer-audio-null` is not built yet (`~/Developer/beamer-wp2` has engine sources, no `native/audio/build/`). The test skips cleanly. Run it with `BEAMER_AUDIO_NULL=<path>` (and `BEAMER_AUDIO_NULL_ARGS`, if needed). It checks ready, devices, error codes and exit 0 on EOF, then two helpers streaming mic over loopback (`flow_state up`, `stream_state running`, `stats` with path udp).
- Full `core/tests` on the Mac: 1938 tests, 4 errors. All 4 are the known `git archive` errors in `test_profiles_migration`/`test_tile_resize` (dev/README). No regression.

**Deviations / interpretations (WP5, WP6 and WP7 should read these).**
1. `AudioSnapshot` is `kw_only=True`: §8.3 wants Mapping fields to default to empty, and those fields come before fields without a default. Field names, order and types follow §8.3 exactly. `model.py` adds helpers: `empty_route/empty_snapshot`, `newer`, `next_stamp`, `output_of`, `form_of`, `trim_forms`, `route_data/route_from`, `machine_data/machine_from`.
2. `RouteStatus.params` keys: `to` (space-separated machine ids for the mic, one id for an output), `machine`, `device` (a name). Output without `system_capture` gives `state="error", reason="no_system_capture"`. A refused or failed own stream gives `state="error"`, with `reason` = the error code.
3. `StreamView.state`: `opening|starting|running|failed`. `path`: `udp|link|local|""`. The local loop appears as a mic stream with source = target = own.
4. Service API: `link_up/link_down` are per peer (up when the first link is live, repeat calls refresh caps; down when no link is left). `link_message(peer, {"type","data"})`; `post` gets the same shape. `start(run_thread=False)` and `pump()` drive the worker without a thread (tests, harness).
5. `helper_cmd` is the base argv (`helper.locate(state_dir)`); the service adds `--control ...` (plus `--dev` when `audio-dev.enabled` exists). A `helper` entry in `audio-dev.enabled` overrides `helper_cmd` even if the adapter passed another one.
6. Helper lifecycle reaches the service as `{"ev":"_state","state":"starting"|"failed"}`; `ready` means ok. `helper.available()` reads `feature_flags.AUDIO` with `getattr(..., False)` until WP5 adds the flag.
7. Equal route stamps with different content (should never happen) are broken by `repr`, so the merge stays a true semilattice.
8. An `audio_open` on a flow that is not the current one is answered `opened {ok:false, error:"helper"}`. The sender retries when the flow changes.

**Needs from other WPs.**
- WP2 / PHASE2 §4.2 (risk): the Mac offers with `remote: null`, and WP2's `Flow::open_socket` then binds `INADDR_ANY`. On the Mac that is an unsigned listener on a non-loopback address (firewall dialog). Suggestion: add an optional `host` hint to `flow_open` (the service knows `peer_host` at offer time), or have the Mac helper bind towards the peer. The native test points the offer at 127.0.0.1 from the start to avoid this.
- WP5: `feature_flags.AUDIO`; call `link_up` once per peer (see 4); pass `helper.locate(state_dir)` as `helper_cmd`.

**Commits:** 4e8efa2 model (milestone 1) · 5ccd888 wire/planner/store/helper/service/fake helper/harness + tests · b958178 helper tests · e8770c5 exports · b5a82ac stream-consistency check · (this report).
