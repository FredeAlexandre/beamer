# The ledger's owner domains

#51 laid the ledger out: every Python test at `archive/python`, 6,232 of them, in exactly one
domain's slice, `spec/ledger/<domain>.txt`, with no fate yet. The format and the check are in
[`spec/README.md`](../README.md#the-python-ledger). Each domain's miner gives its slice's tests their
fates and adds or removes no line (the `build-spec-miner` skill).

The domains are the twenty "Spec: <domain>" tasks under #24. Every domain has a slice, empty where
no Python test is its own:

| Domain | Task | Tests |
|---|---|---|
| `link` | #52 | 324 |
| `pairing` | #53 | 286 |
| `store` | #54 | 2 |
| `discovery` | #55 | 117 |
| `driving` | #56 | 331 |
| `crossing` | #57 | 1033 |
| `input` | #58 | 803 |
| `clipboard` | #59 | 213 |
| `settings` | #60 | 467 |
| `display` | #61 | 31 |
| `audio` | #62 | 148 |
| `camera` | #63 | 202 |
| `media` | #64 | 0 |
| `platform` | #65 | 339 |
| `runtime` | #66 | 3 |
| `app` | #67 | 93 |
| `viewmodel` | #68 | 1070 |
| `ui` | #69 | 770 |
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

`media`, `perf` and `observability` own no Python test: the media engine is C++, and `perf` and
`observability` are written from #4, #14 and the other domains. `store` was given none by #51,
since Python had no store (each feature flooded its own messages, and those tests went to the
feature); it owns only the 2 the crossing miner moved to it (#57), below.

## Ambiguous assignments

An assignment is ambiguous when rule 3 or 4 had to choose: a second domain is a fair owner too.
There are 127, covering 1,238 tests. Each row names the other candidates and why the owner won.
A miner who finds such a test's assertions are all another domain's settles it as
`.claude/skills/build-spec-miner/blocked.md` says, without moving the line.

The rows marked "moved from crossing (#57)" record lines that did move, after #51: the crossing
miner found their assertions all another domain's (#147), and moved them to that domain's slice
with no fate. Most split a class, an exception to rule 2's "no assignment finer than a class":
the class's other tests stay crossing's.

| Tests | Count | Owner | Also a candidate | Why the owner |
|---|---|---|---|---|
| `core/tests/test_address_choice_commit.py` | 3 | discovery | pairing | the address a peer is reached at, chosen by the user, is discovery's address list |
| `core/tests/test_arrival_grace.py`, class `IncomingMoveLogging` | 2 | input | crossing | moved from crossing (#57): a driven machine's log of the pointer moves its driver sends |
| `core/tests/test_atlas.py`, class `CrossingStretchTests` | 4 | viewmodel | crossing | moved from crossing (#57): which stretches of a contact the crossing map lights |
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
| `core/tests/test_motion_paths.py`, class `MotionPathTests`: `test_a_path_past_its_limit_collapses_to_the_latest_position`, `test_coalescing_keeps_original_mixed_axis_segments` | 2 | link | crossing | moved from crossing (#57): merging pointer moves before they cross the link |
| `core/tests/test_pairing_paths.py`, class `BroadcastTargetTests` | 3 | discovery | pairing | where the beacon is broadcast on a machine with several networks |
| `core/tests/test_pairing_paths.py`, class `HostileNearbyDatagramTests` | 2 | discovery | pairing | hostile datagrams on the discovery socket |
| `core/tests/test_pairing_paths.py`, class `InterfaceTests` | 4 | discovery | pairing | which interfaces a machine announces on |
| `core/tests/test_pairing_paths.py`, class `OversizedDatagramTests` | 1 | discovery | pairing | an oversized datagram on the discovery socket |
| `core/tests/test_patch15_nearby.py` | 3 | pairing | viewmodel | the nearby list's refresh, for pair nearby (F3, dropped) |
| `core/tests/test_portable_discovery.py` | 5 | platform | discovery | the portable build's discovery; the portable build (F23, dropped) is kept in one domain |
| `core/tests/test_profiles.py`, class `ProfilesTests`: `test_bad_nested_register_is_preserved_not_executed`, `test_unreadable_objects_are_kept_aside` | 2 | store | crossing | moved from crossing (#57): unreadable stored profile state is set aside, as the store does with any entry |
| `core/tests/test_profiles_e2e.py` | 30 | crossing | settings, store | profiles synced over the links; profiles are crossing's (F7) |
| `core/tests/test_profiles_migration.py` | 12 | settings | crossing | the settings-file migration to profiles; settings owns the settings file and its migrations |
| `core/tests/test_protocol_v6.py`, class `ClipboardImages` | 10 | clipboard | link | the v6 clipboard image payload |
| `core/tests/test_protocol_v6.py`, class `Focus` | 10 | driving | link | the v6 messages that take and let go, encoded by the wire |
| `core/tests/test_protocol_v6.py`, class `Input` | 4 | input | link | the v6 input frames |
| `core/tests/test_protocol_v6.py`, class `InputMethodKeyFrames` | 1 | input | link | the v6 input-method key frames |
| `core/tests/test_protocol_v6.py`, class `Keys` | 9 | input | link | the v6 key frames |
| `core/tests/test_receiver_backlog.py` | 12 | link | input | receiver backpressure and input order over the link |
| `core/tests/test_rel1510b_geometry.py`, class `ReceiverQueuedGeometryTests`: `test_a_long_bent_drag_collapses_to_one_native_move`, `test_a_long_path_near_a_wall_collapses_to_one_clipped_native_move`, `test_interior_batch_preserves_per_sample_scaling_and_fractional_carry`, `test_interior_drag_keeps_each_native_path_segment`, `test_unarmed_interior_and_parallel_wall_paths_still_batch_native_calls`, `test_unarmed_wall_reversals_keep_each_native_clipping_step` | 6 | input | crossing | moved from crossing (#57): how merged pointer moves are injected on a driven machine |
| `core/tests/test_responder_ownership.py`, class `WrapWayBack` | 2 | crossing | driving | the way back from a driven machine |
| `core/tests/test_responder_ownership.py`, class `ZoneModels` | 5 | crossing | driving | the zones a driven machine arms for its driver |
| `core/tests/test_responder_ownership.py`, class `Zones` (its other tests) | 7 | crossing | driving | the zones a driven machine arms for its driver |
| `core/tests/test_responder_ownership.py`, class `Zones`: `test_resistance_out_of_range_is_malformed` | 1 | driving | crossing | moved from crossing (#57): a take with a push distance out of range is malformed |
| `core/tests/test_responder_ported.py`, class `RearmNeverRevives`: `test_a_frame_replayed_whole_ends_the_link` | 1 | link | crossing | moved from crossing (#57): a replayed frame ends the link |
| `core/tests/test_shortcut_cycle.py`, class `ShortcutCycleTests`: `test_shortcut_mode_is_a_valid_optional_crossing_sync_value` | 1 | settings | crossing | moved from crossing (#57): the shortcut destination as a synced setting |
| `core/tests/test_three_desks.py` | 15 | driving | crossing | #56's Scope names the three-machine tests for driving |
| `core/tests/test_way_helpers.py`, class `WayHelperTests`: `test_part_names_follow_the_edge_direction`, `test_parts_phrase_orders_known_parts_and_defaults_to_middle` | 2 | viewmodel | crossing | moved from crossing (#57): the names of an edge's thirds in sentences |
| `core/tests/test_ways.py`, class `PartOfTheEdgeIsOneChoiceWithTheEdge`: `test_the_map_click_targets_are_the_thirds_a_contact_covers` | 1 | ui | crossing | moved from crossing (#57): the crossing map's click areas on a contact |
| `core/tests/test_ways.py`, class `WayBack`: `test_the_sentence_names_the_machine_in_the_way_and_where_to_change_it`, `test_the_sentence_names_the_machine_with_no_way_back` | 2 | viewmodel | crossing | moved from crossing (#57): the Crossing page's sentences about a peer with no zone leading here |
| `core/tests/test_windows_launch_manifest.py` | 1 | platform | app | the Windows manifest asks for no elevation at launch, an install step (#21) |
| `mac_app/tests/test_atlas_canvas.py` (its other classes) | 60 | viewmodel | ui, crossing | the crossing map's canvas; its tests are mostly what it shows and what a drag, drop or key does |
| `mac_app/tests/test_atlas_canvas.py`, class `WrapAroundSeams`: `test_a_kept_side_is_a_two_point_dashed_seam_that_says_edge_is_off`, `test_a_pad_is_drawn_for_every_corner_in_a_machines_list`, `test_outcome_is_crosses_for_a_wrap_around_edge_even_with_the_contact_unpicked` | 3 | viewmodel | crossing | moved from crossing (#57): what the crossing map shows for wrap-around zones and corners |
| `mac_app/tests/test_atlas_canvas.py`, class `WrapAroundSeams`: `test_a_seam_stops_thirteen_points_short_of_a_corner_pads_vertex`, `test_a_side_that_stops_wrapping_loses_its_layers`, `test_another_machines_pad_also_stops_the_seam`, `test_each_wrap_around_side_has_a_four_point_seam_on_this_macs_own_side`, `test_label_and_arrow_sit_inside_the_tile_by_side`, `test_no_wrap_label_ever_lands_on_the_name_the_status_line_or_another_label`, `test_opacity_follows_the_seam_rules_offline_and_not_selected` | 7 | ui | crossing | moved from crossing (#57): how wrap-around seams are drawn on the crossing map |
| `mac_app/tests/test_audio_driver.py` | 23 | audio | platform | installing the virtual microphone; audio's scope names it |
| `mac_app/tests/test_bridge.py`, class `ControllerTests` | 37 | driving | crossing, link | the shortcut, take, refuse and send home, and alerts on a failed link |
| `mac_app/tests/test_bridge.py`, class `CrossingWiringTests`: `test_a_sleeping_display_still_bounds_the_desktop`, `test_default_desktop_bounds_is_the_union_of_every_display` | 2 | platform | crossing | moved from crossing (#57): the Mac's desktop bounds, a sleeping display included |
| `mac_app/tests/test_bridge.py`, class `CrossingWiringTests`: `test_inbound_switch_to_an_unknown_target_is_ignored`, `test_no_crossing_while_windows_drives_and_cannot_be_reached` | 2 | driving | crossing | moved from crossing (#57): a request to cross on to an unknown machine, and crossing from a driven machine whose driver is unreachable |
| `mac_app/tests/test_bridge.py`, class `CursorPinningTests` (its other tests) | 8 | crossing | input | the pointer held at the edge while pushing |
| `mac_app/tests/test_bridge.py`, class `CursorPinningTests`: `test_warp_error_logged_once_not_spammed` | 1 | input | crossing | moved from crossing (#57): a refused pointer placement logged once |
| `mac_app/tests/test_bridge.py`, class `RoundTripTests` | 4 | viewmodel | link | when a response-time sample is shown |
| `mac_app/tests/test_config_key_map_override.py` | 1 | input | settings | a saved key map override |
| `mac_app/tests/test_corner_marks.py` | 20 | ui | viewmodel | the crossing map's corner marks; the tests are mostly their pixels and geometry |
| `mac_app/tests/test_crossing.py`, class `ResistanceTests`: `test_track_endpoints_and_fill_head_share_the_same_coordinate_scale` | 1 | ui | crossing | moved from crossing (#57): the resistance strip's track |
| `mac_app/tests/test_crossing.py`, class `TickStepsTests` | 1 | platform | crossing, ui | moved from crossing (#57): which push-pressure ticks the trackpad's haptics fire, and haptics (F32) are platform's |
| `mac_app/tests/test_crossing_ledger.py` | 32 | viewmodel | crossing | the ledger's controls on the Crossing page (F6); what they show and change |
| `mac_app/tests/test_crossing_matrix.py`, class `MacDrivenTests`: `test_a_pc_this_mac_does_not_allow_to_drive_it_is_refused` | 1 | driving | crossing | moved from crossing (#57): a take from a peer that may not drive this machine is refused |
| `mac_app/tests/test_crossing_matrix.py`, class `WhatIsSharedTests` | 1 | viewmodel | crossing | moved from crossing (#57): what the Crossing page says is shared |
| `mac_app/tests/test_dock_visibility.py` | 6 | viewmodel | platform | Show in Dock (F27), which the viewmodel's scope names |
| `mac_app/tests/test_dock_window.py` | 14 | viewmodel | platform | Show in Dock and menu bar (F27), which the viewmodel's scope names |
| `mac_app/tests/test_dormant_rows.py` | 11 | viewmodel | pairing, crossing | how a dormant peer's rows show and what re-pair opens |
| `mac_app/tests/test_idle_refresh.py` | 6 | viewmodel | runtime, ui | how often the window and menu re-read settings and redraw |
| `mac_app/tests/test_ignored_titles.py` | 16 | viewmodel | input | the names shown for stays-here inputs |
| `mac_app/tests/test_link_state.py` | 14 | viewmodel | link | the words each link state shows |
| `mac_app/tests/test_mac_listener_port.py` | 4 | discovery | link | the port the listener and the beacon use |
| `mac_app/tests/test_mac_loopback.py` | 12 | driving | input, clipboard | the Mac driving a PC and driven by it over loopback, end to end |
| `mac_app/tests/test_mac_three.py` (its other classes) | 18 | crossing | driving | a push through a zone takes the machine the zone leads to, with three machines |
| `mac_app/tests/test_mac_to_mac.py` | 4 | input | driving | two Macs on loopback; both classes check which keys arrive |
| `mac_app/tests/test_mac_wire.py` | 12 | driving | input, link | the Mac's controller against a real responder, end to end |
| `mac_app/tests/test_map_ticks.py` | 22 | ui | viewmodel | the crossing map's thirds, badge, fit and seam; the tests are their geometry |
| `mac_app/tests/test_notices_mac.py` | 11 | platform | viewmodel | notification delivery (F36) is platform's; what a notice says is the viewmodel's |
| `mac_app/tests/test_part_edge_thirds.py` (its other tests) | 4 | crossing | viewmodel | map hit bands and ledger segments share the saved third |
| `mac_app/tests/test_part_edge_thirds.py`, class `PartEdgeThirds`: `test_map_clicks_each_third_on_each_side_and_ledger_follows` | 1 | viewmodel | crossing | moved from crossing (#57): a click on a third of the crossing map |
| `mac_app/tests/test_part_edge_thirds.py`, class `PartEdgeThirds`: `test_mouse_down_and_drag_wait_for_mouse_up_and_never_move_tiles`, `test_releasing_in_another_third_cancels`, `test_selected_tracks_breaks_and_other_peers_seam`, `test_tooltips_cursor_and_five_pixel_band` | 4 | ui | crossing | moved from crossing (#57): how the crossing map's thirds are pressed, drawn and labelled |
| `mac_app/tests/test_same_on_both.py` | 46 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `mac_app/tests/test_settings_v6.py`, class `OverlapTests` | 8 | crossing | settings | overlapping zones in the saved settings |
| `mac_app/tests/test_settings_v6.py`, class `PairedAndRemovedTests` | 19 | pairing | settings | the peers kept in the settings file |
| `mac_app/tests/test_sidebar_response.py` | 11 | viewmodel | link | which response time the sidebar shows |
| `mac_app/tests/test_tile_resize_mac.py` | 13 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `mac_app/tests/test_update_config_redirecting.py` | 2 | driving | crossing | a settings reload while driving |
| `win_app/tests/test_admin_mode_ui.py` | 10 | viewmodel | platform | the admin mode controls and their diagnostics on the Input page |
| `win_app/tests/test_app_config.py`, class `AppearanceConfigTests` | 3 | ui | settings | the saved appearance choice |
| `win_app/tests/test_app_config.py`, class `EdgeGlowConfigTests` | 2 | ui | settings | the saved edge glow choice |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests` (its other tests) | 3 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests`: `test_a_colour_resolves_to_todays_palette_else_its_pack`, `test_all_original_saved_colours_keep_their_id_and_stops` | 2 | ui | crossing | moved from crossing (#57): the edge glow colours |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests`: `test_any_colour_goes_with_any_style`, `test_effect_size_defaults_to_medium_and_round_trips`, `test_every_crossing_effect_and_pack_is_accepted_and_round_trips`, `test_frame_rate_defaults_to_display_and_round_trips`, `test_the_length_defaults_to_normal_and_round_trips`, `test_what_a_switch_plays_defaults_to_the_crossing_and_round_trips` | 6 | ui | crossing | moved from crossing (#57): the saved crossing effect choices, F30 (Later), whose room `ui` names (rule 6) |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests`: `test_colour_choices_are_the_shared_palettes_then_the_effect_packs`, `test_rejects_an_unknown_style_or_colour`, `test_showing_where_the_pointer_lands_is_on_for_an_old_config_and_round_trips_off`, `test_style_and_colour_default_to_the_original_look_and_round_trip` | 4 | settings | crossing | moved from crossing (#57): the edge glow style and colour settings, and showing where the pointer lands |
| `win_app/tests/test_app_config.py`, class `ReadFallbackPeerBookTests` | 4 | pairing | settings | reading the peers from the settings file |
| `win_app/tests/test_arrangement.py`, class `LearnedTests`: `test_the_stamps_author_for_same_on_all_machines_is_kept` | 1 | settings | crossing | moved from crossing (#57): the author of the Same on all machines stamp |
| `win_app/tests/test_atlas_canvas.py` | 24 | viewmodel | ui, crossing | the crossing map's canvas, as on the Mac |
| `win_app/tests/test_capture_portal.py` (its other classes) | 62 | input | crossing | Wayland capture, with the barriers that make zones there |
| `win_app/tests/test_capture_win_seam.py` (its other tests) | 5 | crossing | input | the capture hook's seam with the crossing engine |
| `win_app/tests/test_capture_win_seam.py`, class `InternalSeamTests`: `test_a_set_cursor_failure_fails_open` | 1 | input | crossing | moved from crossing (#57): the Windows capture hook failing open when the pointer cannot be placed |
| `win_app/tests/test_crossing_fix_pass.py` | 11 | ui | viewmodel | regressions on the Crossing page, mostly measured |
| `win_app/tests/test_crossing_ledger.py` | 14 | viewmodel | crossing | the ledger's controls on the Crossing page (F6) |
| `win_app/tests/test_crossing_matrix.py`, class `WaysReachTheDesignPageTests` | 1 | viewmodel | crossing | moved from crossing (#57): the look page's preview |
| `win_app/tests/test_crossing_matrix.py`, class `WhatIsSharedTests` | 1 | viewmodel | crossing | moved from crossing (#57): what the Crossing page says is shared |
| `win_app/tests/test_cursor_overlay.py`, class `CursorLifecycleTests`: `test_arrow_contents_refresh_when_windows_reuses_its_handle` | 1 | ui | crossing | moved from crossing (#57): how the pointer overlay's arrow is drawn |
| `win_app/tests/test_cursor_overlay_native.py` | 1 | ui | crossing | the drawn cursor overlay window (F16); ui's scope names its drawing |
| `win_app/tests/test_desktop_portal_cache.py` | 10 | input | crossing | Wayland input readers' display geometry |
| `win_app/tests/test_desktop_x11.py` | 22 | input | crossing | the X11 desktop's pointer, buttons and full-screen check |
| `win_app/tests/test_dormant_rows.py` | 11 | viewmodel | pairing, crossing | how a dormant peer's rows show |
| `win_app/tests/test_global_shortcut.py`, class `GlobalShortcutTests`: `test_destination_choices_refresh_visible_and_accessible_detail`, `test_tile_is_first_and_key_detail_tracks_local_recording` | 2 | viewmodel | crossing | moved from crossing (#57): the Shortcut setting's detail |
| `win_app/tests/test_hook_key_repeat.py` | 8 | crossing | input | a held shortcut key's repeats in the hook |
| `win_app/tests/test_idle_refresh.py` | 16 | viewmodel | runtime, ui | how often the window re-reads settings and redraws |
| `win_app/tests/test_key_up_after_switch_back.py` | 7 | input | driving | a key released after input came home still reaches the peer |
| `win_app/tests/test_links.py`, class `OnwardTests` | 16 | driving | crossing | pass on, when the driven machine's own zone fires |
| `win_app/tests/test_links.py`, class `SettingsTests` | 6 | settings | crossing | settings that change what the sender does |
| `win_app/tests/test_links_loopback.py` | 9 | driving | input | the PC's input to a real responder on loopback |
| `win_app/tests/test_map_ticks.py` | 20 | ui | viewmodel | the crossing map's thirds, badge, status dot, fit and seam; the tests are their geometry |
| `win_app/tests/test_parity.py`, class `AlertAndWakeTests` | 4 | discovery | viewmodel | the alerts and waking the Mac when a switch has no link |
| `win_app/tests/test_parity.py`, class `ReviewFixTests`: `test_the_subnet_broadcast_of_a_home_address` | 1 | discovery | crossing | moved from crossing (#57): the subnet broadcast a wake packet goes to |
| `win_app/tests/test_parity.py`, class `TriggerKeyTests`: `test_every_trigger_is_titled_in_windows_words` | 1 | viewmodel | crossing | moved from crossing (#57): the shortcut key's name in Windows words |
| `win_app/tests/test_part_edge_corners_qt.py` (its other tests) | 3 | crossing | viewmodel | map thirds, contact geometry and ledger parity |
| `win_app/tests/test_part_edge_corners_qt.py`, class `PartEdgeCornersTests`: `test_a_corner_only_machine_has_its_pad_and_no_seam`, `test_a_grip_on_the_seam_leaves_the_end_band_to_its_third`, `test_a_tile_inside_the_band_keeps_its_end_third_and_its_grip`, `test_band_is_five_pixels_wide_and_available_when_edge_is_off`, `test_inactive_peers_have_no_corner_mark`, `test_selected_seam_and_tracks_break_at_the_thirds` | 6 | ui | crossing | moved from crossing (#57): how the crossing map's thirds, corner marks and grips are drawn |
| `win_app/tests/test_part_edge_corners_qt.py`, class `PartEdgeCornersTests`: `test_contact_chip_and_corner_buttons_wait_for_a_side_contact`, `test_each_side_orients_and_toggles_its_three_bands`, `test_map_click_toggles_the_row_without_a_drag` | 3 | viewmodel | crossing | moved from crossing (#57): which edge and corner choices the crossing map offers, and a click on a third |
| `win_app/tests/test_pc_three_machines.py`, class `ShortcutCycleRefreshTests` | 1 | viewmodel | crossing | moved from crossing (#57): each peer's tick for the shortcut's cycle |
| `win_app/tests/test_pc_three_machines.py`, class `ShortcutMachineTests`: `test_the_status_does_not_name_a_machine_not_in_use`, `test_the_status_is_the_shortcut_machines` | 2 | viewmodel | crossing | moved from crossing (#57): the connection status about the shortcut's machine |
| `win_app/tests/test_return_edge.py`, class `PressureTests`: `test_track_endpoints_and_fill_head_share_the_same_coordinate_scale` | 1 | ui | crossing | moved from crossing (#57): the resistance strip's track |
| `win_app/tests/test_same_on_both.py` | 54 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `win_app/tests/test_sender_motion_flow.py` | 8 | link | input, crossing | the sender batching motion before the link, keeping travel and barriers |
| `win_app/tests/test_settings_migration.py`, class `OverlapTests` | 7 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_tile_resize_qt.py` | 16 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `win_app/tests/test_vcam_install.py` | 38 | camera | platform | installing the virtual camera; camera's scope names it |
