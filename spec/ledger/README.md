# The ledger's owner domains

#51 laid the ledger out: every Python test at `archive/python`, 6,232 of them, in exactly one
domain's slice, `spec/ledger/<domain>.txt`, with no fate yet. The format and the check are in
[`spec/README.md`](../README.md#the-python-ledger). Each domain's miner gives its slice's tests their
fates and adds or removes no line (the `build-spec-miner` skill).

The domains are the twenty "Spec: <domain>" tasks under #24. Every domain has a slice, empty where
no Python test is its own:

| Domain | Task | Tests |
|---|---|---|
| `link` | #52 | 321 |
| `pairing` | #53 | 286 |
| `store` | #54 | 0 |
| `discovery` | #55 | 116 |
| `driving` | #56 | 327 |
| `crossing` | #57 | 1116 |
| `input` | #58 | 793 |
| `clipboard` | #59 | 213 |
| `settings` | #60 | 461 |
| `display` | #61 | 31 |
| `audio` | #62 | 148 |
| `camera` | #63 | 202 |
| `media` | #64 | 0 |
| `platform` | #65 | 324 |
| `runtime` | #66 | 4 |
| `app` | #67 | 98 |
| `viewmodel` | #68 | 1051 |
| `ui` | #69 | 741 |
| `perf` | #70 | 0 |
| `observability` | #71 | 0 |

## How a test got its domain

1. **Its assertions decide.** A test goes to the domain whose behaviour it asserts, as that
   domain's Scope line on its task names it, not to the module it imports or the harness it runs
   on. Every way across pushed through desks on loopback (`core/tests/test_crossing_e2e.py`) is
   `crossing`'s, and an audio message's capability gating is `audio`'s.
2. **By file, then by class.** A file goes whole to one domain, unless its classes assert different
   domains' behaviour: then each class goes on its own. No assignment is finer than a class, so
   one class's tests are always in one slice.
3. **A mixed file or class goes by most of its tests.** When a file or class asserts several
   domains' behaviour, it goes to the domain that most of its test names are about. An end-to-end
   suite of takes, refusals and let-gos that also checks the clipboard arriving is `driving`'s.
4. **A feature through a screen is the feature's.** A test that checks a feature's behaviour
   through a window (a setting synced, a zone saved) goes to that feature's domain. A test of the
   screen itself goes to `viewmodel` when it asserts what a page, overlay, menu or notice shows,
   enables, validates or says, and to `ui` when it asserts how it is drawn: pixels, geometry,
   sizes, colours, fonts, contrast, motion.
5. **A Scope line that names tests wins.** #56 names the three-machine tests for `driving`:
   `core/tests/test_three_machines.py` and `core/tests/test_three_desks.py`.
6. **Dropped and out-of-scope tests go where their feature would be,** so one miner settles each
   feature's tests with one fate:
   - Linux-only code (#1): the domain of the same behaviour on the Mac and Windows (capture and
     injection to `input`, X11 and portal clipboards to `clipboard`, autostart and portals to
     `platform`).
   - Phone peers: the domain of the behaviour (the phone contract to `link`, phone targets to
     `driving`, phone settings to `settings`, the phone UI to `viewmodel`).
   - #11's drops: pair nearby (F3) to `pairing`; Windows unlock (F21) and the lock screen to
     `platform`; the update check and notice (F22) to `app`; the portable build (F23), its
     discovery included, to `platform`; the SSH tunnel (F42) to `link`; the 1.4.x and profiles
     migrations of the settings file to `settings`.
   - #11's Later: crossing effects (F30) and the notch island and beam (F31) to `ui`, whose Scope
     names the room left for them.
   - The v5 and v6 wire formats: the domain whose messages they encode (pairing messages to
     `pairing`, audio and camera messages to theirs, takes and let-gos to `driving`, keys and input
     to `input`, clipboard images to `clipboard`), and the rest, the frames, handshake and
     preamble, to `link`.
7. **Tests of the Python repo itself** (no duplicate modules, the release-body generator) go to
   `app`.

`store`, `media`, `perf` and `observability` own no Python test: Python had no store (each feature
flooded its own messages, and those tests went to the feature), the media engine is C++, and
`perf` and `observability` are written from #4, #14 and the other domains.

## Ambiguous assignments

An assignment is ambiguous when rule 3 or 4 had to choose: a second domain is a fair owner too.
There are 97, covering 1,204 tests. Each row names the other candidates and why the owner won.
A miner who finds such a test's assertions are all another domain's settles it as
`.claude/skills/build-spec-miner/blocked.md` says, without moving the line.

Rows marked "moved from platform (#65)" record lines that did move, after #51 (#147): the platform
miner found their assertions all another domain's, and moved them to that domain's slice with no
fate. Each splits its class, an exception to rule 2's "no assignment finer than a class", except
`LauncherArgumentHandoffTests`, which moved whole: the class's other tests stay platform's.

| Tests | Count | Owner | Also a candidate | Why the owner |
|---|---|---|---|---|
| `core/tests/test_address_choice_commit.py` | 3 | discovery | pairing | the address a peer is reached at, chosen by the user, is discovery's address list |
| `core/tests/test_audio_link_gating.py` | 23 | audio | link | capability gating of `audio_*` messages; audio's own messages, so audio |
| `core/tests/test_audio_view.py` | 36 | viewmodel | audio | the Audio page model, what each state shows and what each action calls |
| `core/tests/test_camera_link_gating.py` | 26 | camera | link | capability gating of `camera_*` messages; camera's own messages, so camera |
| `core/tests/test_camera_texts.py` | 8 | camera | viewmodel | the placeholder text is shown in the virtual camera's picture, not on a screen of Beamer's |
| `core/tests/test_camera_view.py` | 33 | viewmodel | camera | the Camera module's model, what each state shows |
| `core/tests/test_dormant_repair.py` | 19 | pairing | discovery | dormant repair is in both scopes; these tests check the peer entry, re-pairing and capacity, not address probing |
| `core/tests/test_feature_flags.py` | 1 | pairing | app | its one test checks that phone pairing is hidden by default |
| `core/tests/test_flatpak_libei_path.py` | 1 | input | platform | where Linux input capture finds libei |
| `core/tests/test_length_size_sliders.py` | 13 | ui | settings, viewmodel | the effect length and size sliders, their values, presets and sync; effects are F30 (Later) |
| `core/tests/test_link.py`, class `LinkFollowTests` | 7 | discovery | link | a link following a peer to a new address is discovery's address list |
| `core/tests/test_link.py`, class `LinkHardwareTests` | 5 | discovery | link | the hardware address carried in the handshake serves Wake-on-LAN (F20) |
| `core/tests/test_link_endpoint_follow.py` | 6 | discovery | link, pairing | discovered addresses are hints until the peer authenticates |
| `core/tests/test_motion_flow.py` | 7 | link | input | bounded unacknowledged input, acks and the outbox are the link's input class |
| `core/tests/test_pairing_paths.py`, class `BroadcastTargetTests` | 3 | discovery | pairing | where the beacon is broadcast on a machine with several networks |
| `core/tests/test_pairing_paths.py`, class `HostileNearbyDatagramTests` | 2 | discovery | pairing | hostile datagrams on the discovery socket |
| `core/tests/test_pairing_paths.py`, class `InterfaceTests` | 4 | discovery | pairing | which interfaces a machine announces on |
| `core/tests/test_pairing_paths.py`, class `OversizedDatagramTests` | 1 | discovery | pairing | an oversized datagram on the discovery socket |
| `core/tests/test_patch15_nearby.py` | 3 | pairing | viewmodel | the nearby list's refresh, for pair nearby (F3, dropped) |
| `core/tests/test_portable_discovery.py` | 5 | platform | discovery | the portable build's discovery; the portable build (F23, dropped) is kept in one domain |
| `core/tests/test_profiles_e2e.py` | 30 | crossing | settings, store | profiles synced over the links; profiles are crossing's (F7) |
| `core/tests/test_profiles_migration.py` | 12 | settings | crossing | the settings-file migration to profiles; settings owns the settings file and its migrations |
| `core/tests/test_protocol_v6.py`, class `ClipboardImages` | 10 | clipboard | link | the v6 clipboard image payload |
| `core/tests/test_protocol_v6.py`, class `Focus` | 10 | driving | link | the v6 messages that take and let go, encoded by the wire |
| `core/tests/test_protocol_v6.py`, class `Input` | 4 | input | link | the v6 input frames |
| `core/tests/test_protocol_v6.py`, class `InputMethodKeyFrames` | 1 | input | link | the v6 input-method key frames |
| `core/tests/test_protocol_v6.py`, class `Keys` | 9 | input | link | the v6 key frames |
| `core/tests/test_receiver_backlog.py` | 12 | link | input | receiver backpressure and input order over the link |
| `core/tests/test_responder_ownership.py`, class `WrapWayBack` | 2 | crossing | driving | the way back from a driven machine |
| `core/tests/test_responder_ownership.py`, class `ZoneModels` | 5 | crossing | driving | the zones a driven machine arms for its driver |
| `core/tests/test_responder_ownership.py`, class `Zones` | 8 | crossing | driving | the zones a driven machine arms for its driver |
| `core/tests/test_three_desks.py` | 15 | driving | crossing | #56's Scope names the three-machine tests for driving |
| `core/tests/test_windows_launch_manifest.py` | 1 | platform | app | the Windows manifest asks for no elevation at launch, an install step (#21) |
| `mac_app/tests/test_atlas_canvas.py` (its other classes) | 60 | viewmodel | ui, crossing | the crossing map's canvas; its tests are mostly what it shows and what a drag, drop or key does |
| `mac_app/tests/test_atlas_canvas.py`, class `WrapAroundSeams` | 10 | crossing | viewmodel | wrap-around zones as the map shows them |
| `mac_app/tests/test_audio_driver.py` | 23 | audio | platform | installing the virtual microphone; audio's scope names it |
| `mac_app/tests/test_bridge.py`, class `ControllerTests` | 37 | driving | crossing, link | the shortcut, take, refuse and send home, and alerts on a failed link |
| `mac_app/tests/test_bridge.py`, class `CursorPinningTests` | 9 | crossing | input | the pointer held at the edge while pushing |
| `mac_app/tests/test_bridge.py`, class `RoundTripTests` | 4 | viewmodel | link | when a response-time sample is shown |
| `mac_app/tests/test_config_key_map_override.py` | 1 | input | settings | a saved key map override |
| `mac_app/tests/test_corner_marks.py` | 20 | ui | viewmodel | the crossing map's corner marks; the tests are mostly their pixels and geometry |
| `mac_app/tests/test_crossing_ledger.py` | 32 | viewmodel | crossing | the ledger's controls on the Crossing page (F6); what they show and change |
| `mac_app/tests/test_dock_visibility.py` | 6 | viewmodel | platform | Show in Dock (F27), which the viewmodel's scope names |
| `mac_app/tests/test_dock_window.py` | 14 | viewmodel | platform | Show in Dock and menu bar (F27), which the viewmodel's scope names |
| `mac_app/tests/test_dormant_rows.py` | 11 | viewmodel | pairing, crossing | how a dormant peer's rows show and what re-pair opens |
| `mac_app/tests/test_idle_refresh.py` | 6 | viewmodel | runtime, ui | how often the window and menu re-read settings and redraw |
| `mac_app/tests/test_ignored_titles.py` | 16 | viewmodel | input | the names shown for stays-here inputs |
| `mac_app/tests/test_link_state.py` | 14 | viewmodel | link | the words each link state shows |
| `mac_app/tests/test_login_item.py`, class `LoginItemTests`: `test_requires_approval_has_a_clear_status` | 1 | viewmodel | platform | moved from platform (#65): the words that name a login item waiting for approval; the class's other tests stay platform's |
| `mac_app/tests/test_mac_listener_port.py` | 4 | discovery | link | the port the listener and the beacon use |
| `mac_app/tests/test_mac_loopback.py` | 12 | driving | input, clipboard | the Mac driving a PC and driven by it over loopback, end to end |
| `mac_app/tests/test_mac_three.py` (its other classes) | 18 | crossing | driving | a push through a zone takes the machine the zone leads to, with three machines |
| `mac_app/tests/test_mac_to_mac.py` | 4 | input | driving | two Macs on loopback; both classes check which keys arrive |
| `mac_app/tests/test_mac_wire.py` | 12 | driving | input, link | the Mac's controller against a real responder, end to end |
| `mac_app/tests/test_map_ticks.py` | 22 | ui | viewmodel | the crossing map's thirds, badge, fit and seam; the tests are their geometry |
| `mac_app/tests/test_notices_mac.py` (its other tests) | 8 | platform | viewmodel | notification delivery (F36) is platform's; what a notice says is the viewmodel's |
| `mac_app/tests/test_notices_mac.py`, class `TrayNoticeTest`: `test_repeated_refusals_update_the_tray_but_post_one_notice`, `test_the_tray_app_posts_through_the_notices_not_rumps_with_addresses_hidden_as_set`, `test_tray_state_names_the_current_input_owner` | 3 | viewmodel | platform | moved from platform (#65): which refusals raise a notification, a notification that hides addresses as set, and what the tray menu says of who drives; the class's US-spelling test stays platform's |
| `mac_app/tests/test_part_edge_thirds.py` | 9 | crossing | viewmodel | map hit bands and ledger segments share the saved third |
| `mac_app/tests/test_same_on_both.py` | 46 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `mac_app/tests/test_settings_v6.py`, class `OverlapTests` | 8 | crossing | settings | overlapping zones in the saved settings |
| `mac_app/tests/test_settings_v6.py`, class `PairedAndRemovedTests` | 19 | pairing | settings | the peers kept in the settings file |
| `mac_app/tests/test_sidebar_response.py` | 11 | viewmodel | link | which response time the sidebar shows |
| `mac_app/tests/test_tile_resize_mac.py` | 13 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `mac_app/tests/test_update_config_redirecting.py` | 2 | driving | crossing | a settings reload while driving |
| `win_app/tests/test_admin_mode.py`, class `AdminModeTests`: `test_helper_only_accepts_fixed_boolean_argument`, `test_helper_rejects_relative_config_before_changing_anything`, `test_startup_helper_rejects_show_argument` | 3 | app | platform | moved from platform (#65): the elevated command line accepts only its fixed arguments and changes nothing for any other |
| `win_app/tests/test_admin_mode_ui.py` | 10 | viewmodel | platform | the admin mode controls and their diagnostics on the Input page |
| `win_app/tests/test_app_config.py`, class `AppearanceConfigTests` | 3 | ui | settings | the saved appearance choice |
| `win_app/tests/test_app_config.py`, class `EdgeGlowConfigTests` | 2 | ui | settings | the saved edge glow choice |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests` | 15 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_app_config.py`, class `ReadFallbackPeerBookTests` | 4 | pairing | settings | reading the peers from the settings file |
| `win_app/tests/test_atlas_canvas.py` | 24 | viewmodel | ui, crossing | the crossing map's canvas, as on the Mac |
| `win_app/tests/test_capture_portal.py` (its other classes) | 62 | input | crossing | Wayland capture, with the barriers that make zones there |
| `win_app/tests/test_capture_win_seam.py` | 6 | crossing | input | the capture hook's seam with the crossing engine |
| `win_app/tests/test_crossing_fix_pass.py` | 11 | ui | viewmodel | regressions on the Crossing page, mostly measured |
| `win_app/tests/test_crossing_ledger.py` | 14 | viewmodel | crossing | the ledger's controls on the Crossing page (F6) |
| `win_app/tests/test_cursor_overlay_native.py` | 1 | ui | crossing | the drawn cursor overlay window (F16); ui's scope names its drawing |
| `win_app/tests/test_desktop_portal_cache.py` | 10 | input | crossing | Wayland input readers' display geometry |
| `win_app/tests/test_desktop_x11.py` | 22 | input | crossing | the X11 desktop's pointer, buttons and full-screen check |
| `win_app/tests/test_dormant_rows.py` | 11 | viewmodel | pairing, crossing | how a dormant peer's rows show |
| `win_app/tests/test_gameinput_foreground.py`, class `GameInputForegroundTests`: `test_stale_visit_neither_moves_the_cursor_nor_calls_arrival` | 1 | crossing | platform | moved from platform (#65): a landing for a take that is no longer current neither moves the pointer nor reports it landed |
| `win_app/tests/test_hook_key_repeat.py` | 8 | crossing | input | a held shortcut key's repeats in the hook |
| `win_app/tests/test_idle_refresh.py` | 16 | viewmodel | runtime, ui | how often the window re-reads settings and redraws |
| `win_app/tests/test_input_blocker.py`, class `ForegroundBlockerTests`: `test_all_messages_translate_before_interpolating_process_name` | 1 | viewmodel | platform | moved from platform (#65): an input problem's words go through `tr()` before the blocker's name is put in |
| `win_app/tests/test_installer_maintenance.py`, class `LauncherArgumentHandoffTests` | 2 | app | platform | moved from platform (#65): what the elevated command line is given, and a path it refuses before it runs |
| `win_app/tests/test_key_up_after_switch_back.py` | 7 | input | driving | a key released after input came home still reaches the peer |
| `win_app/tests/test_links.py`, class `OnwardTests` | 16 | driving | crossing | pass on, when the driven machine's own zone fires |
| `win_app/tests/test_links.py`, class `SettingsTests` | 6 | settings | crossing | settings that change what the sender does |
| `win_app/tests/test_links_loopback.py` | 9 | driving | input | the PC's input to a real responder on loopback |
| `win_app/tests/test_map_ticks.py` | 20 | ui | viewmodel | the crossing map's thirds, badge, status dot, fit and seam; the tests are their geometry |
| `win_app/tests/test_parity.py`, class `AlertAndWakeTests` | 4 | discovery | viewmodel | the alerts and waking the Mac when a switch has no link |
| `win_app/tests/test_part_edge_corners_qt.py` | 12 | crossing | viewmodel | map thirds, contact geometry and ledger parity |
| `win_app/tests/test_same_on_both.py` | 54 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `win_app/tests/test_sender_motion_flow.py` | 8 | link | input, crossing | the sender batching motion before the link, keeping travel and barriers |
| `win_app/tests/test_settings_migration.py`, class `OverlapTests` | 7 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_tile_resize_qt.py` | 16 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `win_app/tests/test_vcam_install.py` | 38 | camera | platform | installing the virtual camera; camera's scope names it |
| `win_app/tests/test_windows_tool_encoding.py`, class `WindowsToolEncodingTests`: `test_profile_overrides_keep_settings_and_logs_in_the_unicode_directory` | 1 | runtime | platform | moved from platform (#65): the settings and the log are kept in a data folder whose path is outside ASCII |
