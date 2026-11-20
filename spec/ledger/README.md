# The ledger's owner domains

#51 laid the ledger out: every Python test at `archive/python`, 6,232 of them, in exactly one
domain's slice, `spec/ledger/<domain>.txt`, with no fate yet. The format and the check are in
[`spec/README.md`](../README.md#the-python-ledger). Each domain's miner gives its slice's tests their
fates and adds or removes no line (the `build-spec-miner` skill).

The domains are the twenty "Spec: <domain>" tasks under #24. Every domain has a slice, empty where
no Python test is its own:

| Domain | Task | Tests |
|---|---|---|
| `link` | #52 | 325 |
| `pairing` | #53 | 295 |
| `store` | #54 | 4 |
| `discovery` | #55 | 124 |
| `driving` | #56 | 328 |
| `crossing` | #57 | 1144 |
| `input` | #58 | 793 |
| `clipboard` | #59 | 216 |
| `settings` | #60 | 368 |
| `display` | #61 | 31 |
| `audio` | #62 | 148 |
| `camera` | #63 | 202 |
| `media` | #64 | 0 |
| `platform` | #65 | 336 |
| `runtime` | #66 | 20 |
| `app` | #67 | 93 |
| `viewmodel` | #68 | 1064 |
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
There are 136, covering 1,288 tests. Each row names the other candidates and why the owner won.
A miner who finds such a test's assertions are all another domain's settles it as
`.claude/skills/build-spec-miner/blocked.md` says, without moving the line.

46 rows record lines the settings miner (#60) moved, after #51: their assertions are all
another domain's, so they went to that domain's slice with no fate, as #147 decides. Most split a
class, an exception to rule 2's "no assignment finer than a class": the class's other tests stay in
`settings`.

| Tests | Count | Owner | Also a candidate | Why the owner |
|---|---|---|---|---|
| `core/tests/settings_behaviour.py`, class `SettingsBehaviourTests`: `test_compatible_design_not_yet_received_is_not_labelled_old`, `test_connected_machine_with_design_is_offered_whatever_offline_peers_say`, `test_following_offline_machine_keeps_last_design_and_says_so`, `test_no_connected_machine_lists_none_with_one_quiet_line` | 4 | viewmodel | settings | moved from settings (#60): "Match design with" lists a peer whose design has not arrived as one that cannot be chosen yet, with its tooltip and note (viewmodel.design.follow-unavailable); "Match design with" offers a linked peer whose design arrived, whatever peers with no link say (viewmodel.design.follow-choices, viewmodel.design.follow-note); the note while the followed peer has no link (viewmodel.design.follow-note); "Match design with" with no peer linked: None alone and one note (viewmodel.design.follow-choices, viewmodel.design.follow-note) |
| `core/tests/settings_behaviour.py`, class `SettingsBehaviourTests`: `test_recording_a_trigger_clash_preserves_the_local_trigger_and_shows_a_note` | 1 | crossing | settings | moved from settings (#60): recording a shortcut key that is in a jump key's chord is refused and the key kept (crossing.shortcut.key-jump-clash) |
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
| `core/tests/test_settings_sync.py`, class `FollowRowsTests`: `test_nothing_connected_is_one_quiet_line`, `test_only_connected_machines_are_listed_and_away_ones_never_set_the_note`, `test_the_followed_machine_stays_listed_while_away` | 3 | viewmodel | settings | moved from settings (#60): "Match design with" with no peer linked: no row and one note (viewmodel.design.follow-note); "Match design with" lists only linked peers, and a peer with no link sets no note (viewmodel.design.follow-choices, viewmodel.design.follow-note); "Match design with" keeps listing the followed peer while it has no link (viewmodel.design.follow-choices) |
| `core/tests/test_settings_sync.py`, class `MessageTests`: `test_everything_expands_to_all_four_groups` | 1 | viewmodel | settings | moved from settings (#60): ticking Everything asks for all four groups (viewmodel.settings.tick) |
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
| `mac_app/tests/test_mac_listener_port.py` | 4 | discovery | link | the port the listener and the beacon use |
| `mac_app/tests/test_mac_loopback.py` | 12 | driving | input, clipboard | the Mac driving a PC and driven by it over loopback, end to end |
| `mac_app/tests/test_mac_three.py` (its other classes) | 18 | crossing | driving | a push through a zone takes the machine the zone leads to, with three machines |
| `mac_app/tests/test_mac_to_mac.py` | 4 | input | driving | two Macs on loopback; both classes check which keys arrive |
| `mac_app/tests/test_mac_wire.py` | 12 | driving | input, link | the Mac's controller against a real responder, end to end |
| `mac_app/tests/test_map_ticks.py` | 22 | ui | viewmodel | the crossing map's thirds, badge, fit and seam; the tests are their geometry |
| `mac_app/tests/test_notices_mac.py` | 11 | platform | viewmodel | notification delivery (F36) is platform's; what a notice says is the viewmodel's |
| `mac_app/tests/test_part_edge_thirds.py` | 9 | crossing | viewmodel | map hit bands and ledger segments share the saved third |
| `mac_app/tests/test_same_on_both.py` | 46 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `mac_app/tests/test_same_on_both.py`, class `SameOnBothTests`: `test_an_unpaired_source_cannot_change_or_forward_settings` | 1 | link | settings | moved from settings (#60): a machine that is not a peer changes nothing here and has nothing sent on for it (only peers link, link.auth) |
| `mac_app/tests/test_same_on_both.py`, class `SameOnBothTests`: `test_arriving_settings_leave_a_half_typed_address_and_the_pairing_card`, `test_everything_can_be_turned_off`, `test_follow_picker_recovers_when_a_peer_announces_design`, `test_full_screen_hold_switch_names_this_macs_edges` | 4 | viewmodel | settings | moved from settings (#60): settings taken from a peer leave a half-typed address and the pairing sheet's words as they are; unticking Everything asks for no group (viewmodel.settings.tick); a peer whose design arrives becomes choosable in "Match design with" (viewmodel.design.follow-follows); the words of the full-screen pause switch (viewmodel.settings.own-row) |
| `mac_app/tests/test_same_on_both.py`, class `SameOnBothTests`: `test_the_hold_switch_reaches_the_controller_before_the_debounce` | 1 | crossing | settings | moved from settings (#60): turning the full-screen pause off or on applies at once, before the settings are written |
| `mac_app/tests/test_settings_store.py`, class `LegacyPortTests`: `test_both_ports_are_below_the_range_the_system_hands_out` | 1 | link | settings | moved from settings (#60): the one port 24820 (links, pairing, beacons) is below the ephemeral range (link.transport.port) |
| `mac_app/tests/test_settings_store.py`, class `SettingsStoreTests`: `test_an_old_peer_without_a_jump_key_defaults_to_none`, `test_jump_key_round_trips_and_rejects_non_strings`, `test_trigger_clash_is_refused_without_changing_the_saved_value_and_can_be_cleared` | 3 | crossing | settings | moved from settings (#60): a peer with no jump key saved has none (crossing.jump-key.clear); a jump key is kept, a value not spelled as a chord is refused (crossing.jump-key.spelling); a jump key using the shortcut key is refused, the old one kept, an empty one clears it (crossing.jump-key.shortcut-clash, crossing.jump-key.clear) |
| `mac_app/tests/test_settings_store.py`, class `SettingsStoreTests`: `test_both_directions_default_on_and_each_switch_round_trips`, `test_in_use_defaults_on_round_trips_and_rejects_non_booleans` | 2 | pairing | settings | moved from settings (#60): a peer's may drive / may be driven by default on and are kept apart (pairing.peers.entry, pairing.peers.permissions); a peer is in use after pairing, the choice is kept, a non-on/off value is refused (pairing.peers.entry) |
| `mac_app/tests/test_settings_store.py`, class `SettingsStoreTests`: `test_editing_a_learnt_host_pins_it_and_survives_reload`, `test_explicitly_saving_the_same_learnt_host_pins_it`, `test_unpin_survives_flat_saves_and_reload` | 3 | discovery | settings | moved from settings (#60): typing an address makes it a chosen address, kept after a restart (discovery.address.choose, discovery.address.saved); choosing an address the peer already learnt makes it chosen (discovery.address.choose-learnt); removing a chosen address leaves it learnt, and that survives later saves and a restart (discovery.address.unchosen, discovery.address.saved) |
| `mac_app/tests/test_settings_store.py`, class `SettingsStoreTests`: `test_save_is_private_and_round_trips_shared_schema` | 1 | runtime | settings | moved from settings (#60): a data file is mode 0600 and loads back what was written (runtime.files.mac-private, runtime.files.round-trip) |
| `mac_app/tests/test_settings_sync_link.py`, class `AnnouncedOnLinkTests`: `test_the_arrangement_is_announced_when_a_link_comes_up` | 1 | crossing | settings | moved from settings (#60): the crossing map changed while no link was up reaches the peer at the next link |
| `mac_app/tests/test_settings_v6.py`, class `MigrationTests`: `test_config_json_is_never_written_again` | 1 | runtime | settings | moved from settings (#60): the Python app's config.json is never written (runtime.files.fresh-start) |
| `mac_app/tests/test_settings_v6.py`, class `OverlapTests` | 8 | crossing | settings | overlapping zones in the saved settings |
| `mac_app/tests/test_settings_v6.py`, class `PairedAndRemovedTests` | 19 | pairing | settings | the peers kept in the settings file |
| `mac_app/tests/test_settings_v6.py`, class `RefusingToStartOverTests`: `test_a_file_without_a_machine_id_is_refused_and_never_given_one`, `test_a_file_without_peers_or_zones_is_refused` | 2 | store | settings | moved from settings (#60): a store's file without a well-formed machine id is refused whole and left as it is, never rewritten with a new id (store.file.refused); a store's file not in the store's form is refused (store.file.refused) |
| `mac_app/tests/test_settings_v6.py`, class `RefusingToStartOverTests`: `test_a_future_schema_is_refused_on_save_and_left_as_it_is` | 1 | runtime | settings | moved from settings (#60): a data file of a version this build does not read is refused and no write replaces it (runtime.files.refused-defaults, runtime.files.refused-kept) |
| `mac_app/tests/test_settings_v6.py`, class `ReimportRefusalsTests`: `test_a_zone_with_a_bad_corner_or_an_unknown_peer_is_refused` | 1 | crossing | settings | moved from settings (#60): saved zones that cannot be read (Rust saves none: zones are derived, crossing.map.saved.derived); crossing's miner will likely drop it |
| `mac_app/tests/test_settings_v6.py`, class `SavingTests`: `test_a_first_save_with_nothing_on_disk_makes_a_machine`, `test_saving_keeps_the_machine_id_and_the_id_of_the_peer_and_writes_privately` | 2 | runtime | settings | moved from settings (#60): with no data file on disk, the first start creates it holding the defaults (runtime.files.first-run); a save keeps what it did not change, and the file and folder are 0600 and 0700 (runtime.files.mac-private, runtime.files.round-trip) |
| `mac_app/tests/test_settings_v6.py`, class `SavingTests`: `test_a_flat_save_keeps_a_name_and_hardware_address_learnt_since_the_window_read`, `test_a_new_address_clears_what_was_learnt_at_the_old_one` | 2 | discovery | settings | moved from settings (#60): a settings change saved from an older read never undoes a learnt name or hardware address (discovery.address.concurrent-save, its other direction); typing a new address for a peer drops the hardware address and name learnt at the old one (discovery.hardware.of-peer) |
| `mac_app/tests/test_settings_v6.py`, class `SavingTests`: `test_a_machine_paired_with_no_zone_gets_its_edge_when_the_settings_are_read`, `test_a_side_argument_cannot_move_the_picture_or_disable_edge_intent`, `test_a_tie_won_on_the_larger_by_stores_the_winners_by`, `test_an_arrangement_for_one_machine_leaves_the_others_side_alone`, `test_an_arrangement_from_the_peer_is_stamped_with_the_peers_id_not_this_machines`, `test_changing_the_methods_turns_zones_on_and_off`, `test_editing_ways_saves_intent_without_moving_the_picture_or_writing_off_flags`, `test_loading_a_taken_side_keeps_edge_intent_without_off_flags_or_a_share_offer`, `test_the_corner_zones_edge_follows_the_corner_not_the_side`, `test_the_side_stamp_keeps_its_author_until_it_changes`, `test_trigger_changes_cannot_overwrite_an_existing_jump_key`, `test_ways_for_a_machine_no_longer_paired_are_refused` | 12 | crossing | settings | moved from settings (#60): a peer with no zone choices gets the default choices and its edge (crossing.derive.choices.default, crossing.derive.first-zone); a change of zone choices never moves the crossing map (crossing.derive.choices.one-peer); of two changes to where a peer sits, the newer wins, a tie by the greater machine id (crossing.map.shared.newest; crossing.map.place.chosen-edge's parity); a change to one peer's place leaves the others' (crossing.map.shared.newest); a peer's edge kept with the stamp and id of the machine that set it (crossing.map.place.chosen-edge's parity); turning a peer's ways on and off derives and removes its zones (crossing.derive.edge.*, crossing.derive.notch.*); a change of one peer's zone choices moves no footprint and keeps the others' (crossing.derive.choices.one-peer); a peer on a side another peer fills keeps its edge chosen (crossing.derive.choices.kept, crossing.derive.offer.*); the edge a corner zone crosses through (crossing.zone.corner-edge, whose parity is this Mac behaviour); a shortcut key in a jump key's chord is refused and nothing is written (crossing.shortcut.key-jump-clash); a change of zone choices for a machine that is not a peer is refused (crossing.derive.choices.unknown-peer) |
| `mac_app/tests/test_settings_v6.py`, class `SavingTests`: `test_each_shared_values_stamp_is_saved_and_a_malformed_one_is_dropped` | 1 | store | settings | moved from settings (#60): each shared entry's stamp is kept in the store's file; a malformed stamp refuses the file (store.file.kept, store.file.refused) |
| `mac_app/tests/test_settings_v6.py`, class `SavingTests`: `test_the_store_serves_the_settings_it_holds_to_a_peer_book_and_keeps_them_after_a_save` | 1 | pairing | settings | moved from settings (#60): the peer list the links read follows each save, and a name a link learns is kept (pairing.peers.concurrent-save; the 1.4 entry part is dropped) |
| `mac_app/tests/test_settings_v6.py`, class `ValidationTests`: `test_a_bad_machine_id_is_refused` | 1 | store | settings | moved from settings (#60): a store's file with a malformed machine id is refused (store.file.refused); the id itself is pairing.identity.machine-id |
| `mac_app/tests/test_settings_v6.py`, class `ValidationTests`: `test_an_unknown_schema_is_refused_rather_than_migrated_over` | 1 | runtime | settings | moved from settings (#60): a data file of a version this build does not read is refused (runtime.files.refused-defaults) |
| `mac_app/tests/test_settings_v6.py`, class `ValidationTests`: `test_more_than_thirty_two_peers_are_refused`, `test_two_entries_with_one_id_or_our_own_id_are_refused` | 2 | pairing | settings | moved from settings (#60): a machine has at most 32 peers (pairing.capacity.limit); no two peers share a machine id, and no peer has this machine's id (pairing.identity.machine-id; store.file.refused) |
| `mac_app/tests/test_settings_v6.py`, class `ZonesFromMethodsTests`: `test_a_corner_crosses_by_its_own_left_or_right_edge_whatever_the_side_as_1_4_x_did` | 1 | crossing | settings | moved from settings (#60): the Mac's corner crossed through its left or right edge whatever the side (crossing.zone.corner-edge's parity) |
| `mac_app/tests/test_sidebar_response.py` | 11 | viewmodel | link | which response time the sidebar shows |
| `mac_app/tests/test_tile_resize_mac.py` | 13 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `mac_app/tests/test_unreadable_settings.py`, class `UnreadableSettingsTests`: `test_a_save_does_not_replace_it`, `test_load_reports_it_and_leaves_the_file` | 2 | runtime | settings | moved from settings (#60): a refused data file is not written over by any save (runtime.files.refused-kept); a data file of a later version is refused at load and left as it is (runtime.files.refused-defaults, runtime.files.refused-kept) |
| `mac_app/tests/test_update_config_redirecting.py` | 2 | driving | crossing | a settings reload while driving |
| `win_app/tests/test_admin_mode_ui.py` | 10 | viewmodel | platform | the admin mode controls and their diagnostics on the Input page |
| `win_app/tests/test_app_config.py`, class `AppearanceConfigTests` | 3 | ui | settings | the saved appearance choice |
| `win_app/tests/test_app_config.py`, class `EdgeGlowConfigTests` | 2 | ui | settings | the saved edge glow choice |
| `win_app/tests/test_app_config.py`, class `HandEditedListsTests`: `test_an_unhashable_entry_is_a_config_error_not_a_crash` | 1 | runtime | settings | moved from settings (#60): whatever a hand-edited data file holds is loaded or refused, never a crash (runtime.files.any-bytes) |
| `win_app/tests/test_app_config.py`, class `LegacyPortTests`: `test_a_new_config_starts_on_the_new_port` | 1 | link | settings | moved from settings (#60): every machine takes links on 24820 (link.transport.port) |
| `win_app/tests/test_app_config.py`, class `OverlappingZonesTests` | 15 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_app_config.py`, class `PeerAddressConfigTests`: `test_editing_a_learnt_host_pins_it_and_survives_reload`, `test_explicitly_saving_the_same_learnt_host_pins_it` | 2 | discovery | settings | moved from settings (#60): typing an address makes it a chosen address, kept after a restart (discovery.address.choose, discovery.address.saved); choosing an address the peer already learnt makes it chosen (discovery.address.choose-learnt) |
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
| `win_app/tests/test_links.py`, class `OnwardTests` | 16 | driving | crossing | pass on, when the driven machine's own zone fires |
| `win_app/tests/test_links.py`, class `SettingsTests` | 6 | settings | crossing | settings that change what the sender does |
| `win_app/tests/test_links.py`, class `SettingsTests`: `test_a_clipboard_is_handed_to_the_owner_as_rebuilt_from_what_was_read`, `test_a_clipboard_set_by_a_peer_reaches_the_clipboard_only_from_a_machine_just_let_go`, `test_a_clipboard_with_a_text_that_is_not_a_string_costs_nothing` | 3 | clipboard | settings | moved from settings (#60): a clipboard message is passed on rebuilt from its readable fields only; a peer's clipboard is set here only from the machine just let go, once; a clipboard message with a non-text text is ignored, nothing sent or closed |
| `win_app/tests/test_links.py`, class `SettingsTests`: `test_a_machine_without_an_id_drives_nothing` | 1 | driving | settings | moved from settings (#60): a machine with no machine id neither drives nor takes keys (likely python-detail there: a Rust machine always has its id, pairing.identity.machine-id) |
| `win_app/tests/test_links.py`, class `SettingsTests`: `test_the_links_are_told_the_peers_whenever_they_are_read` | 1 | link | settings | moved from settings (#60): the links follow the peer list each time it is read (link.dial.every-peer); python-detail is the alternative (a mock's sync call) |
| `win_app/tests/test_links_loopback.py` | 9 | driving | input | the PC's input to a real responder on loopback |
| `win_app/tests/test_map_ticks.py` | 20 | ui | viewmodel | the crossing map's thirds, badge, status dot, fit and seam; the tests are their geometry |
| `win_app/tests/test_parity.py`, class `AlertAndWakeTests` | 4 | discovery | viewmodel | the alerts and waking the Mac when a switch has no link |
| `win_app/tests/test_part_edge_corners_qt.py` | 12 | crossing | viewmodel | map thirds, contact geometry and ledger parity |
| `win_app/tests/test_same_on_both.py` | 54 | settings | viewmodel | Same on all machines through the real window; its assertions are the sync's |
| `win_app/tests/test_same_on_both.py`, class `SameOnBothTest`: `test_design_follow_label_refreshes_when_hide_addresses_changes`, `test_design_follow_refreshes_rows_when_peer_capabilities_change`, `test_everything_can_be_turned_off`, `test_full_screen_hold_switch_names_this_pcs_edges` | 4 | viewmodel | settings | moved from settings (#60): "Match design with" names peers anew when hiding addresses changes (viewmodel.design.follow-follows); "Match design with" follows the peers whose links come and go (viewmodel.design.follow-follows); unticking Everything asks for no group (viewmodel.settings.tick); the words of the full-screen pause switch (viewmodel.settings.own-row) |
| `win_app/tests/test_sender_motion_flow.py` | 8 | link | input, crossing | the sender batching motion before the link, keeping travel and barriers |
| `win_app/tests/test_settings_migration.py`, class `ConfigViewTests`: `test_this_pcs_address_is_read_live_not_saved` | 1 | discovery | settings | moved from settings (#60): this machine's own address is read from the network, never saved (discovery.own-address.offered) |
| `win_app/tests/test_settings_migration.py`, class `MigrationTests`: `test_an_id_of_sixteen_zero_bytes_is_drawn_again`, `test_peer_in_use_must_be_a_boolean`, `test_the_machine_id_is_sixteen_random_bytes_in_b64_and_never_zero` | 3 | pairing | settings | moved from settings (#60): Python's random machine id, never all zeros (pairing.identity.machine-id's parity); a peer's in use is on or off (pairing.peers.entry; store.file.refused); Python's random machine id (pairing.identity.machine-id's parity) |
| `win_app/tests/test_settings_migration.py`, class `MigrationTests`: `test_jump_key_clash_is_refused_without_saving_and_can_be_cleared`, `test_jump_key_must_be_a_recorded_chord_and_round_trips`, `test_load_rejects_a_malformed_jump_key` | 3 | crossing | settings | moved from settings (#60): a jump key using the shortcut key is refused, nothing saved, an empty one clears it (crossing.jump-key.shortcut-clash, crossing.jump-key.clear); a jump key is kept, and a value not written as a chord is refused (crossing.jump-key.spelling); a modifier alone is not a chord (crossing.jump-key.chord, crossing.jump-key.spelling) |
| `win_app/tests/test_settings_migration.py`, class `MigrationTests`: `test_the_old_file_is_kept_byte_for_byte_and_its_time_unchanged`, `test_the_paths_are_settings_json_and_the_old_file_beside_it` | 2 | runtime | settings | moved from settings (#60): the Python app's config.json is never written (runtime.files.fresh-start); the data files' names and folder (runtime.files.folder, whose parity records settings.json) |
| `win_app/tests/test_settings_migration.py`, class `NoOldFileTests`: `test_a_missing_config_gives_defaults_and_no_peer`, `test_a_settings_file_for_another_schema_is_refused_and_left_alone` | 2 | runtime | settings | moved from settings (#60): with no data file, the first start creates it holding the defaults (runtime.files.first-run); a data file of a version this build does not read is refused (runtime.files.refused-defaults) |
| `win_app/tests/test_settings_migration.py`, class `OverlapTests` | 7 | crossing | settings | overlapping zones in the saved settings |
| `win_app/tests/test_settings_migration.py`, class `ReadingAnOddFileTests`: `test_a_file_the_app_cannot_use_is_a_settings_error_with_the_path_and_is_left_alone` | 1 | runtime | settings | moved from settings (#60): a refused data file is left as it is through loads and saves (runtime.files.refused-kept; store.file.refused for the peers and zones cases) |
| `win_app/tests/test_settings_migration.py`, class `SavingTests`: `test_a_save_is_atomic_a_new_file_renamed_over_the_old` | 1 | runtime | settings | moved from settings (#60): a failed write leaves the previous file whole and no temporary file (runtime.files.whole, runtime.files.write-failed) |
| `win_app/tests/test_settings_migration.py`, class `SavingTests`: `test_the_machine_id_and_the_migrated_hash_never_change_in_a_save` | 1 | pairing | settings | moved from settings (#60): a save never changes this machine's machine id (pairing.identity.machine-id; the migrated hash is dropped with F19's migration) |
| `win_app/tests/test_settings_migration.py`, class `SavingTests`: `test_trigger_changes_cannot_overwrite_an_existing_jump_key`, `test_turning_ways_off_removes_zones_and_keeps_choices_for_turning_on`, `test_way_edits_and_flat_saves_keep_the_held_side_and_author` | 3 | crossing | settings | moved from settings (#60): a shortcut key in a jump key's chord is refused and nothing is written (crossing.shortcut.key-jump-clash); turning a peer's ways off removes its zones and keeps its thirds and corner for turning on (crossing.derive.edge.off-keeps-thirds, crossing.derive.choices.kept); a peer's edge kept with the stamp and id of the machine that set it (crossing.map.place.chosen-edge's parity) |
| `win_app/tests/test_settings_migration.py`, class `ZonesFromMethodsTests`: `test_a_corner_crosses_the_side_when_it_touches_it_and_its_own_horizontal_edge_otherwise`, `test_a_corner_with_no_side_yet_crosses_its_own_left_or_right_edge`, `test_there_is_no_notch_zone_on_windows` | 3 | crossing | settings | moved from settings (#60): a chosen corner crosses through the edge the peer touches when it is one of the corner's, else its left or right edge (crossing.zone.corner-edge); a corner with no edge touched crosses through its left or right edge (crossing.zone.corner-edge); only a Mac has a notch zone (crossing.derive.notch.mac) |
| `win_app/tests/test_tile_resize_qt.py` | 16 | viewmodel | crossing, ui | resizing a display's tile on the crossing map |
| `win_app/tests/test_unreadable_settings.py`, class `UnreadableSettingsTests`: `test_the_file_is_untouched_by_launch_and_by_every_writer`, `test_the_setting_toggles_do_not_write_over_it` | 2 | runtime | settings | moved from settings (#60): a refused data file is not written over at launch or by any write (runtime.files.refused-kept); no change writes a refused data file (runtime.files.refused-kept) |
| `win_app/tests/test_unreadable_settings.py`, class `UnreadableSettingsTests`: `test_the_message_says_what_happened_and_how_to_recover`, `test_window_opens_and_the_crossing_page_builds` | 2 | viewmodel | settings | moved from settings (#60): the window says the settings file is from a newer Beamer and was left as it is (viewmodel.pages.dot-settings is the nearest rule; no rule states the words); the window and its Crossing page open while the settings file is refused (no rule names it; nearest viewmodel.pages.dot-settings) |
| `win_app/tests/test_vcam_install.py` | 38 | camera | platform | installing the virtual camera; camera's scope names it |
