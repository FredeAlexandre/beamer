# CW1 report: Python camera control plane

Branch `feat/camera`, Mac agent, 2026-11-11. Contract: `dev/camera/PHASE2.md` §12 CW1 (and §3, §4.7, §4.10,
§5, §6, §7, §8.4, §9.1, §10.1).

## Milestones (commits)

| What | Commit |
|---|---|
| Milestone 1: `core/camera/model.py` exactly as §7.2 (+ `__init__`, `test_camera_model`) | `b9116dd` |
| Milestone 2: `core/camera/texts.py` + `test_camera_texts`; i18n `SOURCES` += `core/camera` | `bf21597` |
| wire, planner, store + tests | `6887b20` |
| part, fake video helper, `fake_helper.py` and harness hunks, part and cluster tests | `5b9f98f` |
| cluster sequences also refuse by install state / recv error | `fc73df0` |
| harness: dev command and part lookup independent of CW6's names; this report | (last commit, see `git log`) |

## What I built

- `core/camera/model.py`: the §7.2 types verbatim, plus pure helpers: `merge_route` (per entry, commutative,
  idempotent, unknown top-level entries kept and merged deterministically), `accept_devices`, `sanitize`,
  `resolve_camera`, `camera_by_key`, `size_for`, `newer`/`next_stamp`, data round trips, `empty_route`,
  `empty_snapshot`, value tables (`STATES`, `PLACEHOLDER_STATES`, `SIZES`, ...).
- `core/camera/texts.py`: `placeholder_texts(snap, name)` for the 8 states with the §8.4 wording; `reason_text`
  and the `REASONS` table for the `error` detail line.
- `core/camera/wire.py`: builders/validators for `camera_devices`, `camera_route`, `camera_open`, `camera_opened`,
  `camera_close`; §5.1 limits (16 cameras, 16 native formats, key 200, name 128, app 128, < 16 KiB); gating
  helpers `may_send(caps, hello_features)`, `announce(flag, helper_features)`, `FEATURE = "camera1"`;
  `codec(w, h, bitrate)`; `test_source_ok`.
- `core/camera/planner.py`: `plan_camera(...)` per §6.4 rules 1-8, `VideoSendSpec`, `CameraPlan`, plus
  `source_problem` and `overall` (used by the part to refine statuses with its own stream states).
- `core/camera/store.py`: `camera.json` (route, own_rev, sharing, machines), atomic, malformed = empty.
- `core/camera/part.py`: `CameraPart` + `MediaContext` (§7.1): own list composition and republish, flooding and
  greeting of `camera1` peers, demand hold 3 s, plan diff (`send` + `camera_open` in the same iteration,
  `video_config`, close with reasons), §6.5 (refusal -> helper `stop` at once, retry only on a new token, `no_receiver`
  -> `camera_close {no_receiver}` + backoff 10/30/60 s), sink side (accept only from the route's S, one recv,
  install/helper/flow refusals, `not_source` republish), placeholder push (`vcam placeholders/idle`), `video_warm`,
  `link_out` drop/pause rule (§4.10), permission only on local Mac actions, dev keys and `dev_status`.
- `core/camera/fake_video.py`: the fake helper's video side (cameras, permission, prompt, vcam, video send/recv,
  `video_config/warm/link`, `dev`), engine guards (3 s no PLI, 5 s no feedback -> `failed no_receiver`) on an
  injected clock, `VideoNet` (PLIs/feedback between fake helpers, UDP cut).
