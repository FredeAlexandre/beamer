# The ledger's owner domains

#51 laid the ledger out: every Python test at `archive/python`, 6,232 of them, in exactly one
domain's slice, `spec/ledger/<domain>.txt`, with no fate yet. The format and the check are in
[`spec/README.md`](../README.md#the-python-ledger). Each domain's miner gives its slice's tests their
fates and adds or removes no line (the `build-spec-miner` skill).

The domains are the twenty "Spec: <domain>" tasks under #24. Every domain has a slice, empty where
no Python test is its own:

| Domain | Task | Tests |
|---|---|---|
| `link` | #52 | 326 |
| `pairing` | #53 | 289 |
| `store` | #54 | 0 |
| `discovery` | #55 | 117 |
| `driving` | #56 | 253 |
| `crossing` | #57 | 1136 |
| `input` | #58 | 812 |
| `clipboard` | #59 | 228 |
| `settings` | #60 | 471 |
| `display` | #61 | 31 |
| `audio` | #62 | 148 |
| `camera` | #63 | 202 |
| `media` | #64 | 0 |
| `platform` | #65 | 336 |
| `runtime` | #66 | 3 |
| `app` | #67 | 93 |
| `viewmodel` | #68 | 1046 |
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
There are 126, covering 1,232 tests. Each row names the other candidates and why the owner won.
A miner who finds such a test's assertions are all another domain's settles it as
`.claude/skills/build-spec-miner/blocked.md` says, without moving the line.

Thirty-seven rows record lines that did move, after #51: the driving miner (#56) found their
assertions all another domain's, and moved them to that domain's slice with no fate, as #147
decides. Most split a class, an exception to rule 2's "no assignment finer than a class": the
class's other tests stay where they were. Five tests of `core/tests/test_three_desks.py` moved too,
though rule 5 named the file for driving: their assertions are the crossing map's and the zones'.

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
| `core/tests/test_responder_ownership.py`, class `Ending`: `test_each_link_keeps_its_own_seq` | 1 | link | driving | moved from driving (#56): each link counts its own sequence numbers and acknowledgements |
| `core/tests/test_responder_ownership.py`, class `LeftOutDriverWayHome` | 1 | crossing | driving | moved from driving (#56): the zone a driven machine arms toward a driver kept off its crossing map |
| `core/tests/test_responder_ownership.py`, class `NonOwners`: `test_a_non_owners_arrangement_settings_and_paired_are_handled`, `test_settings_from_a_peer_without_the_capability_are_ignored` | 2 | settings | driving, store | moved from driving (#56): settings, arrangements and paired lists are taken from any peer, settings only from a peer that keeps them |
| `core/tests/test_responder_ownership.py`, class `NonOwners`: `test_a_non_owners_clipboard_is_dropped_and_the_owners_is_set`, `test_the_owners_large_clipboard_still_arrives` | 2 | clipboard | driving | moved from driving (#56): a clipboard from a peer that is not the driver is dropped, and the driver's arrives however large |
| `core/tests/test_responder_ownership.py`, class `NonOwners`: `test_a_non_owners_large_frame_is_authenticated_and_dropped_without_being_parsed` | 1 | link | driving | moved from driving (#56): a large message the app does not expect from a peer is discarded unread, and the link stays up |
| `core/tests/test_responder_ownership.py`, class `WrapWayBack` | 2 | crossing | driving | the way back from a driven machine |
| `core/tests/test_responder_ownership.py`, class `ZoneModels` | 5 | crossing | driving | the zones a driven machine arms for its driver |
| `core/tests/test_responder_ownership.py`, class `Zones` | 8 | crossing | driving | the zones a driven machine arms for its driver |
| `core/tests/test_three_desks.py` (its other tests) | 10 | driving | crossing | #56's Scope names the three-machine tests for driving |
| `core/tests/test_three_desks.py`, class `AMachinePairedLate` | 2 | crossing | driving | moved from driving (#56): the edge chosen to face a peer, arriving from it, and the zones the crossing map then gives it |
| `core/tests/test_three_desks.py`, class `ARow`: `test_an_edge_with_no_zone_leads_nowhere` | 1 | crossing | driving | moved from driving (#56): an edge with no zone leads nowhere |
| `core/tests/test_three_desks.py`, class `AnL`: `test_the_arms_have_no_border_of_their_own` | 1 | crossing | driving | moved from driving (#56): edges with no zone lead nowhere |
| `core/tests/test_three_desks.py`, class `SharedEdge` | 1 | crossing | driving | moved from driving (#56): each third of an edge leads to its own peer |
| `core/tests/test_three_machines.py`, class `AChain`: `test_a_clipboard_copied_on_b_reaches_c_through_the_owner_when_b_is_left` | 1 | clipboard | driving | moved from driving (#56): the clipboard of a machine left in a chain reaches the next one through the driver |
| `core/tests/test_windows_launch_manifest.py` | 1 | platform | app | the Windows manifest asks for no elevation at launch, an install step (#21) |
| `mac_app/tests/test_atlas_canvas.py` (its other classes) | 60 | viewmodel | ui, crossing | the crossing map's canvas; its tests are mostly what it shows and what a drag, drop or key does |
| `mac_app/tests/test_atlas_canvas.py`, class `WrapAroundSeams` | 10 | crossing | viewmodel | wrap-around zones as the map shows them |
| `mac_app/tests/test_audio_driver.py` | 23 | audio | platform | installing the virtual microphone; audio's scope names it |
| `mac_app/tests/test_bridge.py`, class `ControllerTests` (its other tests) | 22 | driving | crossing, link | the shortcut, take, refuse and send home, and alerts on a failed link |
| `mac_app/tests/test_bridge.py`, class `ControllerTests`: `test_a_failed_connection_while_idle_drops_the_link_without_an_alert` | 1 | link | driving | moved from driving (#56): a link whose sending fails while input is home closes, and nothing is said to the user |
| `mac_app/tests/test_bridge.py`, class `ControllerTests`: `test_a_key_repeating_when_its_modifiers_arrive_is_not_a_jump_and_nothing_jumps_while_recording`, `test_double_tap_right_option_only_flips_redirect_flag`, `test_double_tap_survives_a_missed_key_up`, `test_every_way_home_reassociates_the_cursor`, `test_hold_shortcut_announces_an_unready_last_machine`, `test_holding_a_key_trigger_never_toggles`, `test_jump_chord_for_the_current_peer_returns_input_home_and_is_swallowed`, `test_next_shortcut_moves_from_home_to_the_first_machine`, `test_shortcut_off_passes_the_key_without_switching_in_each_input_state`, `test_switching_shortcut_off_discards_an_unfinished_double_tap` | 10 | crossing | driving | moved from driving (#56): the shortcut's double tap and hold, which machine it picks, jump keys, and the pointer pinned while driving |
| `mac_app/tests/test_bridge.py`, class `ControllerTests`: `test_an_event_the_tap_had_is_not_unseen_for_reading_the_clocks_apart`, `test_first_input_after_a_pause_is_not_mistaken_for_a_dead_tap`, `test_set_redirecting_off_resets_mouse_accumulators`, `test_the_tap_queues_physical_key_names_and_the_link_gets_the_peers` | 4 | input | driving | moved from driving (#56): when the Mac's capture counts as stopped, sub-pixel pointer motion, and the key names captured and sent |
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
| `mac_app/tests/test_mac_links.py`, class `AnnouncementTests`: `test_a_peers_settings_reach_the_app_with_who_sent_them`, `test_settings_are_not_announced_to_a_peer_that_does_not_keep_them`, `test_settings_go_on_to_every_peer_but_the_one_they_came_from`, `test_the_link_up_announces_the_side_the_settings_and_who_else_is_paired` | 4 | settings | driving, store | moved from driving (#56): settings sent on to every peer but their source and only to peers that keep them, and what a link announces as it comes up |
| `mac_app/tests/test_mac_links.py`, class `AnnouncementTests`: `test_an_arrangement_from_the_peer_reaches_the_app_as_this_macs_edge`, `test_an_arrangement_made_here_goes_to_the_machine_it_is_for` | 2 | crossing | driving, store | moved from driving (#56): the edge chosen to face a peer, sent to it and taken from it |
| `mac_app/tests/test_mac_links.py`, class `AnnouncementTests`: `test_the_available_pairing_ids_skip_machines_not_in_use` | 1 | pairing | driving, store | moved from driving (#56): the paired list leaves out peers not in use |
| `mac_app/tests/test_mac_listener_port.py` | 4 | discovery | link | the port the listener and the beacon use |
| `mac_app/tests/test_mac_loopback.py` (its other tests) | 7 | driving | input, clipboard | the Mac driving a PC and driven by it over loopback, end to end |
| `mac_app/tests/test_mac_loopback.py`, class `MacDrivesPcTests`: `test_a_key_goes_to_the_pc_swallowed_here_with_the_command_key_as_control`, `test_caps_lock_from_the_redirecting_mac_reaches_the_windows_responder` | 2 | input | driving | moved from driving (#56): the keys a PC injects for a Mac driver, Command as Control and Caps Lock |
| `mac_app/tests/test_mac_loopback.py`, class `MacDrivesPcTests`: `test_the_macs_clipboard_goes_with_the_take_and_the_pcs_comes_back_once_after_the_let_go` | 1 | clipboard | driving | moved from driving (#56): the clipboard follows the take, and comes back once after the let go |
| `mac_app/tests/test_mac_loopback.py`, class `PcDrivesMacTests`: `test_caps_lock_from_the_windows_peer_reaches_the_mac_injector`, `test_the_pcs_keys_and_pointer_are_injected_on_the_mac_in_order` | 2 | input | driving | moved from driving (#56): Caps Lock and the order of input injected on the Mac |
| `mac_app/tests/test_mac_review.py`, class `ArrangementOriginTests` | 1 | crossing | driving, store | moved from driving (#56): the edge chosen to face this machine, taken from each peer under its own id |
| `mac_app/tests/test_mac_review.py`, class `CapabilityAndPairedTests`: `test_a_changed_list_of_peers_is_announced_again`, `test_a_peers_paired_list_is_stored_with_its_entry` | 2 | pairing | driving, store | moved from driving (#56): the paired list sent again when it changes, and each peer's paired list kept |
| `mac_app/tests/test_mac_review.py`, class `CapabilityAndPairedTests`: `test_settings_from_a_peer_that_does_not_keep_them_are_ignored` | 1 | settings | driving, store | moved from driving (#56): settings from a peer that does not keep them are ignored |
| `mac_app/tests/test_mac_review.py`, class `StartupTests` | 2 | settings | driving | moved from driving (#56): the settings file read at start, and the defaults when it cannot be read |
| `mac_app/tests/test_mac_three.py` (its other classes) | 18 | crossing | driving | a push through a zone takes the machine the zone leads to, with three machines |
| `mac_app/tests/test_mac_to_mac.py` | 4 | input | driving | two Macs on loopback; both classes check which keys arrive |
| `mac_app/tests/test_mac_wire.py` (its other tests) | 10 | driving | input, link | the Mac's controller against a real responder, end to end |
| `mac_app/tests/test_mac_wire.py`, class `TakingTheFarMachineTests`: `test_command_arrives_as_control_on_a_pc_and_stays_command_on_a_mac` | 1 | input | driving | moved from driving (#56): Command injected as Control on a PC |
| `mac_app/tests/test_mac_wire.py`, class `TakingTheFarMachineTests`: `test_the_far_machines_clipboard_follows_the_input_home` | 1 | clipboard | driving | moved from driving (#56): the clipboard copied on the driven machine follows the input home |
| `mac_app/tests/test_map_ticks.py` | 22 | ui | viewmodel | the crossing map's thirds, badge, fit and seam; the tests are their geometry |
| `mac_app/tests/test_notices_mac.py` | 11 | platform | viewmodel | notification delivery (F36) is platform's; what a notice says is the viewmodel's |
| `mac_app/tests/test_part_edge_thirds.py` | 9 | crossing | viewmodel | map hit bands and ledger segments share the saved third |
| `mac_app/tests/test_same_on_both.py` | 46 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `mac_app/tests/test_settings_v6.py`, class `OverlapTests` | 8 | crossing | settings | overlapping zones in the saved settings |
| `mac_app/tests/test_settings_v6.py`, class `PairedAndRemovedTests` | 19 | pairing | settings | the peers kept in the settings file |
| `mac_app/tests/test_sidebar_response.py` | 11 | viewmodel | link | which response time the sidebar shows |
| `mac_app/tests/test_tile_resize_mac.py` | 13 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `mac_app/tests/test_update_config_redirecting.py`: `test_update_config_drops_a_link_dialled_to_an_address_that_is_no_longer_saved` | 1 | discovery | driving, link | moved from driving (#56): a link dialled to an address no longer saved for the peer closes |
| `mac_app/tests/test_update_config_redirecting.py`: `test_update_config_while_redirecting_restores_cursor_association` | 1 | crossing | driving | moved from driving (#56): the pointer given back to the hand when input comes home on a settings save |
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
| `win_app/tests/test_hook_key_repeat.py` | 8 | crossing | input | a held shortcut key's repeats in the hook |
| `win_app/tests/test_idle_refresh.py` | 16 | viewmodel | runtime, ui | how often the window re-reads settings and redraws |
| `win_app/tests/test_key_up_after_switch_back.py` | 7 | input | driving | a key released after input came home still reaches the peer |
| `win_app/tests/test_links.py`, class `OnwardTests` (its other tests) | 13 | driving | crossing | pass on, when the driven machine's own zone fires |
| `win_app/tests/test_links.py`, class `OnwardTests`: `test_text_during_chained_handover_brackets_a_modifier_waiting_to_be_replayed`, `test_text_replay_keeps_a_modifier_bracket_when_the_key_repeats_during_handover`, `test_text_replayed_after_a_refused_handover_brackets_the_current_machines_modifier` | 3 | input | driving | moved from driving (#56): text sent with the modifiers held released around it |
| `win_app/tests/test_links.py`, class `ReviewTests`: `test_a_link_lost_during_a_clipboard_read_drops_what_waited_and_a_retake_sends_the_clipboard_after_its_focus`, `test_a_read_asked_for_again_waits_its_turn_behind_a_write_decided_before_it`, `test_a_slow_clipboard_read_does_not_hold_up_the_take_of_another_peer`, `test_pointer_moves_during_a_clipboard_read_keep_their_relative_travel`, `test_what_follows_a_clipboard_read_for_that_peer_still_goes_after_it` | 5 | clipboard | driving | moved from driving (#56): reading this machine's clipboard for a take, and what waits behind that read |
| `win_app/tests/test_links.py`, class `ReviewTests`: `test_every_auto_repeat_of_a_key_keeps_the_name_its_press_went_under` | 1 | input | driving | moved from driving (#56): a key's repeats keep the name its press was sent under |
| `win_app/tests/test_links.py`, class `ReviewTests`: `test_settings_from_a_peer_that_does_not_list_the_capability_are_ignored` | 1 | settings | driving, store | moved from driving (#56): settings from a peer that does not keep them are ignored |
| `win_app/tests/test_links.py`, class `SettingsTests` | 6 | settings | crossing | settings that change what the sender does |
| `win_app/tests/test_links.py`, class `TakingTests`: `test_a_dropped_peer_forgets_held_key_names_before_reconnect`, `test_input_method_keys_send_one_tap_and_never_enter_held_key_state`, `test_noncomposable_dead_key_sequence_is_sent_as_text`, `test_noncomposable_dead_key_text_releases_and_restores_remote_modifiers`, `test_temporary_text_release_keeps_the_held_modifier_wire_name_if_style_changes` | 5 | input | driving | moved from driving (#56): input-method keys, dead keys sent as text, and the names modifiers are sent under |
| `win_app/tests/test_links.py`, class `TakingTests`: `test_a_peer_without_the_clipboard_capability_is_sent_none`, `test_an_image_too_large_to_decode_is_not_sent_but_the_text_is`, `test_the_clipboard_follows_the_take_and_goes_before_any_input`, `test_the_clipboard_is_not_sent_again_when_nothing_changed_here` | 4 | clipboard | driving | moved from driving (#56): which clipboard a take sends, when, and within which limits |
| `win_app/tests/test_links_loopback.py` (its other tests) | 4 | driving | input | the PC's input to a real responder on loopback |
| `win_app/tests/test_links_loopback.py`: `test_a_push_through_the_zone_takes_the_peer_at_the_matching_edge` | 1 | crossing | driving | moved from driving (#56): a push through a zone, and the landing point it gives |
| `win_app/tests/test_links_loopback.py`: `test_keys_and_the_pointer_reach_the_peers_injector_while_input_is_there` | 1 | input | driving | moved from driving (#56): the keys and pointer moves a peer injects |
| `win_app/tests/test_links_loopback.py`: `test_the_clipboard_arrives_before_any_input` | 1 | clipboard | driving | moved from driving (#56): the clipboard sent with a take arrives before any input |
| `win_app/tests/test_links_loopback.py`: `test_the_first_link_learns_the_peers_id_and_says_so`, `test_the_round_trip_is_measured_while_input_is_there` | 2 | link | driving | moved from driving (#56): a link learning its peer's machine id, and its round trip |
| `win_app/tests/test_map_ticks.py` | 20 | ui | viewmodel | the crossing map's thirds, badge, status dot, fit and seam; the tests are their geometry |
| `win_app/tests/test_parity.py`, class `AlertAndWakeTests` | 4 | discovery | viewmodel | the alerts and waking the Mac when a switch has no link |
| `win_app/tests/test_part_edge_corners_qt.py` | 12 | crossing | viewmodel | map thirds, contact geometry and ledger parity |
| `win_app/tests/test_same_on_both.py` | 54 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `win_app/tests/test_sender_motion_flow.py` | 8 | link | input, crossing | the sender batching motion before the link, keeping travel and barriers |
| `win_app/tests/test_settings_migration.py`, class `OverlapTests` | 7 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_tile_resize_qt.py` | 16 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `win_app/tests/test_vcam_install.py` | 38 | camera | platform | installing the virtual camera; camera's scope names it |
