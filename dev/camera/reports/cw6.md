# CW6 report: link and service integration

Branch `feat/camera`, Mac agent (PC verified over ssh), 2026-11-11. Contract: `dev/camera/PHASE2.md` §12 CW6
(and §2.2, §4.9 `sr`, §5, §7.1, §7.4), CW1's contract issues (`reports/cw1.md`), CW7's "Notes for CW6".

## Files

| File | What |
|---|---|
| `core/audio/service.py` | the §2.2 hunk: `_CameraContext` (the `MediaContext`, incl. `machine_name`), a `CameraPart` built when `feature_flags.CAMERA` is on (`service._camera`), routing of helper events, `camera_*` messages, link up/down, helper gone/restart; `part.apply()` after every audio settle; the part's wake in `_next_deadline`/`_tick`; one subscriber call per settle for audio or camera changes; public API `camera_snapshot()`, `set_camera`, `set_quality`, `set_camera_sharing`, `ask_camera_permission`, `refresh_camera_install`; `_new_stream` excludes `part.stream_ids()`; video stats out of audio `_stats`; audio `send` `sr:true` towards `video1` peers; `camera1` in `audio_hello`; dev command items to `part.dev_command`, `camera` in the dev status file; `part.shutdown()` on stop; log lines `camera: <peer> speaks camera1` and `camera: camera_devices to/from …`; video `link_out` packets replaced by their count in `audio-dev-events.jsonl` |
| `core/link.py` | `_CAMERA_CAP_FOR` (every `camera_*` type -> `audio`) and `_cap_for()`, used by `OutboundLink.post` (contract issue 1) |
| `core/receiver.py` | `_is_audio` covers `camera_*`: responder `post` drops them without `audio`, `_dispatch` hands them to the audio callback only when both ends have `audio` |
| `mac_app/bridge.py` | outbound-link messages `camera_*` forwarded like `audio_*` |
| `win_app/sender.py` | outbound-link messages `camera_*` forwarded like `audio_*` |
| `win_app/kvm_bridge_win.py` | **no change needed**: it only wires the callbacks of `receiver` and `sender`, which do the type dispatch |
| `core/feature_flags.py` | `CAMERA = True` |
| `mac_app/setup.py` | `includes` += `core.camera`, `.model`, `.part`, `.planner`, `.store`, `.texts`, `.view`, `.wire` (only this hunk committed; CW7's `camera_section` line left to CW7) |
| `WIRE.md` | section 12 "Camera (inside capability `audio`, fork)" = §5 (+ the media-flow summary); the `audio` row of the capability table mentions `camera_*` |
| `core/tests/test_camera_link_gating.py` | new, 26 tests (below) |

## Acceptance (CW6)

Mac commands run from a private export (`git archive HEAD` + my files), venv `mac_app/.venv`; PC from a plain folder
`C:\Users\Alex\camera-cw6\` with `C:\Users\Alex\Developer\beamer\win_app\.venv\Scripts\python.exe`.

| Criterion | Result |
|---|---|
| Gating: initiator and responder links drop `camera_*` without `audio` | **pass**: `test_camera_link_gating` `OutboundGatingTests` (the 5 types posted to an `OutboundLink` without `audio`: 0 queued; with it: 5 queued), `ResponderLinkTests` (responder `post` drops them; `_dispatch` hands the 5 types over only for (audio, audio), never for (no audio, audio) or (audio, no audio)) |
| Both apps forward `camera_*` like `audio_*` | **pass**: `PcSenderTests` (`win_app/sender.py` `on_message`: 5 types to the audio callback with `audio`, 0 without), `MacBridgeTests` (`mac_app/bridge.py` `KVMController._link_message` in a subprocess: the 5 types + `audio_hello` with `audio`, nothing without; skipped on the PC: AppKit) |
| The part sends nothing to a peer without `camera1` | **pass**: `test_nothing_camera_to_a_peer_without_camera1_or_audio` (fork PC with a no-video helper, Linux peer without `audio`: 0 `camera_*` received, `camera_violations` and `violations` empty); `test_camera_cluster` now runs on the real `AudioService` (13 tests, 200 seeded sequences, invariant checker silent, 0 `camera_*` to the fork machine); live links below: nothing to the 1.5.x peer |
| `camera1` only with the flag and a `video1` helper | **pass**: `test_camera1_only_with_a_video_helper`; `test_flag_off_means_no_part_no_camera1_and_camera_ignored` (flag patched off: no part, `camera_snapshot()` None, no `camera1`, 0 `camera_*` posted, camera messages and setters ignored) |
| `_new_stream` never returns a camera stream id | **pass**: `test_new_stream_never_returns_a_camera_stream_id` (`secrets.randbits` forced to draw the running video stream id twice: the third draw is returned; the next audio mic stream differs) |
| Audio `send` carries `sr:true` only to a `video1` peer | **pass**: `TimeMapTests` (one Mac mic to a `video1` PC and to a fork PC without it: `sr: true` / no `sr` key; none when our own helper lacks `video1`); null-helper pair: the real helper takes `{"kind":"mic","sr":true}` and the stream runs |
| Each §7.4 dev command reaches the part, `helper_dev` the helper, the status file has `camera` | **pass**: `DevCommandTests` through the real dev command file (`audio-dev.enabled`, `--dev` helper): `camera_test_source`, `set_camera`, `set_quality`, `camera_sharing`, `camera_refuse` each reach `part.dev_command` (5 calls for 5 items) with their effect (route, 1080p, `test_source` announced, the PC refuses `silent`, sharing off stops the send); `helper_dev {"video_loss":...}` -> helper `dev`; `audio-dev-status.json` has `camera` (`test_source`, `snapshot.sharing`, `refuse`) beside the unchanged audio keys; a dev command never prompts (0 `permission_request`); a video `link_out` is logged in the dev events as a packet count |
| Each public setter reaches the part on the worker (a test per setter, from another thread) | **pass**: `ThreadedSetterTests` (threaded service: `set_camera`, `set_quality`, `set_camera_sharing`, `ask_camera_permission`, `refresh_camera_install` called from another thread, each recorded on the worker thread with its arguments); 2000 calls of `camera_snapshot()` + `snapshot()` from a test thread < 50 ms (CW7's menu tick) |
| `subscribe` fires on a camera change | **pass**: `test_subscribe_fires_on_a_camera_change` (`set_quality` changes only the camera snapshot: the subscriber is called and sees 1080p; an idle pump calls nothing) |
| `machine_name`: own, held machine, fallback | **pass**: `test_machine_name`: own -> "mac", held audio list -> "lebig", no list -> the link's peer name, unknown -> "Machine · <last 6>" |
| CW1's contract issues (shutdown, wake-ups, stats, helper events) | **pass**: `test_shutdown_closes_the_camera_streams` (`camera_close {shutdown}` + helper `stop`, `camera.json` written); `test_part_wake_alone_closes_after_the_hold` (only the service's own deadlines, no message or engine tick: closed 3.0 s after the demand ended); `test_video_stats_stay_out_of_the_audio_stats`; every helper event goes to `part.helper_event` first |
| Cluster harness on the real service (CW1) | **pass**: `camera_service()` now returns `core.audio.service.AudioService` unchanged; `test_camera_cluster` 13 OK (200 seeds); all `test_camera_*` 157 OK on the Mac and the PC |
| Full `core/tests`, `mac_app/tests`, PC `win_app/tests` unchanged except the known failures | see "Full suites" |
| `dev/deploy.sh` both with null video: logs show `camera1` and `camera_devices`; caps still 14; asusrog silent; audio E2E `mic_mac_pc` unchanged | **not run: this brief forbids deploying** (operator item, Needs Alex). Caps: none added (`test_no_new_capability`, `MAX_CAPS` 16; the camera rides `audio`). Substitute below |
| **Substitute: null-helper pair** (`dev/camera/reports/cw6_null_pair.py <export>`, export of `4d99847`, `beamer-audio-null` built there with `build-tests.sh`) | **pass**: two real `AudioService`s ("Mac" macos, "PC" windows), each with a real `beamer-audio-null` (null audio + null video backend), links in memory, UDP on 127.0.0.1 only (no LAN bind: no firewall prompt), plus a peer without `audio`. Log: `camera: pc speaks camera1`, `camera: mac speaks camera1`, `camera: camera_devices to/from …` both ways; route Mac -> PC; PC demand (`null_video vcam_in_use`) -> Mac `streaming` 0.052 s later; after 3 s PC recv `fps_shown` 30.1, 2423 kbit/s, `lost` 0, D 50 ms, path udp; Mac send 30.2 fps, 1 IDR; video stats only in the part (audio `_stats` empty); Mac mic to the `video1` PC sent with `sr:true`, running; demand off -> Mac send closed 3.04 s later; 0 messages to the old peer; `camera_*` only Mac <-> PC |
| Live links (real `OutboundLink` + `LinkResponder` on loopback) | **pass**: `LiveServicesTests`: a Mac service dialling a PC service (both with the fake video helper) exchange `camera_devices` (each part holds the other's list), both hellos carry `camera1`, a route set on the Mac reaches the PC; the 1.5.x Linux peer linked to the PC receives no `audio_*`/`camera_*` and the PC posts none to it |

## Full suites

All on exports of `4d99847` (my commit, on top of CW2/CW3/CW7's latest), never the shared tree.

| Suite | Result | Failures, all known / not CW6's |
|---|---|---|
| Mac `core/tests` (`python -m unittest discover -s core/tests -t .`) | Ran 2173 in 563 s: failures 2, errors 4, skipped 5 | errors: `test_profiles_migration` (2) and `test_tile_resize` (2), the `git archive` of private revisions `be9661d1`/`2dbe44ab` (dev/README.md); failures: `test_i18n_coverage` stale catalogue (allowed rule) and `win_app/vcam_install.py:91` (CW9, already in CW1's report). Every `test_camera_*` (157), `test_audio_*` (159, 2 skipped as before) and `test_link*` (95) pass |
| Mac `mac_app/tests` (`cd mac_app; python -m unittest discover -s tests`; its discovery also runs `core/tests`) | Ran 4024 in 1111 s: failures 7, errors 5, skipped 5 | the 6 core ones above, plus `test_glow_falloff` (needs `pytest`, not in the venv), `test_frame_clock` main-screen clock, `test_click_focus` tab ring, `test_settings_layout_acceptance` page `audio` at 320 px (3 subtests): **identical on the parent commit `d9626a6`** (the same modules run on an export of `4d99847^`: the same 4 layout/click-focus failures and the same frame-clock failure), so not CW6's (the layout ones follow CW7's Camera module in the Audio page) |
| PC `core/tests` (`C:\Users\Alex\camera-cw6\`, Windows 11, Python 3.13) | Ran 2173 in 583 s: failures 3, errors 4, skipped 14 | export before the last `core/link.py` change: the 3rd failure was `test_audio_link_gating` `_CAP_FOR` (contract issue 1, fixed in the commit). On the `4d99847` export (`camera-cw6\head`): `test_camera*` 157 OK (1 skipped: the Mac bridge), `test_audio*` 159 OK (2 skipped as before), `test_link*` 95 OK |
| PC `win_app/tests` (`camera-cw6\head\win_app`, `discover -s tests`, also runs `core/tests`) | Ran 4309 in 1169 s: failures 3, errors 6, skipped 127 | exactly dev/README.md's list (`test_admin_mode`, `test_admin_mode_ui` import errors; `test_profiles_migration` 2 and `test_tile_resize` 2; AZERTY `test_a_shifted_digit_gives_the_symbol_the_layout_prints`) plus the stale catalogue and CW9's `vcam_install.py:91` |

CW9b's `dbeff7d` (after my commit) fixes `vcam_install.py:91`, so that failure is gone at today's head.
`test_i18n_coverage`: no string of CW6's is flagged (the only `tr` added, "Machine · {id}", already exists in
`core/audio/view.py`); the stale-catalogue failure is the allowed one.

## Contract issues (choices made, keep or overrule)

1. **`_CAP_FOR` hunk vs an existing audio test.** §2.2 says `_CAP_FOR += every camera_* type -> "audio"`, but
   `test_audio_link_gating.CapabilityTableTests.test_every_audio_type_needs_the_audio_capability` asserts the
   `audio` entries of `_CAP_FOR` are exactly the audio types, and the audio tests must stay green (and are not
   CW6's files). So the camera types sit in `core/link.py:_CAMERA_CAP_FOR` and `OutboundLink.post` reads both
   through `_cap_for(kind)`. Same behaviour on the wire; `send_input` is unchanged (camera messages are not input).
2. **`MediaContext.schedule` keeps the latest value, not the minimum.** CW1 asked the service to "keep the
   nearest". The part calls `schedule` only when its single next wake changes, so its latest call *is* its
   nearest wake; keeping `min(old, new)` would keep a wake the part dropped (e.g. a cancelled demand hold) and,
   once it fired, lose the later one forever (the part does not call again for an unchanged wake). The service
   keeps the part's wake beside its own and wakes at the earliest of all (`_next_deadline`), which is the "keeps the
   nearest" of the contract. Test `test_part_wake_alone_closes_after_the_hold` (only wake-ups, no event: closed
   at the 3 s hold) and `test_schedule_keeps_the_latest_wake`.
3. **`sr:true` also needs our own `video1`.** §2.2 says audio `send` gets `sr` when the peer's hello has `video1`;
   the service also requires its own helper to announce `video1` (only a video engine sends a time map, and an
   older helper is never sent the field).
4. **A machine whose helper lacks `video1` does not announce `camera1` but still publishes its `camera_devices`
   (status `no_video`) to `camera1` peers.** That is CW1's part rule (`_post` gates on the *peer's* `camera1`
   only) and what §5.2's `no_video` status needs; nothing is ever posted to a peer without `camera1`.
5. **`_state` routing.** The part learns of a helper gone through the service's `_helper_gone` (which also runs
   on a vmic-exclusive restart that has no `_state` event), not by passing `_state` to `helper_event`.
6. **Subscribers**: one call per settle when the audio snapshot or the camera snapshot changed (`changed()` only
   sets a flag), so the pages are not woken twice.
7. **`kvm_bridge_win.py`** needed no hunk (see Files).
8. **Deploy acceptance not run** (see Needs Alex / operator): this brief forbids deploying or restarting the
   installed Beamer, so `dev/deploy.sh` with the null video helper was replaced by the equivalent check below
   ("Null-helper pair"), which runs the real `AudioService` code against two real `beamer-audio-null` helpers.

## Needs Alex

Nothing from Alex. **Operator**: the deploy item of CW6's acceptance (`dev/deploy.sh` both machines with
`audio-dev.enabled` helper = `beamer-audio-null`; both Beamer logs show `camera1` and `camera_devices`; asusrog
silent; audio E2E `mic_mac_pc` unchanged) needs a deploy, which this brief forbids; run it when deploying is
allowed. The log lines to look for: `camera: <peer> speaks camera1`, `camera: camera_devices from <peer> (...)`.

## Notes for CW11 (and CW7/CW8)

- Public API on `AudioService`: `camera_snapshot()` (plain read, None when `feature_flags.CAMERA` is off),
  `set_camera(machine, CameraRef|None)`, `set_quality("720p"|"1080p")`, `set_camera_sharing(bool)`,
  `ask_camera_permission()`, `refresh_camera_install()`, `retry_helper()` (existing); `subscribe` fires for camera
  changes too. The part is `service._camera`.
- Dev files: every item of `audio-dev-cmd.json` goes to the audio keys and then to `part.dev_command(item)`
  (`set_camera`, `set_quality`, `camera_sharing`, `camera_test_source`, `camera_refuse`); `helper_dev` reaches the
  helper's `dev` (`video_loss`, `video_drop_au`, `null_video`, `playout_floor_ms`, ...). `audio-dev-status.json`
  has `camera` = `part.dev_status()` (`snapshot`, `stats` per video stream, `test_source` in force,
  `test_source_requested`, `refuse`, `link_out_dropped`, `permission_requests`, `warm`).
- `audio-dev-events.jsonl` logs every helper event (camera ones included: `cameras`, `vcam`, `camera`, video
  `stream_state`/`stats`); a video `link_out` is logged with `"p": <packet count>` instead of its packets.
- The Beamer log names the camera handshake: `camera: <peer> speaks camera1` once per peer and
  `camera: camera_devices to|from <peer> (rev, status, cameras, sharing, install, in use)` per list.
- `dev/camera/reports/cw6_null_pair.py <export root>` runs the real service with two `beamer-audio-null`
  helpers on 127.0.0.1 (build them first with `native/audio/engine/build-tests.sh`); it patches `_usable_host` to
  accept loopback for itself only, and drives demand with `helper_dev`-style `null_video {"vcam_in_use":...}`.
  A starting point for E2E cases that need no second machine.
- PC: `C:\Users\Alex\camera-cw6\` (plain folder, two exports) is mine and can be deleted.
