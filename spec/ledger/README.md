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
| `pairing` | #53 | 293 |
| `store` | #54 | 0 |
| `discovery` | #55 | 116 |
| `driving` | #56 | 327 |
| `crossing` | #57 | 1155 |
| `input` | #58 | 795 |
| `clipboard` | #59 | 213 |
| `settings` | #60 | 466 |
| `display` | #61 | 36 |
| `audio` | #62 | 148 |
| `camera` | #63 | 202 |
| `media` | #64 | 0 |
| `platform` | #65 | 337 |
| `runtime` | #66 | 3 |
| `app` | #67 | 95 |
| `viewmodel` | #68 | 914 |
| `ui` | #69 | 811 |
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
There are 136, covering 1,278 tests. Each row names the other candidates and why the owner won.
A miner who finds such a test's assertions are all another domain's settles it as
`.claude/skills/build-spec-miner/blocked.md` says, without moving the line.

The rows marked "moved from viewmodel (#68)" record lines that did move, after #51: the viewmodel
miner found their assertions all another domain's (#147), and moved them to that domain's slice with
no fate. Most split a class, an exception to rule 2's "no assignment finer than a class": the
class's other tests stay viewmodel's.

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
| `mac_app/tests/test_atlas_canvas.py` (its other classes and tests) | 43 | viewmodel | ui, crossing | the crossing map's canvas; its tests are mostly what it shows and what a drag, drop or key does |
| `mac_app/tests/test_atlas_canvas.py`, class `AtlasCanvas`: `test_arrangement_box_centres_and_refits_after_a_peer_moves`, `test_canvas_focus_ring_follows_keyboard_navigation_and_leaves_with_focus`, `test_display_drag_keeps_its_map_transform_frozen`, `test_fit_centres_a_column_when_this_mac_is_at_one_end`, `test_map_fit_includes_split_display_offsets`, `test_own_name_is_above_its_display_layers`, `test_selected_and_own_tile_borders_use_neutral_roles`, `test_selection_relayout_does_not_change_either_drag_transform`, `test_shortcut_destination_labels_fit_without_wrapping`, `test_small_tiles_hide_detail_and_keep_title_clear_of_display_numbers` | 10 | ui | viewmodel | moved from viewmodel (#68): how the crossing map is drawn: its fit and centring, its scale during a drag, the focus ring, borders and labels |
| `mac_app/tests/test_atlas_canvas.py`, class `AtlasCanvas`: `test_distant_tile_with_a_notch_way_still_follows_placed_tiles`, `test_gap_machine_follows_visible_contacts_in_the_shortcut_cycle`, `test_incoming_layout_accepts_inactive_position_and_keeps_its_ways`, `test_incoming_layout_keeps_a_named_third_party_from_the_current_paired_list`, `test_left_out_peer_position_does_not_put_it_ahead_of_visible_peers`, `test_moving_a_tile_keeps_an_inactive_machine_position_and_ways`, `test_shortcut_cycle_uses_the_canvas_reading_order` | 7 | crossing | viewmodel | moved from viewmodel (#68): the crossing map and the shortcut's cycle as crossing derives them, checked through the map |
| `mac_app/tests/test_atlas_canvas.py`, class `WrapAroundSeams` | 10 | crossing | viewmodel | wrap-around zones as the map shows them |
| `mac_app/tests/test_audio_driver.py` | 23 | audio | platform | installing the virtual microphone; audio's scope names it |
| `mac_app/tests/test_audio_page.py`, class `AudioPageTests`: `test_main_thread_stays_under_a_frame_with_meters_at_20_hz` | 1 | ui | viewmodel | moved from viewmodel (#68): how fast the Audio page redraws |
| `mac_app/tests/test_bridge.py`, class `ControllerTests` | 37 | driving | crossing, link | the shortcut, take, refuse and send home, and alerts on a failed link |
| `mac_app/tests/test_bridge.py`, class `CursorPinningTests` | 9 | crossing | input | the pointer held at the edge while pushing |
| `mac_app/tests/test_bridge.py`, class `RoundTripTests` | 4 | viewmodel | link | when a response-time sample is shown |
| `mac_app/tests/test_camera_section.py`, class `CameraModuleTests`: `test_main_thread_stays_under_a_frame_with_camera_snapshots_at_4_hz`, `test_the_arrow_lanes_follow_the_tile_lines` | 2 | ui | viewmodel | moved from viewmodel (#68): how the Camera module is drawn and how fast it redraws |
| `mac_app/tests/test_config_key_map_override.py` | 1 | input | settings | a saved key map override |
| `mac_app/tests/test_corner_marks.py` | 20 | ui | viewmodel | the crossing map's corner marks; the tests are mostly their pixels and geometry |
| `mac_app/tests/test_crossing_ledger.py` (its other tests) | 27 | viewmodel | crossing | the ledger's controls on the Crossing page (F6); what they show and change |
| `mac_app/tests/test_crossing_ledger.py`, class `LedgerTests`: `test_chips_wrap_in_lines_and_corner_grid_is_two_by_two_at_both_widths`, `test_narrow_block_insets_every_cell_from_the_selection_rule`, `test_narrow_cells_and_visible_controls_fit_inside_the_ledger`, `test_selected_machine_status_dot_is_inset_from_the_rule_at_both_widths`, `test_window_width_selects_table_or_stacked_rows_without_sideways_scroll` | 5 | ui | viewmodel | moved from viewmodel (#68): how the ledger's cells and controls fit at each window width |
| `mac_app/tests/test_dock_visibility.py` | 6 | viewmodel | platform | Show in Dock (F27), which the viewmodel's scope names |
| `mac_app/tests/test_dock_window.py` (its other tests) | 12 | viewmodel | platform | Show in Dock and menu bar (F27), which the viewmodel's scope names |
| `mac_app/tests/test_dock_window.py`, class `DockWindowTests`: `test_same_on_all_machines_never_changes_this_macs_choice`, `test_same_on_all_machines_never_changes_this_macs_menu_bar_choice` | 2 | settings | viewmodel | moved from viewmodel (#68): Show in Dock and Show in menu bar stay this Mac's own under Same on all machines |
| `mac_app/tests/test_dormant_rows.py` (its other tests) | 7 | viewmodel | pairing, crossing | how a dormant peer's rows show and what re-pair opens |
| `mac_app/tests/test_dormant_rows.py`, class `DormantCrossingTests`: `test_dormant_layout_and_intent_survive_another_machine_moving`, `test_incoming_layout_accepts_dormant_position_and_keeps_intent` | 2 | crossing | viewmodel | moved from viewmodel (#68): a dormant peer's footprint and zone choices as the crossing map changes |
| `mac_app/tests/test_dormant_rows.py`, class `DormantCrossingTests`: `test_dormant_ledger_and_rename_fit_the_narrow_page` | 1 | ui | viewmodel | moved from viewmodel (#68): the dormant ledger row and its rename field fit a narrow page |
| `mac_app/tests/test_dormant_rows.py`, class `DormantCrossingTests`: `test_repair_keeps_label_layout_zones_and_left_out_after_reloading` | 1 | pairing | viewmodel | moved from viewmodel (#68): pairing again with a dormant peer keeps its label, footprint, zones and kept-off state |
| `mac_app/tests/test_idle_refresh.py` (its other tests) | 5 | viewmodel | runtime, ui | how often the window and menu re-read settings and redraw |
| `mac_app/tests/test_idle_refresh.py`, class `CachedSettingsTests`: `test_unchanged_load_does_not_read_but_atomic_replacement_does` | 1 | settings | viewmodel | moved from viewmodel (#68): when the settings file is read again |
| `mac_app/tests/test_ignored_titles.py` | 16 | viewmodel | input | the names shown for stays-here inputs |
| `mac_app/tests/test_link_state.py` (its other tests) | 12 | viewmodel | link | the words each link state shows |
| `mac_app/tests/test_link_state.py`, class `RulerScaleTest`: `test_round_trip_and_pinning`, `test_square_root_gives_the_useful_band_half_the_travel` | 2 | ui | viewmodel | moved from viewmodel (#68): a ruler control's scale |
| `mac_app/tests/test_mac_listener_port.py` | 4 | discovery | link | the port the listener and the beacon use |
| `mac_app/tests/test_mac_loopback.py` | 12 | driving | input, clipboard | the Mac driving a PC and driven by it over loopback, end to end |
| `mac_app/tests/test_mac_three.py` (its other classes) | 18 | crossing | driving | a push through a zone takes the machine the zone leads to, with three machines |
| `mac_app/tests/test_mac_to_mac.py` | 4 | input | driving | two Macs on loopback; both classes check which keys arrive |
| `mac_app/tests/test_mac_wire.py` | 12 | driving | input, link | the Mac's controller against a real responder, end to end |
| `mac_app/tests/test_machines_panel.py`, class `CodeBoxCentringTests`: `test_each_typed_digit_sits_in_the_middle_of_its_box` | 1 | ui | viewmodel | moved from viewmodel (#68): the digits centred in the code boxes |
| `mac_app/tests/test_machines_panel.py`, class `EnteringACodeTests`: `test_long_discovery_name_keeps_room_and_full_name_tooltip_at_900_points` | 1 | ui | viewmodel | moved from viewmodel (#68): a long name in the pairing sheet's list is cut, with its whole text as tooltip |
| `mac_app/tests/test_map_ticks.py` | 22 | ui | viewmodel | the crossing map's thirds, badge, fit and seam; the tests are their geometry |
| `mac_app/tests/test_notices_mac.py` | 11 | platform | viewmodel | notification delivery (F36) is platform's; what a notice says is the viewmodel's |
| `mac_app/tests/test_pages.py`, class `DesignChoicesTest`: `test_the_notch_style_only_matters_under_glow_and_beam` | 1 | ui | viewmodel | moved from viewmodel (#68): the notch style under glow and beam (F31, Later) |
| `mac_app/tests/test_part_edge_thirds.py` | 9 | crossing | viewmodel | map hit bands and ledger segments share the saved third |
| `mac_app/tests/test_profiles_map_ledger.py`, class `ProfilesLedger`: `test_disabled_corner_chips_draw_solid_neutral_borders`, `test_disabled_edge_chips_draw_solid_neutral_borders_and_text` | 2 | ui | viewmodel | moved from viewmodel (#68): how disabled chips in the profiles' ledger are drawn |
| `mac_app/tests/test_profiles_map_ledger.py`, class `ProfilesMap`: `test_picker_draws_thirds_and_unlit_tracks_for_the_viewed_machine`, `test_picker_tiles_offer_pointing_hand_even_when_editable`, `test_tile_word_dots_use_signal_for_desk_and_own_only` | 3 | ui | viewmodel | moved from viewmodel (#68): how the profiles' map draws thirds, the pointer and the profile word's dot |
| `mac_app/tests/test_profiles_page.py`, class `ProfilePage`: `test_open_archive_fills_own_band_and_actions_align_at_the_right`, `test_profile_switch_hugs_and_tabs_mark_active_without_a_filled_add_button` | 2 | ui | viewmodel | moved from viewmodel (#68): the Profiles page's layout |
| `mac_app/tests/test_same_on_both.py` | 46 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `mac_app/tests/test_settings_v6.py`, class `OverlapTests` | 8 | crossing | settings | overlapping zones in the saved settings |
| `mac_app/tests/test_settings_v6.py`, class `PairedAndRemovedTests` | 19 | pairing | settings | the peers kept in the settings file |
| `mac_app/tests/test_sidebar_response.py` (its other tests) | 10 | viewmodel | link | which response time the sidebar shows |
| `mac_app/tests/test_sidebar_response.py`, class `SidebarResponseTests`: `test_full_response_value_wraps_inside_supported_sidebar_widths` | 1 | ui | viewmodel | moved from viewmodel (#68): the sidebar's response time wraps to fit the sidebar |
| `mac_app/tests/test_tile_resize_mac.py` | 13 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `mac_app/tests/test_update_config_redirecting.py` | 2 | driving | crossing | a settings reload while driving |
| `win_app/tests/test_admin_mode_ui.py` (its other tests) | 8 | viewmodel | platform | the admin mode controls and their diagnostics on the Input page |
| `win_app/tests/test_admin_mode_ui.py`, class `AdminControlTests`: `test_success_quits_only_after_helper_completes` | 1 | platform | viewmodel | moved from viewmodel (#68): a successful admin mode change ends Beamer only once it is done |
| `win_app/tests/test_admin_mode_ui.py`, class `ElevatedDiagnosticTests`: `test_medium_replacement_waits_for_high_instance_mutex` | 1 | app | viewmodel | moved from viewmodel (#68): a Beamer without administrator rights counts a running elevated Beamer as running (F37) |
| `win_app/tests/test_app_config.py`, class `AppearanceConfigTests` | 3 | ui | settings | the saved appearance choice |
| `win_app/tests/test_app_config.py`, class `EdgeGlowConfigTests` | 2 | ui | settings | the saved edge glow choice |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests` | 15 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_app_config.py`, class `ReadFallbackPeerBookTests` | 4 | pairing | settings | reading the peers from the settings file |
| `win_app/tests/test_atlas_canvas.py` (its other tests) | 18 | viewmodel | ui, crossing | the crossing map's canvas, as on the Mac |
| `win_app/tests/test_atlas_canvas.py`, class `AtlasCanvasTests`: `test_a_wrap_around_seam_stops_thirteen_pixels_short_of_a_corner_pad`, `test_drag_keeps_the_map_transform_until_release`, `test_internal_display_frames_are_drawn_only_for_multiple_displays`, `test_no_wrap_label_ever_lands_on_the_name_the_detail_line_or_another_label`, `test_this_pc_name_is_painted_in_a_single_display_tile`, `test_wrap_seams_follow_the_offline_and_not_selected_opacities` | 6 | ui | viewmodel | moved from viewmodel (#68): how the PC's crossing map draws wrap-around seams, display frames, names and labels, and keeps its scale during a drag |
| `win_app/tests/test_atlas_map_feel.py`, class `AtlasMapFeelTests`: `test_arrangement_box_centres_and_refits_after_a_peer_moves` | 1 | ui | viewmodel | moved from viewmodel (#68): the crossing map fits and centres every footprint |
| `win_app/tests/test_camera_section_win.py`, class `Module`: `test_keyboard_reaches_every_control` | 1 | ui | viewmodel | moved from viewmodel (#68): keyboard focus reaches every control of the Camera module |
| `win_app/tests/test_capture_portal.py` (its other classes) | 62 | input | crossing | Wayland capture, with the barriers that make zones there |
| `win_app/tests/test_capture_win_seam.py` | 6 | crossing | input | the capture hook's seam with the crossing engine |
| `win_app/tests/test_crossing_fix_pass.py` | 11 | ui | viewmodel | regressions on the Crossing page, mostly measured |
| `win_app/tests/test_crossing_ledger.py` (its other tests) | 11 | viewmodel | crossing | the ledger's controls on the Crossing page (F6) |
| `win_app/tests/test_crossing_ledger.py`, class `CrossingLedgerTests`: `test_corner_brackets_sit_centred_in_whole_squares`, `test_map_corner_pad_bracket_sits_centred_in_its_pad`, `test_narrow_has_no_horizontal_scroll_and_all_controls_fit` | 3 | ui | viewmodel | moved from viewmodel (#68): how the ledger's corner brackets and pads are drawn, and how it fits a narrow page |
| `win_app/tests/test_crossing_page_parity.py`, class `CrossingPageParityTests`: `test_ledger_controls_fit_at_640_and_900` | 1 | ui | viewmodel | moved from viewmodel (#68): the ledger's controls fit at 640 and 900 px |
| `win_app/tests/test_cursor_overlay_native.py` | 1 | ui | crossing | the drawn cursor overlay window (F16); ui's scope names its drawing |
| `win_app/tests/test_desktop_portal_cache.py` | 10 | input | crossing | Wayland input readers' display geometry |
| `win_app/tests/test_desktop_x11.py` | 22 | input | crossing | the X11 desktop's pointer, buttons and full-screen check |
| `win_app/tests/test_dormant_rows.py` (its other tests) | 8 | viewmodel | pairing, crossing | how a dormant peer's rows show |
| `win_app/tests/test_dormant_rows.py`, class `DormantCrossingTests`: `test_dormant_position_does_not_block_an_active_edge_snap`, `test_incoming_layout_moves_shelved_positions_but_keeps_local_intent` | 2 | crossing | viewmodel | moved from viewmodel (#68): inactive peers' footprints and zone choices as the crossing map changes |
| `win_app/tests/test_dormant_rows.py`, class `DormantCrossingTests`: `test_repair_keeps_label_layout_zones_and_left_out_after_reloading` | 1 | pairing | viewmodel | moved from viewmodel (#68): pairing again with a dormant peer keeps its label, footprint, zones and kept-off state |
| `win_app/tests/test_hook_key_repeat.py` | 8 | crossing | input | a held shortcut key's repeats in the hook |
| `win_app/tests/test_identify_screens_qt.py`, class `IdentifyPageTests`: `test_both_receive_links_ignore_phone_and_unknown_peer_before_overlay`, `test_button_identifies_local_and_all_capable_linked_peers`, `test_linked_shelved_peer_gets_all_its_display_numbers`, `test_local_button_and_both_receive_links_share_overlay_limit`, `test_stale_partial_labels_use_complete_live_numbering` | 5 | display | viewmodel | moved from viewmodel (#68): identify displays: which machines show numbers, the 3 s limit, and requests from machines that are not peers |
| `win_app/tests/test_idle_refresh.py` (its other tests) | 14 | viewmodel | runtime, ui | how often the window re-reads settings and redraws |
| `win_app/tests/test_idle_refresh.py`, class `IdleRefreshTest`: `test_display_geometry_change_is_reconciled_without_a_file_edit`, `test_display_signal_refreshes_hardware_before_reconciling` | 2 | crossing | viewmodel | moved from viewmodel (#68): this machine's displays on the crossing map follow a display change |
| `win_app/tests/test_ignored_page.py`, class `RecorderTests`: `test_hint_gets_the_whole_width_the_box_reserved_for_it`, `test_laid_out_keycap_has_room_for_its_key_and_hint` | 2 | ui | viewmodel | moved from viewmodel (#68): the key recorder's layout |
| `win_app/tests/test_key_up_after_switch_back.py` | 7 | input | driving | a key released after input came home still reaches the peer |
| `win_app/tests/test_links.py`, class `OnwardTests` | 16 | driving | crossing | pass on, when the driven machine's own zone fires |
| `win_app/tests/test_links.py`, class `SettingsTests` | 6 | settings | crossing | settings that change what the sender does |
| `win_app/tests/test_links_loopback.py` | 9 | driving | input | the PC's input to a real responder on loopback |
| `win_app/tests/test_map_ticks.py` | 20 | ui | viewmodel | the crossing map's thirds, badge, status dot, fit and seam; the tests are their geometry |
| `win_app/tests/test_pages_win.py`, class `CrossingPageTest`: `test_crossing_controls_and_diagram_stay_in_a_capped_column`, `test_the_native_titlebar_can_maximise_and_has_no_width_cap` | 2 | ui | viewmodel | moved from viewmodel (#68): the Crossing page's width cap and the maximisable window |
| `win_app/tests/test_pages_win.py`, class `CrossingPageTest`: `test_drop_to_a_gap_tells_the_peer_its_way_back_changed`, `test_incoming_layout_keeps_a_locally_held_paired_tile_when_missing`, `test_layout_drop_keeps_the_compiled_side_and_its_stamp`, `test_layout_drop_refusal_persists_the_animated_free_position`, `test_overlapping_drop_persists_an_accepted_free_snap`, `test_shared_layout_accepts_shelved_overlap_and_named_paired_third_party`, `test_shelved_position_does_not_block_a_drop_and_is_remembered` | 7 | crossing | viewmodel | moved from viewmodel (#68): drops, snap points and arriving crossing maps, checked through the PC's Crossing page |
| `win_app/tests/test_pages_win.py`, class `DesignControlsTest`: `test_a_switch_still_follows_the_crossing_style`, `test_a_switch_tile_plays_at_the_chosen_length`, `test_every_tile_draws_its_style_at_the_chosen_place_and_length` | 3 | ui | viewmodel | moved from viewmodel (#68): how the Design page's tiles draw their previews |
| `win_app/tests/test_pages_win.py`, class `DesignGroupsTest`: `test_every_effect_tile_names_its_intensity`, `test_only_the_new_effects_count_as_effects` | 2 | ui | viewmodel | moved from viewmodel (#68): the effect tiles' intensity (F30, Later) |
| `win_app/tests/test_pairing_window.py`, class `MachinesWindowTest`: `test_a_pairing_does_not_replace_a_machine_that_linked_since_it_was_read`, `test_a_pairing_does_not_replace_a_machine_that_was_removed_since_it_was_read`, `test_a_pairing_replaces_the_machine_it_was_meant_to_and_keeps_it_first`, `test_re_pairing_same_identity_keeps_intent_cycle_and_jump`, `test_the_paired_announcement_omits_machines_not_in_use` | 5 | pairing | viewmodel | moved from viewmodel (#68): which peer a pairing replaces, keeps or brings back, and the paired list |
| `win_app/tests/test_pairing_window.py`, class `MachinesWindowTest`: `test_pairing_a_machine_starts_sending_when_nothing_could_send_before`, `test_removing_the_only_machine_that_could_be_sent_to_stops_sending` | 2 | input | viewmodel | moved from viewmodel (#68): capture starts and stops as peers this machine may drive come and go |
| `win_app/tests/test_pairing_window.py`, class `MachinesWindowTest`: `test_with_nothing_paired_appearance_and_speed_still_save` | 1 | settings | viewmodel | moved from viewmodel (#68): appearance, pointer speed and reverse scroll save with no peer paired |
| `win_app/tests/test_pairing_window.py`, class `PairingSheetWindowTest`: `test_entered_code_ink_is_centred_in_the_code_box`, `test_pairing_code_text_is_centred_in_its_stretched_label` | 2 | ui | viewmodel | moved from viewmodel (#68): the code centred in its field and label |
| `win_app/tests/test_parity.py`, class `AlertAndWakeTests` | 4 | discovery | viewmodel | the alerts and waking the Mac when a switch has no link |
| `win_app/tests/test_part_edge_corners_qt.py` | 12 | crossing | viewmodel | map thirds, contact geometry and ledger parity |
| `win_app/tests/test_pc_three_machines.py`, class `CrossingPageTests`: `test_a_side_set_here_is_that_machines_alone_and_is_sent_to_it`, `test_an_overlapping_partial_tile_is_sent_to_a_free_snap`, `test_arrangement_keeps_the_return_way_holder_without_a_notice_block`, `test_drop_over_another_machine_saves_a_nonoverlapping_snap`, `test_dropping_tiles_on_separate_thirds_saves_and_tells_the_new_way_back`, `test_either_link_hands_on_way_back`, `test_every_link_up_says_whether_a_way_leads_to_that_machine`, `test_ledger_way_changes_tell_the_peer_its_current_return_way`, `test_moving_one_machine_tells_every_machine_whose_way_back_it_changed`, `test_nonselected_picture_arrangement_keeps_its_side_and_layout`, `test_offline_picture_peer_keeps_its_zone_and_reconnection_adds_no_glue`, `test_offline_picture_peer_keeps_its_zone_when_another_edge_is_edited`, `test_overlapping_drop_lands_only_at_an_accepted_free_position`, `test_picture_arrangement_keeps_layout_and_selects_its_ledger_row`, `test_picture_peer_return_way_changes_are_saved_without_an_answer`, `test_picture_peer_return_way_flag_updates_without_a_notice_block`, `test_removing_a_left_out_machine_keeps_the_remaining_crossing`, `test_removing_a_machine_that_shared_a_side_leaves_the_zones_as_a_fresh_compile`, `test_ways_written_here_reach_that_machines_zones_only` | 19 | crossing | viewmodel | moved from viewmodel (#68): return statuses, drops and derived zones with three machines, checked through the PC's Crossing page |
| `win_app/tests/test_profiles_canvas_qt.py`, class `ProfilesCanvasTests`: `test_profile_word_dot_uses_the_word_even_when_an_older_machine_is_linked`, `test_remote_map_does_not_draw_amber_contacts_between_other_machines` | 2 | ui | viewmodel | moved from viewmodel (#68): the profile word's dot and contact colours on the PC's profiles map |
| `win_app/tests/test_rig_map_rc13.py`, class `RigMapRc13`: `test_stacked_rig_arrangement_does_not_overlap` | 1 | crossing | viewmodel | moved from viewmodel (#68): peers placed by default do not overlap |
| `win_app/tests/test_same_on_both.py` | 54 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `win_app/tests/test_scroll_wheel_focus.py`, class `ScrollWheelFocusTest`: `test_all_settings_pages_protect_their_wheel_controls`, `test_click_focus_allows_wheel_on_custom_sliders`, `test_focused_controls_take_the_wheel_and_stop_after_focus_moves`, `test_native_wheel_does_not_acquire_focus_or_scroll_twice`, `test_slider_outside_scroll_area_keeps_its_wheel_behaviour`, `test_spin_box_editor_does_not_swallow_unfocused_wheel`, `test_tab_and_arrow_keys_still_edit_the_slider`, `test_touchpad_scroll_leaves_unfocused_slider_unchanged`, `test_unfocused_controls_scroll_the_dialog_instead_of_editing` | 9 | ui | viewmodel | moved from viewmodel (#68): the scroll wheel over an unfocused control scrolls the page instead |
| `win_app/tests/test_sender_motion_flow.py` | 8 | link | input, crossing | the sender batching motion before the link, keeping travel and barriers |
| `win_app/tests/test_settings_migration.py`, class `OverlapTests` | 7 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_style_for.py`, class `StyleForTest`: `test_hover_edge_is_neutral`, `test_selected_marker_is_in_tile_grab`, `test_style_families_stay_horizontal_and_wrap_at_spec_widths` | 3 | ui | viewmodel | moved from viewmodel (#68): tile colours and wrapping |
| `win_app/tests/test_tile_resize_qt.py` (its other tests) | 15 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `win_app/tests/test_tile_resize_qt.py`, class `TileResizeQtTests`: `test_machine_grip_does_not_cover_another_machines_split_display` | 1 | ui | viewmodel | moved from viewmodel (#68): the resize handle is not drawn over another machine's display |
| `win_app/tests/test_tray_icon.py`, class `TrayWindowTest`: `test_a_second_launch_raises_the_window_with_the_tray_off` | 1 | app | viewmodel | moved from viewmodel (#68): a second launch raises the running Beamer's window, also with Show in tray off (F37) |
| `win_app/tests/test_tray_icon.py`, class `TrayWindowTest`: `test_same_on_all_machines_leaves_it_alone` | 1 | settings | viewmodel | moved from viewmodel (#68): Show in tray stays this PC's own under Same on all machines |
| `win_app/tests/test_vcam_install.py` | 38 | camera | platform | installing the virtual camera; camera's scope names it |