- Hunks: `core/audio/fake_helper.py` (`video=True` opt-in: video and unknown commands go to `FakeVideo`; ready
  features/events; flow close, stop and crash clean video streams); `core/tests/audio_cluster_harness.py`
  (camera nodes, `VideoNet`, `cut_udp`, `advance`, `check_camera` invariant checker, `camera_violations`, and a
  test-only stand-in for CW6's service hunk, see below); `tools/i18n_extract.py` and
  `core/tests/test_i18n_coverage.py` (`SOURCES` += `"core/camera"`).
- Tests: `test_camera_{wire,model,planner,part,cluster,texts}.py`: 17 + 16 + 17 + 30 + 13 + 7 = **100 tests**.

## Acceptance (CW1)

Commands: Mac `mac_app/.venv/bin/python -m unittest discover -s core/tests -p "test_camera*" -t .`; PC the same
with `C:\Users\Alex\Developer\beamer\win_app\.venv\Scripts\python.exe` in a private export of `HEAD`
(`C:\Users\Alex\camera-cw1\`, `git archive HEAD`, never the PC clone).

| Criterion | Result |
|---|---|
| All `test_camera_*` of §10.1 pass on the Mac and the PC | **pass**: Mac 131 tests OK (my 100 + CW7's 31 `test_camera_view`); PC 131 OK (Windows 11, Python 3.13.11) |
| Cluster convergence < 50 rounds for 200 seeded sequences | **pass**: worst 6 rounds, median 3 (Mac source, PC sink, Linux without `audio`, fork PC without `camera1`; edits, link toggles, demand, sharing, permission, helper crashes, UDP cuts, `camera_refuse`, install/recv refusals, time jumps) |
| No `camera_*` to a peer lacking `audio` or `camera1` | **pass**: `violations` and `camera_violations` empty in every sequence and scenario; the fork machine received 0 `camera_*` |
| A Mac with sharing off never sends | **pass**: checked after every step (checker) and `test_sharing_off_wins_whatever_the_others_set` (PC sets the route and opens the camera: 0 `send`) |
| Camera-on invariant checker never fires | **pass**: 0 failures over 200 sequences (+ a 5 s tail per sequence) and every scenario. Checker validated by injecting bugs: a demand hold that never ends -> fired at seed 60 ("camera on 5.60 s without a receiver") |
| On-demand open in the same worker iteration as the `vcam in_use` flood arrives | **pass**: `test_send_and_open_in_the_same_iteration_as_the_demand` (one `link_message` + `apply`: `send` + `camera_open`), cluster: the round that delivers the PC's list produces the Mac's `send` |
| Close at hold 3.0 s ± 0.1 s (simulated clock) | **pass**: 3.03 s in the cluster (10 ms steps), 3.0 s in the part test |
| Refusal -> helper `stop` in the same worker iteration | **pass**: every `camera_opened` error code (`not_installed`, `outdated`, `helper`, `unsupported`, `not_source`); no re-open in 120 s without a token change, one re-open after T's new rev |
| `no_receiver` backoff | **pass**: re-opens at 10.0, 43.0, 106.0 s (±0.35 s at 0.1 s ticks); silent receiver over 120 s: on 3.0-3.1 s per attempt at t = 0.2, 13.2, 46.3, 109.5 s; no attempt while the demand is gone |
| `permission_request` count = number of local Mac actions | **pass**: 0 for a PC-side selection + PC demand + merged route; +1 per `set_camera(own)` (permission undetermined) and per `ask_permission()`; cluster: 3 requests for 3 Mac actions |
| With `test_source` the planner sends without any camera permission | **pass**: planner table (undetermined, denied, no camera) and part test (fake helper `--dev`, permission undetermined, no cameras: `send` with `{"test": "synth;start_ms=800"}`, 0 prompts) |
| `vcam placeholders` sent with the page closed | **pass**: no UI object exists in `test_camera_part`; sent at start, on every change of texts or idle, after each helper ready, never twice for the same content |
| `test_i18n_coverage` passes apart from the stale-catalogue rule | **partial, not CW1's**: `test_catalogues_are_current_and_complete` fails (stale `beamer.pot`/`it.po`, allowed); `test_no_hard_coded_user_visible_strings` fails on `win_app/vcam_install.py:91: log('%s exited %s (%s)')`, committed by CW9 in `d6859c5` (a logger named `log` the checker does not recognise). Nothing in `core/camera` is flagged |
| The part never calls `post` off the worker | **pass**: the fake `MediaContext` asserts the thread on every `post`/`helper_send`; the cluster test sets `Host.require_thread` on every node and calls a setter from another thread |

Other suites: `test_audio*` unchanged (Mac 159 OK; PC OK, 2 skipped as before), `test_audio_cluster` OK with the
harness hunk. Full `core/tests` on the Mac: see the last section.

## Contract issues (choices made, keep or overrule)

1. **`accept_devices` and `sharing`** (§7.2 "a list's `sharing` is kept only when it comes from its own machine's
   list"): a list always describes its author, so I implemented it as (a) `model.sanitize` clears `sharing` and
   `test_source` on a list without `camera_capture` (a PC cannot carry one) and defaults a capture machine's
   missing `sharing` to on; (b) the part never takes a list describing itself (its switch is local state; a newer
   forged rev only makes it republish above it). A relayed newer list of the Mac is accepted with its `sharing`
   (it is the Mac's own value). `model.py` gained helpers beyond the §7.2 text (`sanitize`, `camera_by_key`,
   constants); the §7.2 types are unchanged.
2. **`CameraPart.shutdown()`** is not in §7.1: Beamer quitting needs `camera_close {shutdown}` and an immediate
   save. CW6 should call it from the service's `_shutdown`.
3. **Helper event routing**: `helper_event` also handles `_state` (helper gone: closes video streams, posts
   `camera_close {helper}`) and `ready` (resends warm/placeholders), returning False so audio still handles them;
   it consumes video `link_out` (`"video": true`) itself, finding the peer by flow, so CW6 may pass every helper
   event to `helper_event` first instead of routing `link_out` to `link_out(peer, event)` (both work).
   `stats` returns False: CW6 should drop `part.stream_ids()` from audio's `_stats` (else `audio_stats` to
   watching peers lists video streams).
4. **Dev `set_camera` never prompts** (and `set_camera(own)` with the synthetic source in force never prompts): the
   E2E harness sets the route by dev command with permission `not_determined`, and a prompt on the Mac's screen
   would block nothing but is a person-only artefact (§10.5 "runs without Alex granting anything").
5. **Planner states the contract does not name**: route names a machine without `camera_capture` -> `no_camera`
   reason `unsupported`; S's helper starting -> `waiting_machine` reason `starting`; S's helper failed/no video ->
   `error` reason `helper`. Overall status precedence: S's problem, then `streaming`, `starting`, `error`,
   `unreachable`/`no_direct_path`, `idle`, install problems, `no_virtual_camera`. A sink with demand shows
   `starting` until S's capture is `running` (remote view) or until its own stream runs (S's and T's own views).
   `CameraStatus.params`: `machine` (S), `to` (space-separated T ids), `device` (name, `waiting_device`).
6. **Retries for other failures** (T's `camera_close {error}` after a decoder failure, a `send` error `permission`
   or `no_camera`): same rule as refusals (new token only, no timer); `no_receiver` alone has the backoff. A
   running stream resets the backoff step; a demand that ends resets it too.
7. **`MediaContext.schedule`**: the part calls it only when its earliest wake changes; the service must keep the
   earliest and call `apply` at that time (the harness stand-in does `min`).
8. **Placeholder wording with the synthetic source**: "Starting Test source" / "on <S>" (the camera name is in
   the `starting` title). CW7 may want "Starting the test source".
9. **`VideoStreamView.delay_ms`**: recv side = stats `target_ms` (D), send side = stats `rtt_ms`; `frozen_ms` recv
   only. `camera_open.camera` = the route's device, else the resolved camera when the route names one, else null.
10. **Fake helper video is opt-in** (`FakeHelper(video=True|{...})`), so the audio tests' feature lists and
    events are unchanged.

## Needs Alex

Nothing.

## Follow-ups for CW6 and CW7

- **CW6**: the harness's `camera_service()` is a test-only stand-in for your service hunk (it subclasses
  `AudioService`, implements `MediaContext` and the routing of §2.2). As soon as `AudioService` has
  `camera_snapshot`, the harness uses the real class unchanged; for that, the tests need: the part reachable as
  `service.camera` or `service._camera`; the public setters `set_camera`, `set_quality`, `set_camera_sharing`,
  `ask_camera_permission`, `refresh_camera_install`; dev items through `_dev_command(item)` (or a `camera_dev`
  method); `FakeHelper(video=...)` from the harness's `helper_factory`; `camera1` in `audio_hello.features` when
  the flag is on and the helper has `video1`. The stand-in's methods are a working reference for `MediaContext`
  (`flow_for` = agreed flow only, `peers_with` from `hello.features`, `online` = audio's online set,
  `machine_name` order as `_Names`). Also: `shutdown()`, filter video ids from audio `_stats` (issue 3),
  `_new_stream` excluding `part.stream_ids()`, `dev_status()` into `audio-dev-status.json` under `camera`.
- **CW7**: wording review of `texts.py` (issue 8, and the `REASONS` lines); status params and precedence in
  issue 5; `snapshot.sinks[own]` is the PC's own sink state, `status` the route's line.

## Full `core/tests` on the Mac

`mac_app/.venv/bin/python -m unittest discover -s core/tests -t . -v` (working tree at `fc73df0` + the harness
change of the last commit): **Ran 2147 tests in 563 s, FAILED (failures=2, errors=4, skipped=3)**, none from CW1:
- errors (4): `test_profiles_migration` (2) and `test_tile_resize` (2, revisions `be9661d1`, `2dbe44ab`): `git
  archive` of commits that exist only in the maintainer's private history (the known environmental failures of
  `dev/README.md`, which also apply on the Mac);
- failures (2): `test_i18n_coverage`: the stale catalogue (allowed rule) and CW9's `win_app/vcam_install.py:91`
  (contract issue list above, acceptance table).

Every `test_camera_*`, `test_audio_*` (incl. `test_audio_cluster` with the harness hunk) and every other test
passes.
