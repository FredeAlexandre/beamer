# Feature inventory of Beamer 1.5.19 plus the fork

Research for the wayfinder ticket [Feature inventory of Beamer 1.5.19 plus the fork](https://github.com/FredeAlexandre/beamer/issues/3),
on the map [Wayfinder: port Beamer to Rust](https://github.com/FredeAlexandre/beamer/issues/1).

**Question.** What does Beamer do today, feature by feature and surface by surface, so that nothing is ported or dropped silently?

**Scope and method.** Commit `fffbede` on `main` (upstream 1.5.19 plus the fork's audio, camera and shared displays).
Four readers swept `core/`, `mac_app/`, `win_app/` and everything else (`native/`, `tools/`, `scripts/`, `dev/`,
`locales/`, assets, packaging), one each. Every non-test `.py` file sits in exactly one feature row of its area, and
the line totals match `wc -l`. Test counts are `grep -c "def test_"`. **No keep or drop judgement is made here.** That is
the job of [Keep or drop: which features the Rust app ships with](https://github.com/FredeAlexandre/beamer/issues/11).

## Size at a glance

| Area | Lines (non-test) | Files | Tests (`def test_`) | Notes |
|---|---|---|---|---|
| `core/` | 27,656 | 61 | 2,205 | 90 of those tests exercise app code, not core |
| `mac_app/` | 26,363 | 37 | 1,842 | AppKit through PyObjC, plus rumps for the menu bar. No Qt |
| `win_app/` Windows + shared | 25,910 | 62 | about 1,810 | PySide6 |
| `win_app/` Linux-only | 6,271 | 18 | 357 | Shipped inside the Windows exe today |
| Root `theme.py` + `tokens.py` | 421 | 2 | (in app suites) | `win_app/tokens.py` is a byte copy |
| `native/` Beamer-written | 41,266 | | C++ suites + Python drivers | 37,241 C/C++/ObjC, 1,891 Python drivers |
| `native/` vendored | 127,579 | 370 | | Opus 1.6.1, nlohmann/json 3.11.3, speexdsp 1.2.1, Monocypher 4.0.2 |
| `tools/` + `scripts/` (Python) | 6,830 | | | Build probes, i18n, phone vectors, UI audit, Linux proofs |

There is no CI: `.github/` holds only issue templates.

## Feature index (cross-cutting)

One row per user-visible or operational feature, with where it lives in each area. Sizes are rough: a feature inside
one of the monolith files (`mac_app/kvm_bridge_app.py` at 5,935 lines, `mac_app/bridge.py` at 3,250,
`win_app/kvm_bridge_win.py` at 6,264) is marked "in shell", and the detailed sections below give line ranges. "Fork"
marks code that upstream does not have.

| # | Feature | core | mac_app | win_app (Windows) | native | Tests (rough) |
|---|---|---|---|---|---|---|
| F1 | **Link and wire protocol v6**: encrypted framed sessions, ping and keepalive, input ack and backpressure, capabilities, endpoint follow, v5 refusal | `protocol.py`, `link.py`, `link_timing.py`, `receiver.py`, `response_time.py` (≈4.6k) | `bridge.py` links + `windows_input.py`, `desktop_mac.py` (in shell + 0.3k) | `sender.py` (2.0k, shared with F4) | n/a | ≈500 core + mac/win link suites |
| F2 | **Pairing**: CPace on a 6-digit code, pairing by address, LAN beacon discovery, dormant repair, peer list | `pairing.py`, `peerlist.py` (2.8k) | `machines_panel.py`, `machines.py`, `widgets.CodeBoxes` (1.1k) | `machines_win.py` (1.0k) + shell 4126-4764 | n/a | ≈250 core, ≈130 mac, ≈80 win |
| F3 | **Pair nearby** (easy pair, SAS confirm). Labelled experimental but live | `easy_pair.py` (0.2k) | in pairing sheet | in pairing sheet | n/a | 22 core |
| F4 | **Input capture and injection**: keys, mouse, scroll, buttons, text; injected-event marking; release-all | `capture_types.py` | CGEventTap in `bridge.py`, `input_injector_mac.py` (0.6k) | `capture_win.py` (LL hooks + Raw Input, 1.1k), `input_injector.py` (0.8k), `platform_parts.py` | n/a | ≈30 mac, ≈200 win |
| F5 | **Edge push, resistance, crossing methods** (edge, thirds, corner, notch, shortcut), arrival grace, drag protection | `return_edge.py`, `resistance_strip.py`, `owner.py`, `crossing_state.py` (2.2k) | `crossing.py` (0.7k) | in `capture_win.py` / `sender.py` | n/a | ≈230 core, 111 mac |
| F6 | **Crossing map** (atlas: tiles, displays, zones, thirds, corners, resize) and **ledger** | `ways.py` (1.7k) | `diagram.py` (2.3k), `ledger.py` (0.8k) | `diagram.py` (1.8k), `crossing_ledger.py` (0.9k) | n/a | 243 core, ≈180 mac, ≈130 win |
| F7 | **Crossing profiles**: named sets of ways, desk merge, "same ways on every machine" | `profiles.py` (0.9k) | shell | shell | n/a | 110 core + UI suites |
| F8 | **Shortcut and jump keys**: double-tap or hold, cycle order, per-machine jump key | in `ways.py` | `widgets.KeyRecorder`/`JumpKeyRecorder`, `bridge.py` | `capture_win.Trigger`, shell | n/a | in F6 and shell suites |
| F9 | **Shake to come home** | `shake.py` (0.2k) | `bridge.py` | `sender.py`, `shake_stroke.py` | n/a | 27 core, 15 mac, 12 win |
| F10 | **Keys and layouts**: wire key table, positional vs semantic modifiers, dead keys, special system keys (Spotlight and app switcher) | `keytable.py`, `special_keys.py` (0.2k) | `keyboard_layout.py` (UCKeyTranslate), `key_codes.py` (0.4k) | in capture and injector (ToUnicodeEx, VkKeyScanW) | n/a | ≈50 core, 32 mac, many win |
| F11 | **Media and brightness keys** | n/a | `media_keys.py` | `brightness_worker.py`, `display_brightness.py` (WMI, built-in panel only) | n/a | small |
| F12 | **Trackpad gestures** across machines | `gesture` message | `gestures.py` (0.3k) + invisible GestureOverlay panel | `touchpad_injector.py` (synthetic Precision Touchpad, Win 11) | n/a | 32 mac, 5 win |
| F13 | **Ignored inputs** ("Stays on this machine") | `ignored.py` | `ignored_titles.py` | shell | n/a | ≈50 |
| F14 | **Mouse only, pointer speed, scroll scaling, reverse scroll** | `receiver.InputScale` | shell | shell | n/a | small |
| F15 | **Clipboard**: text and PNG both ways, visit stamps, size caps | in `protocol.py`, `receiver.py`, `owner.py` | `clipboard_mac.py` (0.2k) | `clipboard_win.py` (0.4k) | n/a | ≈25 core, 24 mac, 34 win |
| F16 | **Pointer hiding**: hide while driving, idle hide when driven, the Windows veil, a cursor overlay for a mouseless PC | `idle_pointer.py` | `pointer_hide.py` (private CGS) | `desktop_win.py` (SetSystemCursor), `cursor_overlay.py`, veil in shell | n/a | ≈60 |
| F17 | **Full-screen hold** (no crossing while a full-screen app is in front) | n/a | shell (AXFullScreen) | `desktop_win.py` (SHQueryUserNotificationState) | n/a | small |
| F18 | **Settings sync** ("Same on all machines") and **design follow** | `settings_sync.py` (0.9k) | shell | shell | n/a | 133 core + UI |
| F19 | **Settings store and 1.4.x migration** | `peerlist.normalise_phones` | `settings_store.py`, `config.py` (1.2k) | `app_config.py` (1.2k) | n/a | 134 mac, 126 win |
| F20 | **Wake-on-LAN** and the hardware address in the handshake | `wol.py` | `wake.py`, `hardware_mac.py` | `hardware_win.py`, `sender.py` | n/a | 36 mac, 10 win, 0 core |
| F21 | **Windows lock-screen unlock** through an external credential provider (`Global\RoomDeckUnlock.*`, not in the repo) | `receiver.py` hook | `no_unlock.py` stub | `unlock_win.py` (0.2k) | n/a | 11 |
| F22 | **Update check** (notice only, never downloads) | `updates.py` (0.2k) | shell | shell | n/a | 36 core |
| F23 | **Portable build** (run from a folder, passphrase-encrypted store, input-controller lease) | `portable_runtime.py`, `portable_store.py` (0.3k) | n/a | `portable_entry.py`, `hooks/`, `build_portable.*`, vault in shell | n/a | ≈60 |
| F24 | **Admin mode** ("Control administrator windows", elevated relaunch) | n/a | n/a | `admin_mode.py` (0.3k) | n/a | 45 |
| F25 | **Firewall check and repair** | n/a | n/a | `firewall_win.py` (0.4k) | n/a | 57 |
| F26 | **Start at login** | n/a | `login_item.py` (SMAppService) | `autostart_win.py` (scheduled task) | n/a | 5 mac, 31 win |
| F27 | **Dock, menu bar and tray** presence and glyph | n/a | `dock_visibility.py`, menu-bar glyph in shell | `tray_glyph.py` | n/a | ≈40 |
| F28 | **Identify screens** (numbers on every display for 3 s, local or on a peer's request) | `identify_screens.py` | `identify_overlay.py` | `identify_overlay.py` | n/a | ≈40 |
| F29 | **Edge glow and beam** on the pushed edge | n/a | EdgeGlow in shell | `edge_glow.py` (0.5k) | n/a | ≈25 |
| F30 | **Crossing effects**: the Pen engine, colour packs and the `fx_*` directions (Membrane, Sparks, Instrument, Folio, Selvedge offered; **Ink and Warp hidden**, 1,251 lines) | `effects.py` (1.5k), `fx_*.py` (3.3k) | `effects_overlay.py`, `frame_clock.py`, `previews.py` (1.5k) | `effect_overlay.py`, `effect_previews.py`, `frame_clock.py` (1.0k) | n/a | 73 core, ≈140 mac, 71 win |
| F31 | **Notch island and notch beam** | `SpanEdge` notch model | `notch_island.py`, `notch_beam.py` (0.8k) | n/a | n/a | 31 |
| F32 | **Trackpad haptics** while pushing | n/a | Haptics in shell | n/a | n/a | small |
| F33 | **Look**: tokens, theme, light and dark, appearance switch, fonts, widget kit, motion | root `theme.py`, `tokens.py` | `widgets.py`, `motion.py` (3.0k) | `theme.py`, `tokens.py`, `titlebar.py`, `widgets.py`, `motion.py` (2.9k) | n/a | ≈100 |
| F34 | **i18n**: en source, `it` (1,246 of 1,500 translated), pseudo-locale, US spelling at run time | `i18n.py`, `locale.py` | relaunch on change | relaunch on change | n/a | 25 core |
| F35 | **Crash log and rotating log** | `crash_log.py` | shell | shell | n/a | small |
| F36 | **Notifications** (refusals, wake, shook home, cannot switch) | text in core | `notices.py` (UNUserNotificationCenter) | tray balloons in shell | n/a | ≈12 mac |
| F37 | **Single instance and second-launch activation** | n/a | `instance_signal.py` + flock | `Local\Beamer.Receiver` + `Local\Beamer.InputController` mutexes, QLocalServer | n/a | ≈15 |
| F38 | **Permissions page** (Accessibility, Input Monitoring, relaunch) | n/a | shell | n/a | n/a | in shell suites |
| F39 | **Connection page**: hide addresses, port, language, address readout | n/a | shell | shell | n/a | in shell suites |
| F40 | **Input-blocker diagnostics and GameInput recovery** | n/a | n/a | `input_blocker.py`, `input_injector.py`, SYSTEM task from the installer | n/a | 34 |
| F41 | **Process tuning**: App Nap opt-out, ABOVE_NORMAL priority, EcoQoS opt-out, highest-priority hook thread | n/a | shell | shell | n/a | 7 |
| F42 | **macOS 27 LAN workaround**: SSH tunnel fallback to `127.0.0.1:24822` | n/a | `bridge.py` + `Beamer Tunnel.command` | n/a | n/a | none |
| F43 | **Audio and microphone follow input** (fork) | `audio/` (4.5k) | `audio_mac.py`, `audio_driver.py`, `audio_page.py` (1.8k) | `audio_win.py`, `audio_page_win.py` (1.4k) | engine (≈8k), Mac backend (1.8k), HAL driver (1.2k C), Win backend (2.7k) | 159 core, 36 mac, 29 win, C++ suites |
| F44 | **Camera follows input** (fork) | `camera/` (3.4k) | `camera_section.py` (0.2k) | `camera_section_win.py`, `vcam_install.py` (0.7k) | video engine (3.2k), Mac video (2.4k), Win video sink (2.3k), install verbs (0.7k), camera DLL (1.4k) | 181 core, 19 mac, 65 win, C++ suites |
| F45 | **Shared displays** (fork): one owner for a monitor cabled to two machines | `display/` (0.5k) | `shared_display_mac.py` (private CGS/SLS) | `shared_display_win.py` (CCD API; **no DDC/CI**) | n/a | 16 core, **0 mac, 0 win** |
| F46 | **Packaging and install**: py2app + DMG + notarization; PyInstaller + Inno Setup + elevated maintenance; dev installers | n/a | `setup.py`, `build_dmg.sh`, `dmg/`, `Install-Beamer-Mac.command` | `build.spec`, `Beamer-Setup.iss`, `Install-*.ps1` | `build-mac.sh`, `build-win.cmd` | none |
| — | **Phone peers** (out of scope, never port) | see "Phone-peer code" below | | | | |

## Facts later tickets need

- **Phone-peer code to leave behind.** A phone is a peer with `port == 0`. Phone code is the QR pairing path
  (`core/pairing.py`, `mac_app/qr.py`, `win_app/qr_code.py`, the hidden "Pair a phone" button), `peerlist.normalise_phones`,
  `"ios"`/`"android"` platform names, and about 40 `port == 0` guards across core and both apps. It is hidden by
  `PAIRING_QR_VISIBLE = False`. `tools/phone_vectors.*`, `core/tests/spec_phone.py`, `v6_vectors.json` and the
  phone parts of `WIRE.md` exist for phone clients. Line-level lists are in each section below.
- **Hidden, dead or test-only code** (the keep/drop ticket should confirm each as a drop rather than port it by
  accident): the Ink and Warp effects; pairing v2 (`Announcer`, `Discovery.pair`); the v5 refusal path; about 15
  uncalled functions in `ways.py`, `settings_sync.py` and `camera/view.py`; `diagram.ArrangementDiagram` and
  `edge_glow.GlowPreview` (Windows); the Mac tap-based gesture path (never fires on macOS 27); the `no_unlock.py` stub;
  the dev-only `audio-dev-*` files written by the audio service; the Phase 1 prototypes under `dev/` (two are still
  test oracles).
- **Feature flags**: `PAIRING_QR_VISIBLE = False`. `AUDIO`, `CAMERA` and `SHARED_DISPLAY` are `True` (fork). Camera
  and shared displays ride the `audio` capability and the AudioService worker.
- **Untested today**: `shared_display_mac.py`, `shared_display_win.py`, `core/wol.py`, the `fx_*` modules (only
  through the effects tests), `link_timing.py`, packaging.
- **OS versions implied by today's code** (input for
  [Minimum macOS and Windows versions for the Rust app](https://github.com/FredeAlexandre/beamer/issues/17)): the Mac
  app is `LSMinimumSystemVersion 13.0`, the HAL driver targets 12.0+, the audio helper 13.0+, process taps need
  14.2+, and the display-link frame clock needs 14+ (NSTimer fallback on 13). On Windows, the virtual camera
  (`MFCreateVirtualCamera`) and the synthetic touchpad gesture injector need Windows 11.
- **Overlay demands** (input for the UI bake-off): every overlay is transparent, click-through and always on top,
  placed per monitor. The Mac uses NSStatusWindowLevel (the notch at NSPopUpMenuWindowLevel), on all Spaces and above
  full-screen apps. Windows re-asserts HWND_TOPMOST over the taskbar. Frame rate is the display's maximum (120 Hz on
  ProMotion) divided by a user setting of 1, 2 or 4, and previews are capped at 60. One Mac overlay is deliberately
  **not** click-through: the invisible gesture capture panel. The Windows pointer veil is a 16×16 window at opacity
  0.01. Effects are replayed from a recorded draw-op list (paths, linear and radial gradients, alpha, additive
  compositing). The notch beam uses a Gaussian-blur bloom.
- **External or third-party dependencies at run time**: VB-CABLE as the Windows virtual mic (not bundled); the
  RoomDeck credential provider for unlock (not in the repo); the GitHub releases API for updates.
- **OS touchpoint counts**: core about 34 call sites; mac_app 203 distinct APIs (5 private); win_app 101 Win32
  functions + 13 pywin32 + one COM chain, WMI, registry and 4 shelled tools; Linux about 145 (portals, X11,
  libxkbcommon, libei); native code, per-framework counts in its section. The per-crate mapping is in the OS bindings
  research ([macOS](https://github.com/FredeAlexandre/beamer/issues/6), [Windows](https://github.com/FredeAlexandre/beamer/issues/7)).
- **Spec sources a port will lean on**: `WIRE.md` (including the fork's sections 11 to 13), `PAIRING.md`,
  `dev/audio/PHASE2.md` and `dev/camera/PHASE2.md` (native code cites their § numbers throughout), and
  `vcam-win/shm_protocol.h` ("frozen; change only with the operator").

---

The four sections below are the detailed sweeps, one per area, as written by each reader.

## core/

### Features (core)

Scope: every non-test `.py` under `core/` (61 files, 27,656 lines by `wc -l`). Test counts are `grep -c "def test_"` per file in `core/tests` (2,205 in all, including `settings_behaviour.py` 17 and `effect_size_probe.py` 1. `audio_fixtures.py`'s single `def test_input` is a fixture helper and is left out). 90 of them test app code, not core; they are listed in Notes.

| Feature | What it does (one line) | Files | Lines (non-test, `wc -l`) | Tests (test files + count of test methods) |
|---|---|---|---|---|
| Package root | Package docstring: what core holds and that platform code is injected. | `__init__.py` | 7 | test_no_copies 2 (no duplicate copies of core modules in mac_app/win_app) = **2** |
| Wire format + encryption (protocol v6) | Preamble/handshake, ChaCha20-Poly1305 framed sessions with counters, X25519 key ids, a builder and validator for every message (input, clipboard text + PNG with size/pixel/metadata caps, focus/switch/accept/refuse/accepts, arrangement, settings, identify_screens, paired, text, gesture), v5 refusal, ports 24820/24821. | `protocol.py` | 1,328 | test_protocol_v6 94, test_v6_vectors 54, test_wire_vectors 17 (+ `v5_link.py` frozen v5 copy, `pngs.py` helper) = **165** |
| Initiator link | `OutboundLink` per peer: dial, handshake, ping/keepalive (1 s ping, 2 s dead), input sequencing + ack/backpressure, round-trip measurement, capability gating, endpoint follow (address learnt from peers), TCP_NODELAY/keepalive; bounded local timings. | `link.py`, `link_timing.py` | 1,049 + 52 = 1,101 | test_link 86, test_motion_flow 7, test_link_endpoint_follow 6, test_link_latency 3 = **102** |
| Responder (receiver) | `LinkResponder`: TCP listener, many peers / one owner, `PeerBook`, handshake slots, input injection with `InputScale` (pointer/scroll speed, reverse scroll), motion coalescing, zones armed as return edges, clipboard both ways (visit stamps, clipboard-back), Windows lock-screen unlock hook, send-home, identify-screens, settings/arrangement/audio dispatch to callbacks. | `receiver.py` | 2,092 | test_responder_ownership 57, test_responder_ported 47, test_responder_handshake 44, test_clipboard_reliability 22, test_responder_input 19, test_responder_races 13, test_receiver_backlog 12, test_input_scale 4, test_responder_accessors 4, test_crossing_input_fence 3, test_clipboard_stress 3 = **228** |
| Owner (input routing) | Pure state machine for where this machine's input goes: `go`, take/accept/refuse/busy, timeouts (TAKE_WAIT 1 s, LEFT_OUT 3 s, SENT_HOME_FOR 5 s, CLIPBOARD_AFTER 10 s), held-key release, clipboard send/set actions, multi-hop `switch` routing. | `owner.py` | 501 | test_owner 82, test_three_desks 15, test_three_machines 12 = **109** |
| Crossing state machine | Pure crossing events (drop tiles, edit ways, receive picture/arrangement, link up, leave out/put back, displays changed, profile switch/new/rename/remove, shake toggles), compiles intent into zones and returns an Outcome of messages to send. | `crossing_state.py` | 763 | test_crossing_state 41, test_crossing_e2e 35 = **76** |
| Return edge / edge push / resistance | Monitor rectangles, edge/part/corner/notch (`SpanEdge`) models, pressure-based resistance before crossing, landing position on arrival, arrival grace; strip geometry mapping 0–500 resistance onto a slider track. | `return_edge.py`, `resistance_strip.py` | 448 + 13 = 461 | test_arrival_grace 19, test_rel1510b_geometry 16, test_switch_display_pair 4, test_motion_paths 4, test_scoped_corner 3 = **46** |
| Ways, layout map, zones, shortcuts, jump keys | Crossing map ("atlas") tiles, snap/resize, derive and compile layout to zones (edges, thirds, corners, notch), side sharing and clash resolution, way-back state, jump-key chords and validation, shortcut cycle order (`next`/`ticked`), user-facing sentences. | `ways.py` | 1,666 | test_ways 149, test_atlas 62, test_shortcut_cycle 14, test_phone_targets 8, test_tile_resize 7, test_way_helpers 3 = **243** |
| Crossing profiles | Named profiles of ways, migration, stamped per-desk registers, desk messages and merge, "same ways on every machine", shake settings per profile, profile list view rows. | `profiles.py` | 902 | test_profiles 49, test_profiles_e2e 30, test_profile_events 19, test_profiles_migration 12 = **110** |
| Settings sync | Opt-in sync of groups (Crossing, Design, Shortcut, Names) with stamps, merge newest-wins, snapshot/revert, design follow (follow a peer's design + lock), per-key Mac/PC value mapping, notes/banners. | `settings_sync.py` | 916 | test_settings_sync 88, settings_behaviour 17, test_settings_mixed_groups 12, test_settings_per_key 9, test_settings_per_key_desks 3, test_settings_per_key_review 2, test_same_groups_network 2 = **133** |
| Peer list | Add/remove peer entries, labels for duplicate names, dormant (stale) pairings, refusal throttle, `paired` id lists, pairing error/refusal wording, phone-entry normalisation. | `peerlist.py` | 294 | test_peerlist 46, test_peer_address 7 = **53** |
| Pairing (CPace) + LAN discovery | CPace-X25519-SHA512 PAKE on a 6-digit code (v3, v2 still present), pairing over UDP broadcast beacon and TCP, discovery of machines, dormant repair, interface/broadcast enumeration, pairing address, QR text/secret, `PairingService` socket side. | `pairing.py` | 2,459 | test_pairing_cpace 56, test_pairing_v3 46, test_pairing_service 37, test_pairing_paths 27, test_dormant_repair 19, test_pair1_windows 8, test_portable_discovery 5, test_phone_contract 3 = **201** |
| Easy pair ("Pair nearby", SAS) | Experimental commit/reveal short-authentication-string pairing in a 60 s window, carried in pairing beacons (`easy_*` messages); live behind the apps' "Pair nearby" button. | `easy_pair.py` | 206 | test_easy_pair 13, test_easy_pair_service 9 = **22** |
| Keys: key table, special keys, dead keys | Physical key → wire name table (WIRE.md §7), evdev name/US tables, cross-OS system-key names (Spotlight/search, app switcher chord Cmd↔Alt), dead-key composition for Mac capture. | `keytable.py`, `special_keys.py`, `capture_types.py` | 115 + 59 + 72 = 246 | test_keytable 18, test_special_keys 16, test_mac_dead_keys 10, test_keypad_identity 3 = **47** |
| Ignored keys/buttons | Per-machine list of keys/mouse buttons/media keys that stay local while input is remote, with a press/release gate. | `ignored.py` | 91 | test_ignored 12 = **12** |
| Wake-on-LAN | Read peer MAC from ARP after a link succeeds, build the magic packet, send to subnet + limited broadcast. | `wol.py` | 109 | none = **0** |
| Update check | GitHub releases API once a day / on demand, version compare, which notice to show, portable download link; never downloads. | `updates.py` | 206 | test_updates 36 = **36** |
| Portable runtime + store | Run-from-folder mode: data dirs inside app folder, redirected temp/cache env vars, reserved listener port, instance suffix; passphrase-encrypted settings file (argon2id + SecretBox). | `portable_runtime.py`, `portable_store.py` | 112 + 201 = 313 | test_portable_store 13, test_portable_runtime 5, test_portable_network 3 = **21** |
| Feature flags | Four module constants gating phone QR pairing, audio, camera, shared displays. | `feature_flags.py` | 8 | test_feature_flags 1 = **1** |
| i18n | gettext catalogues with British English source, `tr`/`trn`/`trc`/`N_`, .po parsing to .mo in memory, pseudo-locale, OS language detection, `BEAMER_LANGUAGE` override. | `i18n.py` | 289 | test_i18n 11, test_i18n_coverage 7 = **18** |
| Locale (spelling) | Detect a US user region from the OS and Americanise display spelling. | `locale.py` | 89 | test_locale 7 = **7** |
| Crash log | Uncaught worker-thread exceptions to the log; on macOS `faulthandler` writes all thread stacks to `crash.log`. | `crash_log.py` | 44 | test_crash_log 3 = **3** |
| Response time | Monotonic round-trip samples from optional ping probes. | `response_time.py` | 63 | test_response_time 3 = **3** |
| Shake to come home | Clock-free shake detection on relative mouse motion, per-backend stroke lengths, 50–200 % sensitivity. | `shake.py` | 192 | test_shake 27 = **27** |
| Idle pointer | Hide a driven machine's pointer after its driver lets go, until it moves again (watch thread). | `idle_pointer.py` | 88 | test_idle_pointer 7 = **7** |
| Identify screens | Number a machine's displays in map order for the map and a 3 s on-screen overlay. | `identify_screens.py` | 13 | test_identify_screens_core 8 = **8** |
| Windows tool output | Run a Windows console tool and decode its OEM-codepage output outside subprocess's reader threads. | `windows_tools.py` | 27 | test_windows_tools 6 = **6** |
| Crossing effects engine | Canvas-like `Pen` that records draw ops (paths, gradients, alpha, composite), `Player` timeline (push/ticks/cross/arrive/switch), classic Glow/Beam/Aperture/locator ring, colour packs (Light, Mono, Bright), legibility on light wallpapers, previews for Design pages, pace/size/frame-rate settings, hidden-direction stand-ins. | `effects.py` | 1,476 | test_switch_arrival 19, test_effect_sets 14, test_length_size_sliders 13, test_style_previews 7, test_colour_packs 6, test_effect_size 5, test_effect_size_scaling 4, test_effect_detent_pace 4, effect_size_probe 1 = **73** (+ helpers render_effect_parity, render_effect_sets, profile_effect_sets, run_effect_checks) |
| The nine fx_* crossing effects | Seven effect "directions" of three effects + packs each (Membrane, Sparks, Instrument, Folio, Selvedge offered; Ink, Warp hidden), plus Light's aperture and shared material/boundary helpers. | `fx_folio.py` 106, `fx_ink.py` 959, `fx_instrument.py` 745, `fx_light.py` 40, `fx_material.py` 30, `fx_membrane.py` 549, `fx_selvedge.py` 96, `fx_sparks.py` 487, `fx_warp.py` 292 | 3,304 | no per-module test files; exercised through the effects tests above (test_effect_detent_pace imports `fx_instrument.detent`) = **0** direct |
| Shared displays (`display/`) | A monitor cabled to two machines is owned by one; EDID display keys, `display_*` messages, ownership route, which local outputs stay on, menu/tray entries; runs in the AudioService worker; `display.json`. | `display/__init__.py` 4, `model.py` 208, `part.py` 283, `view.py` 45 | 540 | test_display 16 = **16** |
| Audio (`audio/`) | Audio + microphone follow the input: control-plane `AudioService` worker, native `beamer-audio` helper child process (JSON lines, watchdog, restart backoff), snapshot model, route planner, `audio_*` wire messages, `audio.json` store, Audio page model, in-process fake helper for tests. | `audio/__init__.py` 13, `service.py` 1,632, `view.py` 972, `wire.py` 462, `fake_helper.py` 406, `helper.py` 384, `model.py` 364, `planner.py` 197, `store.py` 79 | 4,509 | test_audio_service 37, test_audio_view 36, test_audio_link_gating 23, test_audio_model 17, test_audio_planner 15, test_audio_helper 12, test_audio_wire 10, test_audio_cluster 7, test_audio_helper_native 2 = **159** |
| Camera (`camera/`) | Camera follows the input: `CameraPart` inside the audio worker, video stream planner, `camera_*` wire messages, `camera.json`, Camera page model, PC "Beamer Camera" placeholder texts, fake video for tests. | `camera/__init__.py` 14, `part.py` 1,150, `view.py` 671, `wire.py` 409, `model.py` 378, `fake_video.py` 338, `planner.py` 196, `texts.py` 110, `store.py` 87 | 3,353 | test_camera_view 33, test_camera_part 30, test_camera_link_gating 26, test_camera_r4 19, test_camera_wire 19, test_camera_planner 17, test_camera_model 16, test_camera_cluster 13, test_camera_texts 8 = **181** |

Totals: 32 rows, 27,656 lines (matches `wc -l` total), 2,115 test methods mapped to core features + 90 app-side (Notes) = 2,205.

Clipboard, gestures, `text` input and Windows unlock have no file of their own: they live in `protocol.py` (messages, caps), `receiver.py` (apply/inject) and `owner.py` (clipboard send/set actions).

### Phone-peer code (core)

In the wire model a phone is a peer whose `port` is 0: it pairs, may drive a desktop, is never dialled, and never receives input. Phone-specific code, and the phone guards inside shared code:

- `feature_flags.py:1` `PAIRING_QR_VISIBLE = False`: hides "Pair a phone" (mac_app/machines_panel.py:92-93, 831; win_app/machines_win.py:575, 894). test_feature_flags says "Phone pairing stays hidden until the phone apps ship".
- `pairing.py`
  - `:49` docstring: a code shown as a QR carries a second secret.
  - `:137-144` `ERROR_BAD_QR`, `QR_SECRET_BYTES = 16`.
  - `:151` TCP pairing "for a host no broadcast reaches and for the phones". The listener is shared with desktop `pair_by_address`.
  - `:156-157` `QR_PREFIX = "beamer://pair?"`, `_QR_KEYS`.
  - `:272` QR secret appended to the CPace password (22 bytes).
  - `:326-339` `peer_entry`: `port == 0` means a phone (`host=""`, `send=False`).
  - `:441-444` `qr_text`; `:447-475` `_decimal` and `read_qr`.
  - `:485-497` `pairing_address`: the address the sheet shows and the QR carries (shared).
  - `:526-527, :569, :581` `PairingHost.qr_secret`.
  - `:617-661` the `qr` field in `pair_start`.
  - `:787-842` `PairingClient(qr_secret=…)`, which sends `qr: 1`.
  - `:2028-2040` `PairingService.qr_text`. Called by mac_app/machines_panel.py:808 and win_app/kvm_bridge_win.py:4695, rendered by win_app/qr_code.py.
  - `:2371-2379` `pair_by_qr` (the requester side of a QR scan) and `:2417-2427` `_not_paired_already` (QR path only). Neither has a caller in mac_app or win_app: this is the phone side, used only by tests.
- `peerlist.py`
  - `:42-47` `desktops()` / `first_desktop()` filter out `port == 0`.
  - `:50-78` `normalise_phones`: strips receiving-only fields, zones and `design_follow_peer` from zero-port peers. Called from mac_app/settings_store.py:645 and win_app/app_config.py:823, 1039.
  - `:88-92` `labels()` docstring, "Paired phones keep their labels".
  - `:127-128` `label_for` returns "" for port 0.
  - `:148` `paired_ids` skips port 0.
- `protocol.py:436` `PLATFORMS = ("macos", "windows", "linux", "ios", "android")`; `:492` b64 docstring ("a QR secret"); `:921` `hello` accepts `port` 0.
- `keytable.py:9` `MAC_PLATFORMS = frozenset({"macos", "ios"})`.
- `owner.py`
  - `:4` docstring: the owner serves "(ported) the phones".
  - `:128-130` `resistance_px=None` for a phone ("no hand at an edge").
  - `:405-406, :491-492` omit `resistance_px` when None.
- `receiver.py:150-159` `learnable_host`: never learns a host from a port-0 peer; `:205` `zone_models` skips port-0 peers.
- `crossing_state.py:154-155` "a phone cannot have ways".
- `ways.py`
  - `:1027` `jump_key` is None for port 0.
  - `:1036-1037` `check_jump_key` raises "a phone cannot be a jump target".
  - `:1205-1206` `edit` raises "a phone cannot have a side or zones".
  - `:1368-1370` `_zoned`; `:1526-1527` `_sendable`; `:1545` docstring.
- `easy_pair.py:33` `eligible` excludes port 0 (desktop-only, phone-aware guard).
- Tests:
  - `core/tests/spec_phone.py` (an independent phone peer, `Phone`/`PhoneLink`).
  - test_phone_contract (3), test_phone_targets (8).
  - test_pairing_cpace `PhoneVectorTests` :104.
  - test_pairing_v3 :518 (phone requester saved undialled) and the QR tests :595-704.
  - test_v6_vectors :3, :446-461, :564-568, :721, :750-766 (`tools/phone_vectors.py` generator, "not part of the public source").
  - test_wire_vectors :2.
  - `core/tests/v6_vectors.json` ("the vector file the phone apps are built against").
  - test_peerlist :67-85, :281; test_ways :14, :818, :1002, :1028, :1083; test_owner :112; test_keytable :8-9; test_feature_flags :7-9; test_pairing_service QR tests :264-309, :620.

### Linux-only parts (core)

- `keytable.py:64-115` `EVDEV_NAMES`, `EVDEV_US`, `evdev_physical` (Linux sender key codes). Not purely Linux: win_app/capture_win.py:359 uses `EVDEV_NAMES` for numpad scan names, and `evdev_physical` has no non-test caller.
- `shake.py:9-10` `STROKE_X11 = 192.0`, `STROKE_WAYLAND = 48.0`.
- `locale.py:29-35` `_linux_locale` (LC_ALL/LC_MESSAGES/LANG), `:63-64` Linux branch.
- `i18n.py:91` Flatpak resource-path comment, `:232-241` `_linux_languages` (LANGUAGE/LC_*), `:251` fallback branch.
- `portable_runtime.py:70` sets `XDG_CACHE_HOME`. It is set on every OS in portable mode, but only Linux reads it.
- `pairing.py:1051-1100` `_Ifaddrs` / `_posix_interfaces` via libc `getifaddrs`. This is shared macOS+Linux; `:1067` is a darwin branch.
- `receiver.py:1872` comment: the clipboard stamp is unreadable before a Wayland session is up (generic `stamp is None` handling).
- Platform string only: `protocol.py:436, :857` (unknown platforms keyed as linux); `crossing_state.py:345, :351`; `profiles.py:456`; `audio/service.py:54`; `audio/view.py:32`; `audio/fake_helper.py:25` (`"linux": []`, so Linux has no audio devices). test_audio_link_gating:66 checks that Linux never announces audio.
- Linux tests in core/tests that exercise win_app's Linux modules: see Notes.

### UI surfaces described in core

- **Crossing effects overlay** (`effects.py`, `fx_*.py`)
  - Draw ops: core emits per-frame `Pen` op lists (fill and stroke of bezier paths, linear and radial gradients, `globalAlpha`, composite `lighter` (additive) and `source-over`, already-applied transforms). The Mac replays them in Core Graphics, Windows in QPainter.
  - Placement and clipping: an effect plays on one display (`display` rect, Pen clipped to it) or on the whole desktop. The toolkit needs a transparent overlay per monitor covering every screen.
  - Window behaviour (implied, not stated in core): click-through and always-on-top.
  - Frame rate and timing: frame-rate divisor "Match display / Half / Quarter" (`effects.py:1230`). Each depart/arrive animation is capped at 0.8 s. Pace is 0.25–2.5× (presets 0.5/1/2). Size presets are small/medium/large.
  - Inputs from the toolkit:
    - a Reduce-motion flag (accessibility), the only accessibility demand in core
    - dark/light wallpaper, for `legible()` colour shading
    - the Mac notch rect
    - device-pixel scale (`effect_size_probe`)
- **Design-page previews** (`effects.preview_scene`, `preview_still_scene`, `still_view`, `preview_switch_scene`): small animated and still canvases with a hover loop. They need the same Pen replay into a widget.
- **Identify-screens overlay** (`identify_screens.py`): a number label per display, shown for `OVERLAY_SECONDS = 3.0`, placed per monitor.
- **Crossing map editor** (`ways.py`): tile geometry (map units, `RESIZE_MIN/MAX` 40–480), snap radius 12, legacy star diagram sizes (`DIAGRAM_*`), drag-resize, thirds/edge/corner picking, atlas sentences. It needs a canvas with drag, hit-testing and resize.
- **Resistance strip** (`resistance_strip.py`): a 0–500 track with an 18 pt inset. It needs a custom slider or strip widget.
- **Page/menu view-models (text plus choices, no drawing):**
  - `audio/view.py` (Audio page: mic/output selectors, status lines)
  - `camera/view.py` (Camera section + map extras)
  - `display/view.py` (menu bar / tray entries per shared display)
  - `peerlist.py` pairing/refusal sentences
  - `settings_sync.py` notes and the following banner
  - `profiles.py` profile list/machine lines
  - `updates.visible_notice`
- **Beamer Camera placeholder** (`camera/texts.py`): title and detail strings rendered into the virtual-camera picture by the native helper, not by a UI toolkit.

### OS touchpoints (core)

About 34 direct call sites in non-test core:

- ctypes 4: Win `GetUserPreferredUILanguages`, Win `GetUserDefaultLocaleName`, libc `getifaddrs`, `iphlpapi` `GetIpAddrTable`.
- PyObjC Foundation 3: `NSLocale` ×2, `NSURLConnection`.
- subprocess 4: `windows_tools.run`, `arp` on Windows and on POSIX in `wol`, `Popen` of the `beamer-audio` helper.
- TCP sockets 5: link dial, link listener, pairing listener, pairing dial, portable reserved listener.
- UDP sockets 5, with about 14 `sendto` sites: pairing beacon/discovery broadcast socket with `SO_BROADCAST`/`SO_REUSEPORT`, route probe, two per-source reply sockets, WoL broadcast.
- DNS/hostname 3 `getaddrinfo`, plus `gethostname`.
- HTTPS 1: GitHub releases API.
- Fixed files 8: `audio.json`, `camera.json`, `display.json`, `audio-dev-status.json`, `audio-dev-events.jsonl` (+`.1` rotation), `crash.log`, the portable encrypted store, the portable data tree.
- Env 2 sites: portable mode writes TMP/TEMP/TMPDIR/XDG_CACHE_HOME/LOCALAPPDATA/APPDATA; reads of LANG*/LC_*/BEAMER_LANGUAGE/RESOURCEPATH/`_MEIPASS`.
- Signal handler 1: `faulthandler`.
- No mDNS or zeroconf.

### Notes

- **Feature flags** (`core/feature_flags.py`):
  - `PAIRING_QR_VISIBLE = False`: phone QR pairing hidden.
  - `AUDIO = True`: read in `audio/helper.py:96` and mac_app/pages.py. The comment says False when offered upstream.
  - `CAMERA = True`: `audio/service.py:346, :733` and the apps.
  - `SHARED_DISPLAY = True`: `audio/service.py:353`, mac_app/audio_mac.py:75, win_app/audio_win.py:80.
  - AUDIO, CAMERA and SHARED_DISPLAY are fork-only "on". Camera and displays ride the audio capability and the AudioService worker.
- **Hidden effects:** `effects.py:433` `HIDDEN = ("fx_ink", "fx_warp")`, which is 1,251 lines of effect code offered nowhere. A saved choice of one of them maps to a stand-in (`STAND_INS`, `effects.py:434`).
- **Pairing v2 still in code:** `PAIRING_VERSION = 2` is the default of `PairingHost`/`PairingClient` (`pairing.py:85, :510, :791`). `Announcer` (`pairing.py:1202-1372`) and `Discovery.pair` (`:1453`, v2) have no production caller; the apps use only `PairingService` (v3). The docstring (`:44-46`) says they go once the apps move to v3, which they have.
- **v5 link:** v5 is no longer spoken, but `protocol.py` still reads a v5 preamble to refuse it with an "update" message. `core/tests/v5_link.py` keeps a frozen copy.
- **Easy pair:** marked "Experimental… see docs/spec-easy-pair-1.6.md before release", but it is reachable: the "Pair nearby" button in both apps calls `begin_nearby`. It is not behind a flag.
- **Test doubles shipped outside `tests/`:** `audio/fake_helper.py` (406, runnable as `python -m core.audio.fake_helper`) and `camera/fake_video.py` (338).
- **Dev-only output in production code:** `audio/service.py:45-46` writes `audio-dev-status.json` and `audio-dev-events.jsonl`, the latter "dev only… for the E2E harness".
- **Top-level defs with no non-test caller in the repo (by name search):**
  - `keytable.evdev_physical`
  - `settings_sync.layout_recipients`, `settings_sync.switch_note`
  - `protocol.recv_msg`
  - `ways.inverse_layout`, `ways.edge_form_detail`, `ways.share_offer`, `ways.default_side`, `ways.blocked_sentence`, `ways.no_way_back_sentence`, `ways.presence_due`, `ways.gone_sentence`, `ways.missing_sentence`
  - `camera/view.apply_camera_notice`, `camera/view.apply_row`
  - `ways.diagram_layout` is labelled "legacy star placement" but still has callers.
- **No direct tests:** `wol.py`, `link_timing.py` and the `fx_*` modules have no test files of their own.
- **Platform-specific code in core:**
  - Windows lock-screen unlock path: `receiver.py:1968-1981`, "Unlocking Windows…", via an injected `unlock` module.
  - `windows_tools.py` is Windows-only.
  - `crash_log` writes fatal-signal stacks on macOS only.
  - `updates.fetch` uses Foundation on macOS.
- **Tests in `core/tests` that exercise app code, not core (90 methods):**
  - Linux (win_app's Linux modules): test_wayland_scroll_wire 10, test_linux_accents 11, test_ghlinux_devices 7, test_ghlinux_arrival 5, test_ghlinux_clipboard 4, test_linux_accent_holds 3, test_linux_accent_keymaps 2, test_flatpak_libei_path 1.
  - Windows: test_windows_accent_releases 12, test_windows_launch_manifest 1, test_patch15_lock_keys 0.
  - Mac: test_mac_key_repeat 10, test_patch15_nearby 3, test_address_choice_commit 3.
  - Mixed: test_rel1510_input 9 (Linux and Windows input), test_portable_packaging 4, test_release_body 4 (tools/release_body.py), test_firewall_contract 1 (the Linux firewall stand-in).
- **Known environmental failures** (dev/README.md): test_profiles_migration and test_tile_resize `git archive` private commits.

## mac_app/

### Features (mac_app)

Scope: every non-test `.py` under `mac_app/` (35 top-level + 2 in `dmg/`), 26,363 lines (`wc -l`). Tests: 114 `tests/test_*.py` files, 1,842 `def test_`. Line counts are whole files; test counts are per test file, each test file assigned to exactly one row by primary subject.

| Feature | What it does (one line) | Files | Lines (non-test, `wc -l`) | Tests (test files + count of `def test_`) |
|---|---|---|---|---|
| Controller: input capture, redirect, links | `KVMController`: CGEventTap on its own CFRunLoop thread, Quartz event to wire translation (keys, dead keys, caps lock, modifiers, mouse, scroll, buttons), redirect/return, cursor pin + dissociate, jump key, shortcut cycle, shake-to-home, tap watchdog, outbound queue, multi-peer links, clipboard sync (text + PNG), desk/arrangement/profiles/settings sync, identify-screens request, round-trip ms, audio link hooks, SSH-tunnel fallback to 127.0.0.1:24822 | bridge.py | 3250 | test_bridge 141, test_caps_lock 17, test_mac_links 102, test_mac_arrival_grace 7, test_mac_review 14, test_mac_pairing 3, test_mac_loopback 12, test_replay 6, test_peer_labels 6, test_shake_mac 15, test_shortcut_home_chain 17, test_update_config_redirecting 2, test_crossing_handlers 12, test_ignored_inputs 15, test_settings_sync_link 13, test_profiles_sync_mac 11 = **393** |
| Crossing engine | Pure pressure arithmetic against desktop edges: methods shortcut/edge/part (thirds)/corner/notch, resistance decay, hold, drag protection, notch styles | crossing.py | 725 | test_crossing 66, test_crossing_zones 11, test_crossing_matrix 34 = **111** |
| Being driven (inbound) | Listener on this Mac's port running `core.receiver.LinkResponder` with Mac injector/clipboard/geometry; owner hand-off; Quartz display geometry + warp (`display_map`, bounds) | windows_input.py, desktop_mac.py | 317 (235 + 82) | test_driven_mac 42, test_mac_three 20, test_mac_to_mac 4, test_mac_wire 12, test_mac_listener_port 4, test_way_home 18 = **100** |
| Input injection | CGEvent keyboard/mouse/scroll/text posting with INJECTED_MARK user-data, click-state, US-position keys, media keys via NSEvent systemDefined, gestures as System Settings chords, browser back/forward, IOPM user-activity to wake display | input_injector_mac.py | 643 | test_input_injector_mac 20, test_wayland_scroll_mac 5, test_wheel_lines 3 = **28** |
| Keyboard layout + key codes | UCKeyTranslate table of current layout (Carbon TIS via ctypes), refreshed on input-source change; Mac virtual key code <-> wire name tables | keyboard_layout.py, key_codes.py | 387 (228 + 159) | test_keyboard_layout 32 = **32** |
| Media keys | Decode NSSystemDefined (type 14, subtype 8) aux-control data1 into volume/media/brightness names | media_keys.py | 46 | none dedicated (covered in test_bridge, test_driven_mac, test_ignored_titles) = **0** |
| Ignored inputs naming | Titles for "Stays on this Mac" entries (buttons, media keys) and the pure record decision | ignored_titles.py | 86 | test_ignored_titles 16 = **16** |
| Gestures | Translate trackpad gestures (CGEvent types 29/30 private fields, NSEvent magnify/swipe) to wire messages; DockSwipe classifier | gestures.py | 271 | test_gestures 32 = **32** |
| Clipboard | NSPasteboard text/PNG/TIFF read/write under a lock, changeCount stamps | clipboard_mac.py | 179 | test_clipboard_mac 24 = **24** |
| Audio + HAL driver install (fork) | `MacAudio`/`MacAudioHost` wiring `core.audio.service` to links; Beamer Microphone HAL driver status + install (sudo beamer-hal on dev Macs, osascript admin prompt otherwise); Audio page (mic picker, meters, notices, audio map with routes, per-machine output/devices, test, headset, low-latency vmic) | audio_mac.py, audio_driver.py, audio_page.py | 1827 (228 + 224 + 1375) | test_audio_driver 23, test_audio_page 13 = **36** (core/tests also import audio_mac) |
| Camera (fork) | Camera module of the Audio & Camera page: camera picker, quality, status + Retry, sharing switch, camera access, notices (tray items live in kvm_bridge_app.py) | camera_section.py | 244 | test_camera_section 19 = **19** |
| Shared display (fork) | List external displays, turn one off/on via private CGSConfigureDisplayEnabled/SLSConfigureDisplayEnabled, remember turned-off ids in display-mac.json, CoreDisplay product name | shared_display_mac.py | 203 | none = **0** |
| Notch island / notch beam | Crossing feedback at the notch: island (CAShapeLayer + CASpringAnimation, meter, %) and beam (comet along border, CIGaussianBlur bloom); Reduce Motion variants | notch_island.py, notch_beam.py | 832 (467 + 365) | test_notch_island 10, test_notch_beam 10, test_beam_live 5, test_notch_style_previews 6 = **31** |
| Effects overlay | Replays `core.effects` Pens into Core Graphics in click-through panels, one per display touched; departure/return-push/arrival/switch "landing" | effects_overlay.py | 685 | test_effects_overlay 41, test_effect_fires 31, test_effect_layouts 20, test_effect_parity 2, test_effect_size_raster 0 (reuses core's NativeSizeRasterTests), test_effect_size_settings 2 = **96** |
| Frame clock | Display-link (NSScreen/NSView `displayLinkWithTarget`) frame callbacks with NSTimer 60 Hz fallback; global divisor; 60 Hz cap for previews | frame_clock.py | 69 | test_frame_clock 3 = **3** |
| Pointer hide | Hide the Mac cursor while driving another machine: CGDisplayHideCursor + private CGSSetConnectionProperty("SetsCursorInBackground") | pointer_hide.py | 85 | test_pointer_hide 22 = **22** |
| Identify screens overlay | Big display-number label per display for 3 s (OVERLAY_SECONDS), click-through | identify_overlay.py | 104 | test_identify_screens 15 = **15** |
| Wake / no_unlock / hw address | Wake-on-LAN of a sleeping peer on switch (`WakingController`, packet in core.wol); lock-screen stubs (`is_locked` None); this Mac's MAC for handshake `hw` via getifaddrs ctypes | wake.py, no_unlock.py, hardware_mac.py | 295 (183 + 17 + 95) | test_wake 21, test_wake_three 5, test_hardware_mac 10 = **36** |
| Login item | Start at login via SMAppService (bundle-loaded ServiceManagement), status, open Login Items settings | login_item.py | 74 | test_login_item 5 = **5** |
| Dock visibility | Pure policy: show_in_dock / show_in_tray -> regular/accessory activation policy | dock_visibility.py | 29 | test_dock_visibility 6, test_dock_window 14 = **20** |
| Crossing ledger | Crossing page table per paired desktop: edge chips, thirds segments, corners, notch, notch return, cycle tick, jump key, leave out, rename | ledger.py | 813 | test_crossing_ledger 32, test_profiles_map_ledger 16 = **48** |
| Crossing map (diagram) | Core Animation drawings: AtlasCanvas (drag machines, option-click split/group displays, edge thirds, corner resize, inline rename), ArrangementDiagram, PushStrip (resistance) | diagram.py | 2263 | test_atlas_canvas 70, test_corner_marks 20, test_map_ticks 22, test_part_edge_thirds 9, test_tile_resize_mac 13 = **134** |
| Machines panel + pairing | Overview machines list (state, in use, pin address, cycle, directions, leave out, rename, remove/keep, re-pair) and inline pairing sheet (show code, enter code, nearby list + SAS confirm, address entry, phone pairing); pure row model | machines_panel.py, machines.py | 1109 (1016 + 93) | test_machines_panel 67, test_machines 18, test_dormant_rows 11, test_pairing_pixels 9, test_pairing 14, test_pairing_announcer_error 4, test_phone_ui 10 = **133** |
| QR pairing | Pairing QR via CIQRCodeGenerator -> module grid -> NSImage | qr.py | 52 | test_qr 4 = **4** |
| Link state words | Pure mapping of controller status to tone/word/LED/sentence | link_state.py | 102 | test_link_state 14 = **14** |
| Settings pages model | Pure: PAGES list (key, name, SF Symbol, purpose), scope lines, sidebar dots, Design style/colour groups, menu "Send input to" items, address redaction | pages.py | 427 | test_pages 48, test_i18n_pages 3 = **51** |
| Settings store + config | settings.json (WIRE.md s.1) load/validate/save, migration from 1.4.x config.json, `Config` dataclass + default Mac key_map | settings_store.py, config.py | 1173 (946 + 227) | test_settings_store 32, test_settings_v6 92, test_unreadable_settings 2, test_config_key_map_override 1, test_phone_settings 6, test_shortcut_subtitle 1 = **134** |
| Widget kit + motion | Vernier AppKit components (Label, Module, Switch, Segmented, MenuChoice, Ruler, Spark, LED, Readout, Sidebar, KeyRecorder, JumpKeyRecorder, IgnoredRecorder, CodeBoxes, ChoiceTiles, Swatches, focus rings) + eased show/hide/cross-fade honouring Reduce Motion | widgets.py, motion.py | 2974 (2775 + 199) | test_widgets 13, test_click_focus 8, test_jump_key_recorder_layout 2, test_status_memory 8, test_sidebar_response 11 = **42** |
| Design page previews | Live notch/edge/effect previews hosted in views (PreviewLoop, TileHover, stills), scripted push feed | previews.py | 780 | test_previews 15, test_design_tiles 8, test_design_offscreen 14, test_colour_pixels 4 = **41** |
| Notices | UNUserNotificationCenter auth + banner/list/sound posting, foreground presenter delegate | notices.py | 83 | test_notices_mac 11, test_notify_user_thread 1 = **12** |
| Instance signal | Unix-socket server; a second launch sends "activate" to open the window | instance_signal.py | 48 | test_single_instance 1 = **1** |
| App shell, settings window, menu bar | Entry point (argparse, flock single instance, logging, crash log, App Nap activity); permissions checks; full-screen app detection (AX + window list); GestureOverlay; Haptics; EdgeGlow; main menu; About panel; `ControlWindow` (all settings pages except the Audio body); `TrayApp` rumps menu bar + split left/right clicks + notifications | kvm_bridge_app.py | 5935 | test_app_nap 3, test_appearance 8, test_appearance_contrast 3, test_crossing_page_machines 15, test_design_sliders 4, test_follow_banner 1, test_full_screen 5, test_glow_falloff 8, test_hide_addresses_window 6, test_idle_refresh 6, test_mouse_only_page 1, test_profiles_page 32, test_profiles_keyboard_pure 3, test_reload_config_refreshes_window 1, test_same_on_both 46, test_settings_behaviour 1, test_settings_layout_acceptance 19, test_settings_polish 15, test_titlebar_events 4, test_update_notice 12, test_window_geometry 11, test_window_resize 5 = **209** |
| Packaging | py2app setup (Info.plist, privacy strings, fonts, locales, includes), DMG background drawing + dmgbuild settings. Non-Python: build_dmg.sh 375 (universal2 + cffi lipo, audio helper/driver staging, inside-out codesign, notarytool + stapler, dmgbuild), Install-Beamer-Mac.command 56 (build, ditto to /Applications, tccutil reset on ad-hoc), Beamer Tunnel.command 35 (ssh -L 24822->24820), entitlements.plist 19, entitlements-audio.plist 14, requirements-mac.txt 9, dmg/background.png + @2x | setup.py, dmg/draw_background.py, dmg/settings.py | 253 (125 + 104 + 24) | none = **0** |
| Totals | | 37 files | **26,363** | **1,842** (incl. test_core.py 0, which loads core/tests) |

Sub-areas inside the two big files (line ranges, for porting):
- kvm_bridge_app.py: menu-bar glyph 105-137; logging 139-167; AX / Input Monitoring checks 170-180; flock 182-191; GestureOverlay 194-339; notch geometry + menu bar height 341-389; full-screen detection 391-477; Haptics 480-513; EdgeGlow 516-866; About 872-887; main menu 890-955; window frame + titlebar helpers 958-1109; Design sliders 1112-1192; ControlWindow 1195-5021 (Overview 2353, Same-on-all 2418-2815, login/dock/updates 2843-3073, Crossing 3075, Audio 3094, Design 3109-3256 + 4115-4430, Keyboard 3258-3298, Connection/Permissions 3300-3306 + 4431-4515, profiles 3393-3830, shortcut 3953-4036, ignored 4038-4113); StatusItemClick 5024; LaunchWatch 5052; reopen hook 5085; TrayApp 5106-5866; argparse/stay_awake/main 5868-5935.
- bridge.py: event-tap callback wrapper 68; QuartzEventTranslator 157-412; gesture converters 415-503; KVMController 568-3250 (crossing/desk/identify 813-1100, start/stop 1281-1360, shake 1413-1495, redirect 1497-1716, tap thread 1733-2030, local mouse/jump key 2029-2155, system/gesture events 2215-2360, links 2409-2700, clipboard 2766-3098, outbound/repair 3099-3250).

### UI surfaces (mac_app)

Toolkit notes that apply everywhere: theme/palette/fonts come from repo-root `theme.py` (269 lines) + `tokens.py` (152), outside mac_app. No NSVisualEffectView / vibrancy anywhere. Settings window uses an explicit NSAppearance (System/Light/Dark setting, `theme.AppearanceWatch` KVO), transparent titlebar. Overlays read dark mode from NSUserDefaults `AppleInterfaceStyle`. Overlay animation rate: `FrameClock` display link at the screen's `maximumFramesPerSecond` (e.g. 120 on ProMotion), divided by the Design page Frame rate setting (Match display /1, Half /2, Quarter /4), floor 30 in CAFrameRateRange; NSTimer 60 Hz fallback on macOS 13. Previews capped at 60 (`PREVIEW_MAX_RATE`). Overlay coordinates: Quartz top-left global points flipped against primary screen height.

| Surface | Kind | File | Needs from toolkit |
|---|---|---|---|
| Menu-bar status item glyph (3 states: local / held / windows), template image 22x16 | menu | kvm_bridge_app.py (`menu_bar_glyph`, `set_menu_bar_symbol`) | template image tinting, accessibility description per state, tooltip with drive status + last refusal + camera line; left click opens window, right/ctrl click opens menu (menu detached from NSStatusItem) |
| Menu: drive status line (disabled) | menu | kvm_bridge_app.py | dynamic title, 0.4 s refresh timer |
| Menu: camera line "Camera: App on T" (hidden unless sending video) | menu | kvm_bridge_app.py | hide/show item |
| Menu: header "Beamer <version>" (becomes update download link) | menu | kvm_bridge_app.py | dynamic title/callback |
| Menu: Send input / Return input to Mac (or one "Send input to <machine>" per machine, dimmed if not live) | menu | kvm_bridge_app.py | dynamic insertion/removal, enable/disable |
| Menu: Pause crossing / Resume crossing | menu | kvm_bridge_app.py | dynamic title |
| Menu: This Mac drives <name>/every machine (tri-state), <name> drives this Mac (tri-state) | menu | kvm_bridge_app.py | checkmark incl. mixed state |
| Menu: Stop Sharing / Share the Camera; Allow Camera Access... | menu | kvm_bridge_app.py | hidden unless camera feature + camera |
| Menu: per shared display submenu (machines + "Not managed", checked owner) | menu | kvm_bridge_app.py (`_display_items`) | submenus, checkmarks, rebuild on change |
| Menu: Profile submenu (radio by profile) when Same ways on and >=2 profiles | menu | kvm_bridge_app.py | submenu, checkmarks |
| Menu: Settings..., Show in Dock (check), Reload configuration, Open log folder, About Beamer, Quit Beamer | menu | kvm_bridge_app.py | standard items |
| Main menu bar: Beamer (About, Hide Cmd+H, Hide Others Opt+Cmd+H, Quit Cmd+Q); Edit (Undo, Redo, Cut, Copy, Paste, Select All); View (one per page, Cmd+1..6); Window (Close Cmd+W, Minimise Cmd+M) | menu | kvm_bridge_app.py (`install_main_menu`) | key equivalents, responder chain, Windows menu |
| Dock icon reopen | other | kvm_bridge_app.py (`applicationShouldHandleReopen`) | runtime-added delegate method on rumps NSApp; activation policy regular/accessory switch |
| About panel | dialog | kvm_bridge_app.py (`show_about_panel`) | standard About with centred link credits; activate app first |
| Settings window ("Beamer", 900x640 default, min 640x540, remembered frame) | window | kvm_bridge_app.py (`SettingsWindow`, `ControlWindow`) | resizable/full-screen/tiling aware, transparent titlebar + custom drag strip honouring AppleActionOnDoubleClick, explicit light/dark appearance + cross-fade, keyboard navigation with custom focus rings, release-when-closed off, occlusion/miniaturise notifications (pauses work), two layouts at 600 pt content width |
| Sidebar (SF Symbol rows, attention dots, link block: LED + state word + peer, footer version + website link) | page | widgets.py (`Sidebar`) | SF Symbols, accessibility labels ("needs attention"), arrow-key selection, focus ring |
| Window footer "Changes apply as you make them" / "Closing this window keeps Beamer running" | page | kvm_bridge_app.py, pages.py | text |
| Overview page: Machines module (Pair nearby, Pair a phone [hidden by flag], Pair a machine; rows) | page | machines_panel.py | accessibility labels per row/chip/switch, tooltips, inline rename field |
| Overview: Machine row remove confirm ("Remove <name>? ..." Remove / Keep) | sheet (inline) | machines_panel.py | inline confirm, accessibility labels |
| Overview: This Mac (own name rename) | page | machines_panel.py | inline text field |
| Overview: Pair a machine sheet (Show a code + digit boxes + QR + countdown; Enter a code: On this network list, Not listed? address, code boxes, Pair/Cancel; nearby SAS digits Confirm/Cancel; outcome line) | sheet (inline module) | machines_panel.py, widgets.py (`CodeBoxes`), qr.py | QR image, digit fields, accessibility labels, threads via callAfter |
| Overview: Dock and menu bar (Show in Dock, Show in menu bar) | page | kvm_bridge_app.py | switches |
| Overview: Link status module (LED, state, Delay ms readout + sparkline, Machine readout/picker) | page | kvm_bridge_app.py, widgets.py, link_state.py | live refresh, accessibility label for response rail |
| Overview: Keyboard and pointer (Send input big button, Pause crossing, Hold edges while full screen, Hide the pointer on the machine not in use) | page | kvm_bridge_app.py | buttons, switches, tooltips |
| Overview: Settings / Same on all machines (groups, Everything, Revert, older-version note) | page | kvm_bridge_app.py | switches, popup |
| Overview: At login (Start Beamer when you log in, Open Login Items settings, approval status) | page | kvm_bridge_app.py, login_item.py | SMAppService status |
| Overview: Updates (Automatically check, Check for updates, What's new / Download, Dismiss) | page | kvm_bridge_app.py | opens URL |
| Permissions page (Accessibility Grant, Input Monitoring Grant, status words, At login note, Relaunch Beamer) | page | kvm_bridge_app.py | AX prompt, CGRequestListenEventAccess, relaunch via sh + open |
| Crossing page: profile tabs (New profile..., Rename, Delete... popup menu, per-tab Rename/Remove menu), Same ways on every machine | page | kvm_bridge_app.py | NSMenu popUp at button, accessibility labels |
| New profile / Rename profile alert (text field, Add|Save / Cancel) | sheet (NSAlert) | kvm_bridge_app.py (`_profile_name`) | sheet-modal alert with accessory text field |
| Crossing page: map (AtlasCanvas: machine tiles, displays, lit edge thirds, corners, notch, drag/resize/option-click, inline rename), Identify screens button, legend, hint | page | diagram.py | Core Animation layers with animation (off under Reduce Motion), accessibility (group, per-tile elements, announcements via NSAccessibilityPostNotificationWithUserInfo), keyboard focus |
| Crossing page: own ways module (Keep this machine's own ways, Use the desk's ways, Share mine with the desk, Earlier ways) | page | kvm_bridge_app.py | buttons |
| Crossing page: Machines ledger (rows per desktop) | page | ledger.py | table/row/column/checkbox/button accessibility roles + labels, tooltips, stacks under a width |
| Crossing page: Way home (Shake to come home, Shake sensitivity ruler, Same -> Shortcut, warning box) | page | kvm_bridge_app.py | ruler, switch |
| Crossing page: This machine's pointer (Don't cross while dragging) | page | kvm_bridge_app.py | switch |
| Crossing page: Resistance ruler + PushStrip live pressure | page | kvm_bridge_app.py, diagram.py | slider accessibility role; PushStrip refreshed by NSTimer 1/30 s (30 Hz); crossing flash 0.12 s; lights EdgeGlow preview while dragging |
| Crossing page: Shortcut (key recorder, How you press it: Double-tap / Hold, Time between taps, Shortcut goes to: Last machine / Next in order / Next among ticked, conflict note) | page | kvm_bridge_app.py, widgets.py (`KeyRecorder`, `JumpKeyRecorder`) | NSEvent local monitor while recording |
| Audio & Camera page: Microphone module (picker popup, level meter, notices strip with action buttons incl. Install driver / open Privacy panes) | page | audio_page.py | NSPopUpButton menus (refresh held while open), level-indicator accessibility role, meters <= 20 updates/s with CA actions off, opens x-apple.systempreferences URLs |
| Audio & Camera page: Camera module (Camera your apps see, Quality, status + Retry, Camera settings help [PC only], notices) | page | camera_section.py | popups, buttons |
| Audio & Camera page: Machines map (tiles, sound/mic/camera route arrows solid/dashed, legend, waiting grey) | page | audio_page.py (`AudioMap`) | CA layers, SF Symbol masks, accessibility group + label |
| Audio & Camera page: selected machine panel (Sound plays on, Low-latency virtual microphone, Inputs/Outputs rows with meters, Test (10 s), Headset switch) | page | audio_page.py | tooltips, per-row accessibility labels |
| Audio driver install administrator prompt | dialog (OS) | audio_driver.py | osascript "with administrator privileges" (or none via sudo beamer-hal) |
| Keyboard page: Keyboard sharing (Mouse only), Modifier keys (Same positions / Same shortcuts), Stays on this Mac (recorder + list + Remove), Another machine's pointer here (pointer ruler 25-400 %), Scrolling (scroll ruler, Reverse) | page | kvm_bridge_app.py, widgets.py (`IgnoredRecorder`, `Ruler`) | NSEvent local monitor for recording, accessibility labels |
| Design page: On screen (Animate crossings on this Mac, Show where the pointer lands) | page | kvm_bridge_app.py | switches |
| Design page: Style (style tiles with hover-to-preview, Style for: edge/corner/notch/shortcut and menu, Notch style Island/Outline, Show at, Keep animating, Length, Size, Frame rate) | page | kvm_bridge_app.py, previews.py, widgets.py (`ChoiceTiles`) | live previews only while page visible and hovered (`TileHover`), 60 Hz cap, keyboard-steppable sliders with accessibility value |
| Design page: Colour swatches | page | kvm_bridge_app.py, widgets.py (`Swatches`) | radio accessibility |
| Design page: Trackpad (Feel it, Tick as the push builds, Ticks at: Every quarter / Halfway / Only when through, Try it) | page | kvm_bridge_app.py | NSHapticFeedbackManager |
| Design page: Appearance (System / Light / Dark for this window) | page | kvm_bridge_app.py | window NSAppearance switch, cross-fade |
| Design page: Match design with (popup) + follow banner (Stop following) | page | kvm_bridge_app.py, widgets.py | popup accessibility |
| Connection page (Hide addresses, Language + Relaunch, Address + secure field, First paired machine's port, Wake-on-LAN state, Connect) | page | kvm_bridge_app.py | NSSecureTextField, accessibility labels |
| Edge glow (glow band / beam comet along pushed edge or corner; flash on cross; driven-side return push) | overlay | kvm_bridge_app.py (`EdgeGlow`) | borderless non-activating NSPanel, transparent, click-through, NSStatusWindowLevel, CanJoinAllSpaces + FullScreenAuxiliary (above full-screen apps), placed on the pushed display, CAGradientLayer (incl. conic), display-link frame rate |
| Crossing effects overlay | overlay | effects_overlay.py | one transparent click-through panel per display touched, NSStatusWindowLevel, CanJoinAllSpaces + FullScreenAuxiliary + IgnoresCycle + Stationary, hidesOnDeactivate off, CPU Core Graphics replay in drawRect each frame, display-link rate, Reduce Motion, dark mode |
| Notch island | overlay | notch_island.py | transparent click-through panel over the notch at NSPopUpMenuWindowLevel (above menu bar), all spaces + full-screen auxiliary + stationary, CAShapeLayer + CASpringAnimation, notch geometry from safeAreaInsets / auxiliaryTop*Area, Reduce Motion variant |
| Notch beam | overlay | notch_beam.py | same panel as island (subclass), CIGaussianBlur layer filter bloom/halo, strokeStart/End animation, Reduce Motion parks comet |
| Identify screens labels | overlay | identify_overlay.py | per-display panel 220 pt centred, NSStatusWindowLevel, click-through, shadow, all spaces + full-screen auxiliary + ignores cycle + stationary, 3 s then close, cannot become key/main |
| Gesture capture panel (invisible 400x400 at cursor pin while redirecting) | overlay | kvm_bridge_app.py (`GestureOverlay`) | NOT click-through (receives magnify/swipe), alpha 1/255, NSStatusWindowLevel, CanJoinAllSpaces, non-activating, orderFrontRegardless, main-screen flip only |
| Haptic ticks / thud | other (feedback) | kvm_bridge_app.py (`Haptics`) | NSHapticFeedbackPatternLevelChange |
| User notifications (Skipped <name>: not connected; Shook home from <name>; Cannot switch - <status>; refusal / took-over texts; Input returned to this Mac; Waking <name>... / is awake / did not wake / Could not send the wake-up packet; Grant both Mac permissions first; Could not save the change; Configuration reloaded / not reloaded) | notice | notices.py, bridge.py, wake.py, kvm_bridge_app.py | UNUserNotificationCenter banner+list+sound even when frontmost, NSBeep, refusal throttle, addresses redacted when Hide addresses is on |
| Login Items System Settings | other (OS pane) | login_item.py | SMAppService.openSystemSettingsLoginItems |
| Log folder in Finder; update URL; website | other | kvm_bridge_app.py | NSWorkspace openURL |
| DMG window (background with arrow, app + Applications) | window (Finder) | dmg/settings.py, dmg/draw_background.py | dmgbuild, @2x background |

### OS touchpoints (mac_app)

Distinct OS APIs called directly from mac_app Python: **203** = 119 C functions + 79 ObjC classes/functions + 5 subprocess tools. The 2 URL schemes and 2 POSIX calls are listed below but not counted. Class/function names only; ObjC selectors on those objects and constants are not counted separately.

| Framework / route | Distinct | Notes |
|---|---|---|
| CoreGraphics drawing (PyObjC Quartz): CGContext*, CGPath*, CGColor*, CGGradient*, geometry | 55 | effects replay (effects_overlay.py), previews, ledger/widgets |
| CoreGraphics events (PyObjC): CGEventTapCreate/Enable, CGEventCreate*/Post/Get*/Set*, CGEventSource* (3), CGEventMaskBit, keyboard unicode string get/set | 19 | bridge.py tap, input_injector_mac.py |
| CoreGraphics display/cursor/window (PyObjC + ctypes): CGDisplayBounds, CGGetActive/OnlineDisplayList, CGMainDisplayID, CGWarpMouseCursorPosition, CGAssociateMouseAndMouseCursorPosition, CGDisplayHide/ShowCursor, CGWindowListCopyWindowInfo, CGBegin/Complete/CancelDisplayConfiguration, CGDisplayVendor/ModelNumber, CGDisplayIsBuiltin/IsOnline | 16 | |
| CoreGraphics TCC: CGPreflightListenEventAccess, CGRequestListenEventAccess | 2 | Input Monitoring |
| Private (ctypes): CGSSetConnectionProperty, _CGSDefaultConnection, CGSConfigureDisplayEnabled / SkyLight SLSConfigureDisplayEnabled, CoreDisplay_DisplayCreateInfoDictionary | 5 | pointer_hide.py, shared_display_mac.py |
| CoreFoundation (PyObjC + ctypes): CFMachPortCreateRunLoopSource, CFRunLoopAddSource/GetCurrent/Run/Stop, CFAbsoluteTimeGetCurrent, CFStringCreateWithCString, CFDataGetBytePtr, CFRelease | 9 | |
| ApplicationServices / HIServices: AXIsProcessTrustedWithOptions, AXUIElementCreateApplication, AXUIElementCopyAttributeValue | 3 | permission + AXFullScreen |
| Carbon (ctypes): TISCopyCurrentKeyboardLayoutInputSource, TISGetInputSourceProperty, LMGetKbdType, UCKeyTranslate | 4 | plus distributed notification TISNotifySelectedKeyboardInputSourceChanged |
| IOKit (ctypes): IOPMAssertionDeclareUserActivity | 1 | wakes display on remote input |
| CoreText: CTFontCreateWithName, CTFontGetGlyphsForCharacters, CTFontCreatePathForGlyph | 3 | |
| libc (ctypes): getifaddrs, freeifaddrs | 2 | hardware_mac.py |
| AppKit + Foundation classes | 50 | NSWindow, NSPanel, NSView, NSScreen, NSEvent, NSMenu/Item, NSAlert, NSPasteboard, NSHapticFeedbackManager, NSWorkspace, NSProcessInfo, NSUserDefaults, NSDistributedNotificationCenter, NSTimer, NSRunLoop, NSStackView, NSScrollView, NSSlider, NSPopUpButton, NSTextField, NSSecureTextField, NSImage(+SymbolConfiguration), NSBezierPath, NSGradient, NSGraphicsContext, NSBitmapImageRep, NSTrackingArea, NSAnimationContext, NSLayoutConstraint, etc. |
| AppKit C functions | 11 | NSBeep, NSAccessibilityPostNotificationWithUserInfo, NSRectFill, geometry helpers |
| NSStatusItem (through rumps internals `_nsapp.nsstatusitem`) | 1 | rumps 0.4.0 also owns NSStatusBar and timers |
| QuartzCore: CALayer, CAShapeLayer, CAGradientLayer, CATextLayer, CABasicAnimation, CAKeyframeAnimation, CASpringAnimation, CATransition, CATransaction, CAMediaTimingFunction, CAFrameRateRangeMake | 11 | plus NSScreen/NSView `displayLinkWithTarget:selector:` (CADisplayLink, macOS 14+) |
| CoreImage: CIFilter (CIQRCodeGenerator, CIGaussianBlur) | 1 class / 2 filters | |
| UserNotifications: UNUserNotificationCenter, UNMutableNotificationContent, UNNotificationRequest, UNNotificationSound | 4 | |
| ServiceManagement (objc.loadBundle): SMAppService | 1 | mainAppService, register/unregister, status, openSystemSettingsLoginItems |
| Subprocess to OS tools | 5 | `/usr/bin/sudo -n /usr/local/libexec/beamer-hal install`, `/usr/bin/osascript -e 'do shell script /bin/bash ... with administrator privileges'`, `/bin/bash` (install-driver.sh), `/bin/sh -c ... exec /usr/bin/open <bundle>` (relaunch) |
| URL schemes | 2 | x-apple.systempreferences Privacy_Microphone, Privacy_AudioCapture |
| POSIX misc | 2 | fcntl.flock single-instance lock, AF_UNIX socket instance signal |

Raw/undocumented event usage: tap masks built from raw NSEventType 14 (NSSystemDefined media keys), CGEvent types 29/30 (Gesture/DockControl) with private fields 110/123/132; NSEvent `otherEventWithType` systemDefined subtype 8 for media-key injection; event source user data used as INJECTED_MARK.

Build/install scripts (not counted above): codesign, xcrun notarytool, xcrun stapler, lipo, ditto, xattr, tccutil, killall, dmgbuild, py2app, ssh.

### Phone-peer code (mac_app)

- machines.py:17 `PLATFORMS` includes `"ios": "iPhone"`, `"android": "Android"`.
- machines.py:33 `Row.phone`; machines.py:51 `phone = port == 0`.
- machines_panel.py:92-93 "Pair a phone" button, hidden while `feature_flags.PAIRING_QR_VISIBLE` is False.
- machines_panel.py:363, 369, 377, 401, 482, 484, 489: phone rows skip address, Pin address, shortcut-cycle, directions; phone-specific In use tooltip.
- machines_panel.py:565-569 `pair_phone()` opens the sheet and starts the QR host flow.
- machines_panel.py:817 QR caption "The same code as a QR, for a phone to scan."; 830-839 `_set_qr` blanks the QR when the flag is off.
- qr.py (whole file, 52 lines) exists for that QR.
- settings_store.py:645 `peerlist.normalise_phones(settings)` (zero-port senders lose receiving-only fields).
- input_injector_mac.py:251 docstring: `us` key position "absent from a phone's" (phone-typed characters planned by layout, not US position).
- Phones filtered out of desktop logic through `peerlist.desktops` (port != 0): bridge.py:1034, 1050, 2412; windows_input.py:92, 161, 219; ledger.py:764; kvm_bridge_app.py:1748, 1789, 1914, 2497, 2612, 2667, 2699, 3101, 3136, 3152, 4979, 5449, 5506.
- Tests: test_phone_settings (6), test_phone_ui (10), test_machines_panel:470, 492; test_settings_v6:848; test_machines:120.

### Linux-only parts (mac_app)

None. Linux shows up only as a peer: machines.py:17 and ledger.py:429 platform names; input_injector_mac.py:596 uses PC lines-per-notch scroll for `windows`/`linux` peers (test_wayland_scroll_mac covers a synthetic Linux scroll arriving at the Mac injector); dock_visibility.py:4 comment.

### Notes

- Fork-added (feature flags in core/feature_flags.py, all True in this fork, False upstream): AUDIO (audio_mac.py, audio_driver.py, audio_page.py, Audio page entry in pages.py:28-35, entitlements, NSMicrophone/NSAudioCapture usage strings, build_dmg.sh audio staging), CAMERA (camera_section.py, page renamed "Audio & Camera", tray camera items kvm_bridge_app.py:5096-5837, NSCameraUsageDescription + reaction gestures off), SHARED_DISPLAY (shared_display_mac.py via audio_mac.py:72-78, tray display submenus kvm_bridge_app.py:5330-5368). Shared displays ride the `audio` capability. Display and camera tray code is guarded with getattr/hasattr and catches its own exceptions.
- Hidden flags: `PAIRING_QR_VISIBLE = False` hides "Pair a phone" and the QR. The phone pairing UI is built but never shown. CLI: `--config <path>`, `--hidden` (start in menu bar). Env: `RESOURCEPATH` (bundle VERSION/resources), `BEAMER_BUILD` (CFBundleVersion), `BEAMER_SIGN_IDENTITY` (build_dmg.sh). Dev-only install path: `/usr/local/libexec/beamer-hal` with a NOPASSWD sudo rule. If it exists, no admin prompt.
- Private / undocumented APIs: CGSSetConnectionProperty "SetsCursorInBackground" (pointer hide), CGSConfigureDisplayEnabled / SLSConfigureDisplayEnabled + CoreDisplay_DisplayCreateInfoDictionary (shared displays, session-only, so a restart brings the display back), raw event types and fields above.
- rumps is patched at runtime: `objc.classAddMethods` adds `applicationShouldHandleReopen:hasVisibleWindows:` to rumps' NSApp delegate; `_nsapp.nsstatusitem` / `_icon_nsimage` / `setStatusBarIcon` internals are poked; the status-item menu is detached so left click opens the window.
- Best-effort or dead paths: the tap-based gesture capture (bridge.py `_handle_gesture_event`) is documented as never firing on the macOS 27 beta (kvm_bridge_app.py:197-214). GestureOverlay replaces it, and 3/4-finger system gestures cannot be captured. `no_unlock.py` is a 17-line stub (`is_locked` returns None, `ensure_unlocked` returns False). The camera "Camera settings" help row is PC-only ("the model never gives the Mac one", camera_section.py:100). `test_core.py` holds 0 tests and only loads core/tests. `test_effect_size_raster.py` holds 0 tests and re-exports core's NativeSizeRasterTests.
- macOS 27 LAN workaround: `Beamer Tunnel.command` plus bridge.py `_connect_via_ssh_fallback` (127.0.0.1:24822 -> PC 24820 over ssh -L). The error string tells the user to open the .command.
- setup.py `py_modules` leaves out frame_clock, instance_signal and ledger (py2app's import scan still finds them). It lists theme and tokens, which live at the repo root, not in mac_app. Fonts (Hanken Grotesk, B612 Mono) are taken from win_app/assets. Locales `*.po`; CFBundleLocalizations en, it. LSMinimumSystemVersion 13.0.
- Upstream identity kept: About/footer link `https://kalkmancode.co.uk/beamer`, bundle id `uk.co.kalkman.beamer`, copyright Toby Kalkman.
- App-level behaviours that are easy to miss in a port: App Nap opt-out (`NSActivityUserInitiatedAllowingIdleSystemSleep | LatencyCritical`); a full-screen app holds crossing (AXFullScreen, falling back to a layer-0 window covering a display); notch detection drives the "notch" crossing method; titlebar double-click follows the system preference; language change needs a relaunch; Hide addresses redacts IPs/MACs in the window, the menu and notifications; refusal notices are throttled; config.json from 1.4.x is read, never written; a 1 MB x 3 rotating log goes to ~/Library/Logs/Beamer; core.crash_log is installed.
- Install-Beamer-Mac.command runs `tccutil reset` on Accessibility, ListenEvent, LocalNetwork, Microphone, AudioCapture and Camera, only for ad-hoc signed builds.

## win_app/

Scope: `win_app/` at `main` (fffbede). 80 non-test `.py` files, 32,181 lines (`wc -l`). Windows and shared parts: 62 files, 25,910 lines. Linux-only parts: 18 files, 6,271 lines. Tests: 154 `test_*.py` files with 2,167 `def test_` in `win_app/tests` (the counts below come from `grep -c 'def test_'`). UI toolkit: **PySide6** (`PySide6-Essentials>=6.11.1`). No PyQt is used.

### Features (win_app, Windows parts)

| Feature | What it does (one line) | Files | Lines (non-test, `wc -l`) | Tests (test files + count of `def test_`) |
|---|---|---|---|---|
| App shell / settings window / tray / startup | Monolith: `WindowsApplication` (the window, every page builder, the tray, the pairing flow, settings sync, profiles, effects dispatch, pointer hiding, the veil), `main()`, single instance, CLI verbs. Sub-features are listed below the table. | kvm_bridge_win.py | 6264 | 29 files, 332: single_instance 14, power_throttling 4, update_notice 18, unreadable_settings 4, settings_layout_acceptance 19, idle_refresh 16, hide_addresses_win 3, hide_pointer_setting 10, home_shortcut_setting 2, page_column_geometry 2, real_font 1, same_on_both 54, profiles_sync_qt 10, profiles_tray_qt 2, profiles_page_qt 15, pc_three_machines 85, wide_layout 8, crossing_name_elision 3, crossing_outcomes 4, crossing_fix_pass 11, crossing_matrix 14, crossing_page_parity 10, design_sliders 5, design_tiles 5, style_for 5, qt_event_reentry 1, crossing_header_resize 2, audit_regressions 5, settings_behaviour 0 |
| Settings store | settings.json load, save and validation. Migrates the 1.4.x config.json. Flat `Config` view, peer book, profile backups, phone normalisation. Path is `%LOCALAPPDATA%` (or `XDG_CONFIG_HOME`). | app_config.py | 1164 | app_config 37, settings_migration 74, arrangement 8, phone_settings 5, rig_map_rc13 2 = 126 |
| OS-parts switch | Picks the backends once at import (capture, desktop, clipboard, autostart, firewall, unlock, injector). Exposes the `WAYLAND` flag. | platform_parts.py | 85 | none dedicated. It is exercised by the shell tests and by linux_start. |
| Capture (LL hooks + Raw Input) | WH_KEYBOARD_LL and WH_MOUSE_LL on their own thread with a message loop. Raw Input (RIDEV_INPUTSINK) on a message-only window for relative motion. WinEvent hook for desktop switches. Swallows input while redirected. Holds the edge trigger, the double-tap or hold `Trigger`, VK to key names, ToUnicodeEx dead keys, the internal-seam clip and Caps Lock reads. | capture_win.py | 1077 | capture_win 41, capture_win_recovery 15, capture_win_seam 6, ignored_inputs 17, ignored_page 7, parity 27, two_scripts 14, hook_key_repeat 8, global_shortcut 4, same_os_keys 2 = 141 |
| Injection (SendInput) | Keys by VK and scan code with the US place or the layout's place (VkKeyScanW, MapVirtualKeyW). Also Unicode text, relative mouse moves, buttons and X buttons, wheel and hwheel with pixel-to-notch scaling, and release-all. Marks its events with INJECTED_MARK 0xBEA3. Gesture plan with chord fallback. GameInput foreground recovery runs the `Beamer-GameInputRecovery` scheduled task. Problem reports. | input_injector.py | 786 | input_injector 54, stuck_modifier_on_drop 1, horizontal_scroll 5, no_uac_runtime 5 = 65 |
| Touchpad gesture injector | Synthetic Precision Touchpad (CreateSyntheticPointerDevice2 / InjectSyntheticPointerInput, PT_TOUCHPAD, Win11). Plays 3 or 4 finger swipes so Windows' own gestures fire: 12 steps at 10 ms each. | touchpad_injector.py | 220 | covered inside test_input_injector (5 of its 54 are gesture tests) |
| Brightness keys | `brightness_up` and `brightness_down` keys from a peer step the built-in display through WMI `WmiSetBrightness`, on a worker thread. This is **not** DDC: external monitors are untouched. | brightness_worker.py, display_brightness.py | 104 | brightness_worker 4 |
| Input blocker diagnostics | When SendInput is refused, names the foreground window's process, elevation and integrity level, and checks for GameInput's SYSTEM window. Builds the user sentence. | input_blocker.py | 180 | input_blocker 17, gameinput_foreground 17 = 34 |
| Desktop geometry + pointer hiding | EnumDisplayMonitors, Get/SetCursorPos and the display map. Full-screen app detection (SHQueryUserNotificationState plus a check that the foreground window covers its monitor). Button state. Hides the system pointer by blanking every system cursor (SetSystemCursor), with an input-desktop guard and a restore after a crash marker. `set_click_through` (WS_EX_TRANSPARENT and LAYERED). | desktop_win.py | 396 | full_screen_win 1 (and hide_pointer_setting, counted under the shell) |
| Clipboard | CF_UNICODETEXT and CF_DIB to and from PNG, with retrying OpenClipboard and change stamps (GetClipboardSequenceNumber). | clipboard_win.py | 430 | clipboard_win 34 |
| Sender / outbound links | This PC's input going to peers. One `LinkSender` per peer plus `LinkSet`, driving the owner state machine. Handles the clipboard on switch, the pointer pin and landing, edge pressure, shake-to-home, wake-on-LAN (ARP lookup through core/wol), the shortcut target, and routing of `audio_*`/`camera_*`/`display_*` messages (fork). shake_stroke picks the per-backend shake stroke. | sender.py, shake_stroke.py | 2008 | links 128, links_loopback 9, link_set 7, sender_motion_flow 8, sender_shake 12, replay 1, capture_sender 3, key_up_after_switch_back 7, focus_sender 1 = 176 |
| Hardware address | The MAC of the adapter that routes towards a host, through GetAdaptersAddresses, for the handshake `hw` field. | hardware_win.py | 115 | hardware_win 10 |
| Crossing ledger | Per-machine rows of crossing intent: edge chips, a corner grid, a jump-key recorder and a cycle checkbox. Gives screen readers table, row and cell roles through a QAccessible factory. | crossing_ledger.py | 939 | crossing_ledger 14, profiles_canvas_qt 15, user_name_locale_regression 2 = 31 |
| Arrangement canvas (atlas) + push strip | Draggable machine-tile map with zones, thirds, corners, inline rename and a cycle box. `PushStrip` is the resistance slider with a live pressure fill. `ArrangementDiagram` is test-only. | diagram.py | 1800 | atlas_canvas 24, atlas_crossing_colours 7, atlas_map_feel 5, map_ticks 20, tile_resize_qt 16, part_edge_corners_qt 12, scroll_wheel_focus 9, ui_geometry 5 = 98 |
| Pages model (pure) | Page list and order, sidebar dots, footers, sentences (crossing state, ways summary, share), layout geometry, address redaction, the Linux permission state model. | pages_win.py | 605 | pages_win 87, i18n_pages 3, profiles_labels 2, click_focus 5 = 97 |
| Peers view (pure) | One tone, one state word and one sentence for each paired machine's link state. Platform names include iPhone and Android. | peers_view.py | 79 | peers_view 13, phone_ui 13, dormant_rows 11 = 37 |
| Machines list + pairing sheet | `PeerRow` with its switches (send, allow, in use, left out, pin address, cycle), rename, remove and re-pair. `MachinesModule` with Pair nearby, Pair a phone (hidden) and Pair a machine. `PairingSheet` holds the 6-digit code with a countdown drain, the code entry, the QR, the nearby list and the SAS confirm. | machines_win.py | 1033 | pairing_window 56, pair_code_caret 3, pair_code_rendering 6, pair_nearby_ui 6, left_out_ui 2, eliding_text 8 = 81 |
| Pairing QR | Turns the WIRE §6 text into a segno module grid, level M. | qr_code.py | 27 | qr_code 2 |
| Cursor overlay | Draws the user's system arrow in a topmost click-through window when a remotely driven PC has no mouse (SM_MOUSEPRESENT is 0, or the cursor is hidden). | cursor_overlay.py | 164 | cursor_overlay 16, cursor_overlay_native 1, cursor_overlay_ownership 5 = 22 |
| Edge glow | Return-edge `glow` (a band that tracks pressure, then flashes) or `beam` (a line with a travelling comet), on the monitor that owns the edge. Re-asserts HWND_TOPMOST over the taskbar. Holds the preview loop for the Design tiles. `GlowPreview` is unused by the app. | edge_glow.py | 511 | edge_glow 17 |
| Effect overlay + previews + frame clock | Replays core/effects frames (crossing, landing and switch-arrival styles) into one click-through window sized to each frame. Design-page stills that play on hover. Frame timer interval comes from the screen refresh rate divided by the Frame rate setting. Reads Windows reduced motion and dark mode. | effect_overlay.py, effect_previews.py, frame_clock.py | 971 | effect_overlay 26, effect_overlay_fixed 5, effect_fires 25, effect_preview_timeline 6, style_tiles 4, frame_clock 5 = 71 |
| Identify screens | Shows a big number on every screen for 3 s, locally or when a peer asks. | identify_overlay.py | 323 (contains Linux helpers, see §2) | identify_screens_qt 19 |
| Audio + microphone (fork) | `PcAudio`/`LinkPeers` glue to core/audio's AudioService and the external `beamer-audio.exe` helper. The Audio page has a mic select with a level meter, notices, a machines map with sound and mic routes, the selected machine's outputs and inputs, and a low-latency virtual-mic switch. | audio_win.py, audio_page_win.py | 1411 | audio_page_win 29 (+ core/tests/test_audio_link_gating 23) |
| Camera + vcam install (fork) | The Camera module on the Audio & Camera page: camera selector, Quality, status, Retry, and per-machine rows. Elevated install, update, repair and removal of the Beamer Camera DLL through `beamer-audio.exe vcam-install` and ShellExecuteEx "runas". | camera_section_win.py, vcam_install.py | 740 | camera_section_win 27, vcam_install 38 = 65 (+ core/tests/test_camera_link_gating 26) |
| Shared displays (fork) | Lists monitors and turns one off or on with the CCD API (QueryDisplayConfig, SetDisplayConfig, DisplayConfigGetDeviceInfo). Nothing is persisted to Windows' display database. Ownership is chosen in the tray submenus. **No DDC/CI in win_app.** | shared_display_win.py | 151 | **none** |
| Firewall | Reads, explains and repairs the Windows Firewall rules for this exe through PowerShell NetSecurity cmdlets: receiver TCP port, plus pairing UDP and TCP. Can set a network to Private. | firewall_win.py | 403 | firewall_win 50, windows_tool_encoding 6, firewall_first 1 = 57 (+ core/tests/test_firewall_contract 1) |
| Autostart | One scheduled task per user, registered from XML through schtasks (working directory set, no 72 h limit). Migrates and removes the legacy `Beamer` task. User and SID lookup through GetUserNameExW and WTS session info. | autostart_win.py | 277 | autostart_win 14, upgrade_autostart 14, no_uac_autostart 3 = 31 |
| Admin mode | Opt-in "Control administrator windows" stored in HKCU\Software\KalkmanCode\Beamer\RunAsAdministrator. Elevated helper (`--set-admin-mode`, `--set-admin-startup`) through ShellExecuteEx runas. Highest-run-level task. Relaunches elevated or unelevated, the latter through the Explorer shell COM ShellExecute trick. | admin_mode.py | 346 | admin_mode 35, admin_mode_ui 10 = 45 |
| Unlock | When the console is locked (logonui.exe in the console session), signals an external credential provider's `Global\RoomDeckUnlock.<…>` event and polls for up to 10 s. The provider is not in the repository. | unlock_win.py | 205 | unlock_win 11 |
| Quit signals | SIGTERM and SIGINT call Qt quit() through set_wakeup_fd and a QSocketNotifier. | quit_signals.py | 57 | quit_signals 3 |
| Theme / tokens / title bar / widgets / motion | Vernier tokens (a byte copy of the root tokens.py), the QSS theme (light and dark), fonts (B612 Mono, Hanken Grotesk). DWM caption, text and border colours plus immersive dark mode. Custom widgets: Module, Eyebrow, Led, Drain (60-segment countdown), Switch, Ruler, Choice, ChoiceTiles, TileGroups, SwatchGroups, InputRecorder, JumpKeyRecorder, Sidebar, PageButton, ElidingLabel, ElidingButton, FormRow, SideBySide, and keyboard navigation. Motion: 200 ms OutCubic, cross-fades, the sliding choice Ring, off when animations are disabled. | theme.py, tokens.py, titlebar.py, widgets.py, motion.py | 2883 | theme 5, tokens 1, appearance_contrast 2, choice_fill 1, empty_choice_layout 1, layout_primitives 15, locale_text 1, sidebar_lifecycle 7, qt_polish 13, motion 6, colour_pixels 3 = 55 |
| Tray glyph | Draws the two-screen glyph for the drive state with a receiver-state badge at 16, 20, 24, 32, 48 and 64 px. Picks its ink from the taskbar theme (registry SystemUsesLightTheme). | tray_glyph.py | 114 | tray_icon 17 |
| Portable build | The portable entry point fails closed without the `Beamer.portable` marker (native MessageBoxW). A PyInstaller runtime hook sets up storage before QtCore loads. A passphrase vault and input-controller lease live in kvm_bridge_win. | portable_entry.py, hooks/portable_storage.py | 42 | portable 31 (+ core/tests/test_portable_packaging 4) |

**Non-Python build and packaging (Windows):**

| Item | What it does | Files | Lines |
|---|---|---|---|
| PyInstaller onefile | Builds `Beamer.exe`: windowed, `uac_admin=False`, bundles every `win_app/*.py` and `core/*.py` as hidden imports, plus assets, VERSION, locales/*.po and identify_layer_shell.py. Drops ucrtbase and api-ms-win-*. | build.spec | 60 |
| PyInstaller portable (COLLECT) | Folder build from portable_entry.py with the runtime hook. | build_portable.spec, build_portable.ps1 | 78 + 100 |
| Build script | Builds the venv, runs the tests (`BEAMER_SKIP_TESTS=1` skips them), builds the native audio helper (`native\audio\build-win.cmd`), runs PyInstaller, and with `-Release` runs Inno Setup into docs\beamer-releases. Refuses a dirty tree. | build_win_app.ps1 | 132 |
| Inno Setup installer | Per-user install to `{localappdata}\Beamer`, `PrivilegesRequired=lowest`, AppMutex `Beamer.Receiver`, x64compatible, optional signing. Ships beamer-audio.exe and camera\BeamerCameraSource.dll when they are built. Optional "Start at sign-in" task (`--enable-autostart`). Runs the elevated maintenance step. Runs `beamer-audio.exe restore`. The uninstaller runs `vcam-remove` and maintenance `-Remove`. | Beamer-Setup.iss | 158 |
| Elevated maintenance | Removes old elevated tasks, sets protected ACLs, registers and removes the `Beamer-GameInputRecovery` SYSTEM task (restarts GameInputSvc), cleans up Beamer Camera (HKLM COM class, %ProgramFiles%\Beamer\Camera, %ProgramData%), and deletes trees without following links. | Install-Maintenance.ps1, Invoke-InstallMaintenance.ps1 | 310 + 37 |
| Dev installer over SSH | Non-Inno install used by dev/deploy.sh. Copies with retries, handles the helper and camera DLL, starts through the scheduled task. | Install-Beamer.ps1 | 143 |
| Deps / misc | requirements-win.txt (PySide6-Essentials, pyinstaller, hooks-contrib, PyNaCl, pywin32, segno), config.example.json, .gitignore, Beamer.ico | — | 6 / 5 / 8 |
| assets/ | B612Mono Regular and Bold, HankenGrotesk Variable (OFL texts) | — | binary |
| renders/ | qt-pairing-dialog.png (a reference render, not shipped) | — | binary |

**Sub-features inside kvm_bridge_win.py** (line ranges are approximate):

- Logging: a RotatingFileHandler, plus `BEAMER_DEBUG` for per-key debug logging, at 216-240. crash_log is installed at about 6125.
- StatusBridge: cross-thread Qt signals for status, alert, veil, update and so on, at 315-350.
- Second-launch activation: QLocalServer and QLocalSocket, with a pipe name scoped to the Windows session, at 351-414.
- Window shell: sidebar, stacked pages, update banner, footer with Quit, Ctrl+Q and Ctrl+1..9, at 824-967.
- Overview page, 984-1528: machines, name and rename, the Linux permissions module, the Tray toggle, Updates (auto check, check now, download, what's new, dismiss), Link, and Keyboard and mouse (pause crossing, hold edges while full screen, hide pointer, admin control). Also the full-screen poll and alert routing to tray balloons, with refusal rate-limiting and a beep.
- "Same on all machines" settings sync and Revert: 1529-1735, 1904-1923 and 2088-2227.
- Design follow ("Match design with"): 1749-1903.
- Identify screens, local and remote: 1924-1947.
- Crossing desk publish and announce to peers: 1948-2064.
- At sign-in: 2228-2286.
- Crossing page, 2287-3733: profiles (tabs, new, rename, delete, override, archive, desk), the ways module with AtlasCanvas, the ledger, Way home (shake), the drag guard, Resistance with live pressure, and Shortcut (trigger recorder, double-tap or hold, tap interval, target cycle, jump keys).
- Keyboard page, 3434-3600: mouse only, modifier style (semantic, positional or Mac layout), Stays on this PC (ignored inputs), pointer speed and reverse scrolling.
- Design page, 3737-4057: animate, landing, style tiles, colour, length, size, frame rate, appearance and design follow.
- Debounced saves (SETTLE_MS = 300): 4048-4122.
- Pairing, 4126-4764: PairingService, code, nearby with SAS, re-pair, phone, QR cache.
- Peers state and per-peer switches: 4144-4560.
- Connection page, 4765-4936: hide addresses, language with Relaunch, address readout, port with Save and reconnect.
- Firewall module: 4937-5022.
- Theme, title bar and appearance cross-fade: 5023-5058.
- Tray menu: 5059-5173 and 4423-4484.
- Reload config: 5174-5248.
- Start and wiring, 5253-5457: receiver (core.receiver.LinkResponder), the audio service (fork), capture and sending, hardware learning, owner changes, cursor overlay.
- Idle and driving pointer hiding plus the pointer veil: 5475-5548.
- show_window, About, close-to-tray and quit: 5558-5682.
- Effect and glow dispatch: 5688-5801.
- Status and window refresh: 5802-5929.
- Startup, 5930-6264:
  - CLI.
  - `Local\Beamer.Receiver` mutex (QLockFile on Linux).
  - `Local\Beamer.InputController` lease, which keeps portable input inactive while the installed copy runs.
  - Relaunch wait of 15 s.
  - ABOVE_NORMAL priority and the EcoQoS opt-out.
  - Portable passphrase vault.
  - `--import-probe`, `--enable-autostart`, `--uninstall-cleanup` (netsh deletes the 3 firewall rules) and `--set-admin-*` dispatch.

**Tests in win_app/tests that cover core/, not win_app code:** return_edge 27, effect_layouts 19, pairing 6, way_home 4, effect_size_raster 0, core 0. Total 56.

### Linux-only parts (win_app)

| Feature | What it does (one line) | Files | Lines (non-test, `wc -l`) | Tests (test files + count of `def test_`) |
|---|---|---|---|---|
| X11 capture | XInput2 raw events on the root window, plus an active grab of every master device while input is away. Same Hooks and Trigger API. | capture_x11.py | 871 | capture_x11 54 |
| X11 seam barriers | Directional XFixes pointer barriers (CreatePointerBarrier and DestroyPointerBarrier, hand-built requests) at internal monitor seams. | x11_seams.py | 264 | x11_seams 15 |
| X11 injection | XTest fake input, with characters typed through the XKB layout group. | inject_x11.py | 659 | inject_x11 30 |
| XKB keymap | libxkbcommon(-x11) through ctypes: keysym per key, modifiers and group. Also built from the libei keymap text on Wayland. | xkb_x11.py | 178 | via inject_x11 and inject_portal (+ core/tests/test_linux_accent_keymaps 2) |
| X11 clipboard | QClipboard marshalled to the GUI thread. | clipboard_x11.py | 240 | clipboard_x11 20 |
| X11 desktop geometry | RandR 1.5 monitors, query and warp pointer, XFixes hide and show cursor. | desktop_x11.py | 169 | desktop_x11 22 |
| Wayland capture | InputCapture portal plus a libei receiver. Barriers on zone edges, push-through and Release. | capture_portal.py | 1137 | capture_portal 83 |
| Wayland injection | RemoteDesktop portal plus a libei sender, on one session-owning thread. | inject_portal.py | 878 | inject_portal 43 |
| Wayland clipboard | The portal Clipboard on the RemoteDesktop session. | clipboard_portal.py | 174 | clipboard_portal 5 |
| Wayland desktop geometry | Last-known pointer position as the truth, plus zones. | desktop_portal.py | 308 | desktop_portal_cache 10 |
| Portal D-Bus layer | jeepney bus, Request/Response handling, sessions, `sandboxed()` and `notify()`. | portal.py | 295 | portal 11 |
| libei binding | ctypes over libei for the receiver and sender contexts. | libei.py | 367 | via capture_portal and inject_portal (+ core/tests/test_flatpak_libei_path 1, test_wayland_scroll_wire 10) |
| Global shortcut | GlobalShortcuts portal binding (Super+Shift+B sends input out). | global_shortcut_portal.py | 113 | global_shortcut_portal 7 |
| XDG autostart | A `$XDG_CONFIG_HOME/autostart` .desktop entry that honours Hidden and X-GNOME-Autostart-enabled. | autostart_x11.py | 120 | autostart_x11 20 |
| XWayland effects proxy | Runs the crossing effect and glow windows in an `xcb` child process when the compositor allows edge overlays. | effect_overlay_x11.py | 305 | effect_overlay_x11 14 |
| Layer-shell identify helper | GTK layer-shell POPUP windows run with `/usr/bin/python3`. It is still bundled into the Windows exe by both specs. | identify_layer_shell.py | 128 | none |
| Desktop identity | XDG_CURRENT_DESKTOP to `kde`, `gnome` and so on. | linux_desktop.py | 14 | linux_desktop 3 |
| Stand-ins | No-op firewall and unlock modules for Linux. | linux_stand_ins.py | 51 | (core/tests/test_firewall_contract 1) |
| Linux shell glue (tests only) | — | — | — | linux_permissions 13, linux_start 3, wayland_sender 4 |
| Deps | python-xlib 0.33, six, jeepney 0.9.0 (plus the system libei and libxkbcommon) | requirements-linux.txt | 11 | — |

Linux-only total: 18 files, 6,271 lines. Linux test files total 357 tests.

**Linux branches inside shared files:**
- kvm_bridge_win.py:
  - 76, 191-192: XWayland effects import and check.
  - 359-364: socket path in the runtime directory.
  - 567, 1007, 1092-1139: Linux permissions module.
  - 674: mouse-only unsupported on Wayland.
  - 713-743: X11 grab_while, Wayland barriers, push_out, GlobalShortcut, injector permissions.
  - 857, 1222-1280: Flatpak-sandboxed update opens a page instead of downloading.
  - 1046: "Only on this computer" wording.
  - 1375-1388, 1469: Wayland pointer and crossing notes.
  - 1521-1525: portal notification instead of a tray balloon.
  - 2304-2310: KDE and GNOME tray note.
  - 2416-2417: "Seam crossings need X11".
  - 2600: platform name.
  - 3257, 3444: modules hidden on Wayland.
  - 3605-3621: Wayland shortcut note and the Super+Shift+B switch.
  - 3765: appearance note.
  - 4002: Wayland effects unavailable.
  - 4475: tray machine entries disabled on Wayland.
  - 4925-4933: X11 `configure_seams`.
  - 4938-4940: "Linux firewall".
  - 5560-5570: XDG_ACTIVATION_TOKEN.
  - 5695-5785: EffectOverlayX11Proxy selection.
  - 5935, 6094-6096: `--identify-x11`.
  - 5988-5991: QLockFile.
  - 6105: activation token.
- identify_overlay.py: 84-85 (X11 ToolTip bypass flags), 130-175 (layer-shell and X11 helper processes), 251-323 (`x11_screen_regions`, `run_x11_helper`, `__main__`).
- sender.py: 70 (OWN_PLATFORM), 587-588 (key mapping to a Linux target).
- audio_win.py: 28-40 (Linux never announces audio).
- machines_win.py: 488.
- diagram.py: 441.
- pages_win.py: 131-132 (`linux_permissions_visible`) and 88-128 (permission state model).
- app_config.py: 230 (XDG_CONFIG_HOME).
- capture_win and input_injector: pure tables borrowed by capture_x11 and inject_x11 (platform_parts docstring).

### UI surfaces (win_app)

"Qt default DPI" means PySide6 6.x per-monitor-v2 awareness. No DPI API is called directly. Nothing in win_app uses Mica, acrylic or SYSTEMBACKDROP. The main window is **not** frameless: it keeps the native caption and only recolours it through DWM.

| Surface | Kind | File | Needs from toolkit |
|---|---|---|---|
| Settings window (`WindowsApplication`) | window | kvm_bridge_win.py:514 | Native title bar recoloured with DwmSetWindowAttribute (caption 35, text 36, border 34, immersive dark 20/19). Close hides to the tray. Ctrl+Q and Ctrl+1..9. Cross-fade of the body on appearance change (200 ms). 250 ms refresh timer. Qt default DPI. 8 accessible names set here. |
| Sidebar + page buttons + link LED + version/website foot | widget | widgets.py:1493, 1599 | Custom-painted icons (including the fork's speaker icon), dots per page, width-responsive. Accessible names in widgets.py: 18 total. |
| Update notice banner (Get update / What's new / Dismiss) | in-window notice | kvm_bridge_win.py:851 | Show and hide with motion. |
| Footer (note + Quit Beamer) | in-window | kvm_bridge_win.py:871 | The note hides under 640 px. |
| Overview page: Machines (PeerRow per peer; Pair nearby, Pair a phone (hidden), Pair a machine) | page section | machines_win.py:332, 556 | Inline rename QLineEdit, switches, tooltips. 16 accessible names in machines_win. |
| Overview: This machine (name, rename) | section | kvm_bridge_win.py:1016 | — |
| Overview: Pairing sheet (code + 60-segment drain + PairCodeEdit + QR + nearby list + SAS Confirm) | in-window sheet | machines_win.py:657, QrView 625 | The countdown updates on the 250 ms window refresh. Accessible names: "Pairing code {digits}", "Time left", "Nearby pairing status". |
| Overview: Link | section | kvm_bridge_win.py:1292 | — |
| Overview: Keyboard and mouse (Pause crossing, Hold edges while full screen, Hide pointer, Control administrator windows) | section | kvm_bridge_win.py:1323 | — |
| Overview: Settings (Same on all machines groups, Revert) | section | kvm_bridge_win.py:1529 | Notes fade with motion. |
| Overview: Linux permissions (**Linux-only**) | section | kvm_bridge_win.py:1092 | — |
| Overview: At sign-in | section | kvm_bridge_win.py:2228 | — |
| Overview: Tray (Show in tray) | section | kvm_bridge_win.py:1140 | — |
| Overview: Updates (auto check, Check, Download, What's new) | section | kvm_bridge_win.py:1161 | — |
| Crossing: Ways (profile tabs, New/Rename/Delete…, name row, Same ways switch, AtlasCanvas, Identify screens) | section | kvm_bridge_win.py:2344; diagram.py:343 | Canvas drag, tile animation (motion, 200 ms), a QToolTip, 8 accessible names in diagram.py. Narrow layout switches direction under 420 px. |
| Crossing: profile override (Keep own / Earlier ways / Use desk's / Share mine) | section | kvm_bridge_win.py:2475 | — |
| Crossing: ledger (CrossingLedger, Back to This PC) | section | crossing_ledger.py:857; kvm_bridge_win.py:2441 | Custom QAccessible factory for table, row, header and cell roles. 13 accessible names. |
| Crossing: Way home (Shake to come home, sensitivity) | section | kvm_bridge_win.py:2521 | — |
| Crossing: This machine's pointer (no crossing while dragging) | section | kvm_bridge_win.py:2805 | — |
| Crossing: Linux tray note, seam note (**Linux-only**) | note | kvm_bridge_win.py:2304, 2416 | — |
| Crossing: Resistance (PushStrip) | section | kvm_bridge_win.py:3368; diagram.py:1632 | Live pressure poll every 33 ms (about 30 Hz). Flash timer 120 ms. |
| Crossing: Shortcut (trigger recorder, Double-tap/Hold, Time between taps, Goes to; Wayland Super+Shift+B is **Linux-only**) | section | kvm_bridge_win.py:3599 | Key recording. |
| Audio (& Camera) page: Audio unavailable | section | audio_page_win.py:798 | The service lookup timer polls every 1000 ms. |
| Audio: Microphone (MenuSelect + LevelMeter) | section | audio_page_win.py:802, 241, 571 | QMenu popup. The meter redraws on each level push from the service. 9 accessible names in audio_page_win. |
| Audio: notices (NoticeView) | in-window notices | audio_page_win.py:704 | — |
| Audio: Camera module (selector, Quality, status/Retry, rows, Install/Remove) | section | camera_section_win.py:224 | 6 accessible names. |
| Audio: Machines (AudioMap + legend) | section | audio_page_win.py:320, 832 | Click and arrow-key selection. |
| Audio: selected machine panel (output, Low-latency virtual microphone, Microphones/Outputs rows) | section | audio_page_win.py:860, DeviceRow 646 | — |
| Keyboard page: Keyboard sharing (Mouse only; hidden on Wayland) / Modifier keys / Stays on this PC / Another machine's pointer here | sections | kvm_bridge_win.py:3434-3530 | Input recorder. |
| Design page: On screen / Style (tiles with EffectStill and SwitchStill, Length, Size, Frame rate) / Colour (swatches) / Appearance / Match design with | sections | kvm_bridge_win.py:3737-3910; effect_previews.py:214, 317 | Tile previews play on hover at most 60 fps (PREVIEW_MAX_RATE). Choice Ring slides (200 ms). |
| Connection page: Windows Firewall (Linux firewall on Linux) / Addresses / Language (+ Relaunch) / This PC (address, port, Save and reconnect) | sections | kvm_bridge_win.py:4765, 4937 | Buddies and accessible names on the fields. |
| Profile tab context menu (Rename, Delete), profile Delete… menu | context menu | kvm_bridge_win.py:2658, 2671 | — |
| Audio device MenuSelect menu | popup menu | audio_page_win.py:587 | — |
| About Beamer | dialog (QMessageBox, rich text, link) | kvm_bridge_win.py:5575 | Non-modal, WA_DeleteOnClose. |
| Save failed | dialog (QMessageBox.critical) | kvm_bridge_win.py:4884 | — |
| Remove Beamer Camera confirm | dialog (QMessageBox Question, destructive button) | camera_section_win.py:499 | — |
| Portable create/unlock passphrase + warnings | QInputDialog (password) + QMessageBox | kvm_bridge_win.py:6049-6086 | Shown before the main window. |
| Portable marker missing | native MessageBoxW (before Qt starts) | portable_entry.py:15 | — |
| UAC prompts (admin mode helper, vcam install) | OS dialog through ShellExecuteEx runas | admin_mode.py:121; vcam_install.py:207 | — |
| Tray icon | QSystemTrayIcon | kvm_bridge_win.py:5059; tray_glyph.py | Painted at 6 sizes, ink from the taskbar theme, tooltip set to the drive status, left click opens the window. |
| Tray menu: status line (disabled) | tray entry | kvm_bridge_win.py:5065 | — |
| Tray menu: "Beamer {version}" / update link | tray entry | :5069 | — |
| Tray menu: Send input across | tray entry | :5073 | Hidden when there are per-machine entries. |
| Tray menu: per machine "Send input to X" / "Bring input back" / "X is unavailable" | dynamic tray entries | :4445-4476 | Only when 2 or more machines can be sent to. |
| Tray menu: Pause crossing | tray entry | :5075 | — |
| Tray menu: This PC drives machines / Machines drive this PC (checkable, "(some enabled)") | tray entries | :5082-5087, 4423 | — |
| Tray menu: Profile ▸ (profiles, checkable) | submenu | :5088, 5138 | Visible only when Same ways is on and there are 2 or more profiles. |
| Tray menu: shared display submenus, one per monitor, with checkable owners (fork) | dynamic submenus | :5094-5136 | Rebuilt on aboutToShow. |
| Tray menu: Settings… / Reload configuration / Open log folder / About Beamer / Quit Beamer | tray entries | :5097-5104 | — |
| Tray balloon notifications | toast (`showMessage`, 4000 ms) | kvm_bridge_win.py:1527 | Refusal notices are rate-limited, "Cannot switch" also beeps. On sandboxed Linux it goes through portal Notification instead (**Linux-only**). |
| Edge glow window | overlay | edge_glow.py:276 | Translucent. Click-through (WindowTransparentForInput plus WA_TransparentForMouseEvents). Tool window, never activated. Topmost, re-asserted with SetWindowPos HWND_TOPMOST over the taskbar. Placed on the monitor that owns the edge, painted at its DPR. Frame timer at 1000/refresh × divisor (1, 2 or 4). Flash 0.14 s, finish 0.6 s, stale after 0.08 s. Reduced motion respected. |
| Effect overlay window (crossing, landing, switch arrival) | overlay | effect_overlay.py:255 | Same flags as the glow, above the taskbar, resized every frame to the drawn box across the virtual desktop. Per-screen DPR. Frame timer from the refresh rate × divisor. Stale after 0.08 s. Reads SPI_GETCLIENTAREAANIMATION and dark mode. Sets QT_NO_GUI_THREADPOOL. |
| Cursor overlay | overlay | cursor_overlay.py:58 | Translucent, click-through, tool, topmost through SetWindowPos(HWND_TOPMOST). Fixed 16 ms timer (about 60 Hz). Copies the system IDC_ARROW at the screen DPR. |
| Pointer veil | overlay (16×16) | kvm_bridge_win.py:5515 | Frameless tool window, topmost, no focus, opacity 0.01 (fully transparent would not hit-test), BlankCursor. WS_EX_TRANSPARENT and LAYERED toggled through SetWindowLongPtrW. |
| Identify screen numbers | overlay, one per screen | identify_overlay.py:70 | Translucent, click-through, topmost, tool, no focus, `setScreen` per monitor, matched by native origin × DPR. Lasts 3000 ms (+250 ms on the helper path). On Linux it uses X11 ToolTip windows with BypassWindowManager or a GTK layer-shell popup in a child process (**Linux-only**). |
| XWayland effect/glow windows (**Linux-only**) | overlay in an xcb child process | effect_overlay_x11.py:261 | ToolTip and X11BypassWindowManagerHint. 100 ms poll. Divisor passed down. |
| Layer-shell identify popup (**Linux-only**) | overlay | identify_layer_shell.py:96 | Gtk.Window POPUP on the layer-shell overlay layer. |
| Second-launch activation | behaviour (raises the window) | kvm_bridge_win.py:384 | QLocalServer. |
| Design-page GlowPreview | widget (unused in app) | edge_glow.py:379 | — |

Other timers: full-screen check 1000 ms, settle debounce 300 ms, listen retry 1000 ms, ColumnFit 0 ms, motion 200 ms OutCubic (`QVariantAnimation`).

### OS touchpoints (win_app)

**Windows: 101 distinct ctypes Win32 functions, plus 13 pywin32 calls, plus COM, WMI, registry and shelled tools.**

| Area | Count | Names |
|---|---|---|
| user32 | 62 | Hooks and messages: SetWindowsHookExW, UnhookWindowsHookEx, CallNextHookEx, SetWinEventHook, UnhookWinEvent, GetMessageW, TranslateMessage, DispatchMessageW, PostMessageW, PostThreadMessageW. Raw Input: RegisterRawInputDevices, GetRawInputData. Windows: RegisterClassW, UnregisterClassW, CreateWindowExW, DestroyWindow, DefWindowProcW. Input: SendInput. Keyboard: GetKeyState, GetAsyncKeyState, GetKeyboardLayout, MapVirtualKeyW, VkKeyScanW, ToUnicodeEx. Cursor: Get/SetCursorPos, GetCursorInfo, LoadCursorW, CreateCursor, SetSystemCursor, GetIconInfo, CreateIconIndirect, DestroyIcon. Display: EnumDisplayMonitors, MonitorFromWindow, GetMonitorInfoW, GetSystemMetrics, SystemParametersInfoW. Foreground: GetForegroundWindow, GetWindowRect, GetWindowThreadProcessId, GetClassNameW. Window styles: Get/SetWindowLongPtrW, SetWindowPos. Desktop: OpenInputDesktop, CloseDesktop, GetUserObjectInformationW. Clipboard: Open/Close/EmptyClipboard, Get/SetClipboardData, IsClipboardFormatAvailable, GetClipboardSequenceNumber. CCD: QueryDisplayConfig, SetDisplayConfig, GetDisplayConfigBufferSizes, DisplayConfigGetDeviceInfo. Synthetic pointer: CreateSyntheticPointerDevice2, InjectSyntheticPointerInput. Other: MessageBoxW |
| kernel32 | 26 | Create/OpenMutexW, CloseHandle, Global{Alloc,Lock,Unlock,Free,Size}, GetCurrentProcess(Id), GetCurrentThread(Id), SetThreadPriority, SetPriorityClass, SetProcessInformation (EcoQoS), ProcessIdToSessionId, WTSGetActiveConsoleSessionId, OpenEventW, SetEvent, CreateToolhelp32Snapshot, Process32FirstW/NextW, OpenProcess, QueryFullProcessImageNameW, GetModuleHandleW, GetSystemDirectoryW |
| advapi32 | 5 | OpenProcessToken, GetTokenInformation, GetSidSubAuthority, GetSidSubAuthorityCount, IsValidSid |
| shell32 | 2 | IsUserAnAdmin, SHQueryUserNotificationState |
| wtsapi32 | 2 | WTSQuerySessionInformationW, WTSFreeMemory |
| dwmapi / gdi32 / iphlpapi / secur32 | 1 each (4) | DwmSetWindowAttribute / DeleteObject / GetAdaptersAddresses / GetUserNameExW |
| pywin32 | 13 | win32security: OpenProcessToken, GetTokenInformation, LookupAccountName, ConvertSidToStringSid. win32api: CloseHandle, GetCurrentProcess. win32process: GetExitCodeProcess. win32event: WaitForSingleObject. shell: ShellExecuteEx (runas). pythoncom: CoInitialize, CoInitializeEx, CoCreateInstance, CoUninitialize |
| COM | 1 chain | ShellWindows → IServiceProvider/SID_STopLevelBrowser → IShellBrowser → active IShellView → Shell.Application.ShellExecute (unelevated launch through Explorer, admin_mode.py:265-300) |
| WMI | 1 | `WbemScripting.SWbemLocator` against root\wmi: WmiMonitorBrightness / WmiMonitorBrightnessMethods.WmiSetBrightness |
| Registry (winreg) | 5 functions, 2 keys | OpenKey, CreateKeyEx, QueryValueEx, SetValueEx, DeleteValue. Keys: HKCU\Software\KalkmanCode\Beamer\RunAsAdministrator, …\Themes\Personalize (AppsUseLightTheme in theme.py and edge_glow.py; SystemUsesLightTheme in tray_glyph.py). DWM ColorPrevalence is only mentioned in a docstring and is never read. |
| Shelled-out tools | 4 tools, 7 PS cmdlets | `schtasks` (/create /xml, /query, /change /disable, /delete, /Run), PowerShell (Get-NetFirewallApplicationFilter, Get-NetFirewallRule, Get-NetFirewallPortFilter, New-NetFirewallRule, Remove-NetFirewallRule, Get-/Set-NetConnectionProfile), `netsh advfirewall` (uninstall), `arp -a` (through core/wol). The external `beamer-audio.exe` is spawned by core/audio. |
| Named kernel objects | 3 | `Local\Beamer.Receiver[suffix]`, `Local\Beamer.InputController`, `Global\RoomDeckUnlock.*` |

**Linux: about 145 touchpoints.**
- Portals over D-Bus: 8 interfaces (InputCapture, RemoteDesktop, Clipboard, GlobalShortcuts, Notification, Session, Request, DBus.Properties), 18 methods and 9 signals.
- X11: about 19 requests across core, XInput2, XTest, XFixes and RandR.
- libxkbcommon(-x11): 19 functions.
- libei: 60 functions.

### Phone-peer code (win_app)

A phone is a peer entry with `port == 0`. It is never dialled. Phone pairing is UI-gated off by `core/feature_flags.PAIRING_QR_VISIBLE = False`.

- peers_view.py:15-16: PLATFORM_NAMES includes `"ios": "iPhone"` and `"android": "Android"`.
- machines_win.py:
  - 343: `_phone`.
  - 512: `_phone = entry.get("port") == 0`.
  - 479-481, 517, 523, 530, 534-535: phones hide left out, directions, pin address and cycle.
  - 550-551: phone in-use tooltip.
  - 562, 574-576, 584: "Pair a phone" button, hidden by the flag.
  - 625-656: `QrView`.
  - 894-899: QR gated by the flag, caption "for a phone to scan".
- qr_code.py:1-27: the pairing QR, level M for phone cameras.
- kvm_bridge_win.py:
  - 990 and 4577-4579: `_pair_phone` opens the sheet and shows a code.
  - 4698: QR cache.
  - 1990: no desk or announce messages to phones.
  - 2092: settings from a phone are ignored.
  - 4398: phones cannot be left out.
  - `peerlist.desktops` excludes phones from send, design follow, crossing and tray at 1455, 1476, 1484, 1601, 1621, 1719, 1750, 1773, 1915, 1936, 2821, 2843, 3186, 4231, 4260, 4415, 4429.
- sender.py: 1613 (no ARP hardware learning for a phone), 1634 (no wake-on-LAN for a phone).
- app_config.py: 823, 863, 1039 (`peerlist.normalise_phones` repair on load and save), 1120 (no hardware address for a phone).
- pages_win.py:258-266: `crossing_machines` never lists a phone.
- input_injector.py:350-357: a key with no US place ("absent from a phone's") goes through the layout path.
- Tests: test_phone_settings 5, test_phone_ui 13.

### Notes

- **Fork-specific** (diff against `upstream/main`, 4,435 lines added in win_app):
  - New files: audio_win.py, audio_page_win.py, camera_section_win.py, vcam_install.py and shared_display_win.py, 2,302 lines in total.
  - kvm_bridge_win.py: +105 lines (Audio page builder, audio service start, tray shared-display submenus).
  - sender.py: +20 lines (`audio_*`/`camera_*`/`display_*` routing, `host()`).
  - pages_win.py: +11 lines (Audio or "Audio & Camera" page).
  - widgets.py: +13 lines (speaker icon, `&` escape).
  - Packaging: Beamer-Setup.iss +34, Install-Maintenance.ps1 +149, Install-Beamer.ps1, build_win_app.ps1 +33, build_portable.spec +16.
  - These depend on the native `beamer-audio.exe` helper and `BeamerCameraSource.dll` under `native/audio`, outside win_app.
  - Feature flags AUDIO, CAMERA and SHARED_DISPLAY are True in core/feature_flags.py.
- **Shared displays have no DDC/CI in win_app.** "Off" removes the monitor's path from the CCD active topology. "Brightness" in win_app is only WMI built-in-panel brightness for injected brightness keys.
- **shared_display_win.py has no tests.**
- **Dead or test-only code:** `diagram.ArrangementDiagram` (diagram.py:89-342, used only by tests) and `edge_glow.GlowPreview` (edge_glow.py:379-427, never created by the app).
- **Linux code ships in the Windows exe.** build.spec and build_portable.spec glob every `win_app/*.py` into hiddenimports and add identify_layer_shell.py as data. kvm_bridge_win imports `effect_overlay_x11`, `portal`, `global_shortcut_portal` and `linux_desktop` unconditionally on Windows.
- **Hidden flags and switches:**
  - CLI: `--relaunched` and `--identify-x11` (argparse SUPPRESS); `--import-probe`, `--enable-autostart`, `--uninstall-cleanup`, `--set-admin-mode` and `--set-admin-startup` (raw argv verbs); `--hidden`; `--config`.
  - Environment and files: env `BEAMER_DEBUG` (logs every injected key); file `<state_dir>/audio-dev.enabled` (dev audio helper override, core/audio/helper.py); `BEAMER_SKIP_TESTS=1` (build).
  - `PAIRING_QR_VISIBLE=False` hides phone pairing and the QR.
- **External dependency not in the repo:** unlock_win signals a third-party credential provider through `Global\RoomDeckUnlock.*`.
- **i18n gaps (untranslated literals):** tray direction texts (kvm_bridge_win.py:4426-4436), per-machine tray texts (4466-4467), the "some enabled" suffix, the Linux tray note (2305-2307) and the QR caption (machines_win.py:899).
- **Process tuning:**
  - ABOVE_NORMAL_PRIORITY_CLASS keeps low-level hooks from being removed under load.
  - EcoQoS opt-out through SetProcessInformation(ProcessPowerThrottling).
  - The hook thread runs at THREAD_PRIORITY_HIGHEST.
  - A startup timing log line ("Window painted … seconds after process start").
- **Two mutexes:** `Beamer.Receiver` (single window) and `Beamer.InputController` (single owner of hooks and injection). Portable mode stays input-inactive while an installed copy runs.
- **Monkeypatch:** `QPushButton = widgets.ActionButton` is rebound at kvm_bridge_win module level, so every "QPushButton" in the shell is an ActionButton.
- **tokens.py is a byte-identical copy** of the repo-root tokens.py, enforced by a test.
- **Tests in win_app/tests that only exercise core/:** 56. **Tests in core/tests that exercise win_app modules:** 70 (audio and camera link gating, firewall contract, portable packaging, libei path, accent keymaps, Wayland scroll wire, nearby pairing).
- **Known environmental test failures** on the PC are listed in dev/README.md: admin_mode imports and the AZERTY layout assumption, among others.

## native/, tools, assets and packaging

### Native code

All of `native/` is fork-only (0 files on `upstream/main`, 514 tracked files on `main`). Line counts are `wc -l` of tracked files. "Own code" excludes `native/audio/third_party/` and the generated `placeholder_builtin.inc`. Beamer-written: 41,266 lines in total, of which 37,241 are C/C++/ObjC (`.c .cpp .h .mm .inc`) and 1,891 Python. Vendored: 127,579 lines in 370 files, listed in the table below. No CMake, no `.xcodeproj`, no `.vcxproj`/`.sln`. Every build is a shell, PowerShell or cmd script that calls clang or MSVC `cl` directly. Build output goes to `native/audio/build/` and `native/audio/mac-driver/build/`, both git-ignored.

| Component | What it does | Path | Language | Lines (own code) | Vendored? (name, version, lines) | Platform | Tests |
|---|---|---|---|---|---|---|---|
| Engine: control plane | `run_helper` CLI verbs (`list`, `restore`, `version`, `vcam-*`) and the JSON-lines stdio control loop driven by Python (PHASE2 §3). Also the backend contract, events, clocks, logging and base64/hex helpers | `native/audio/engine/{helper,util}.*`, `backend.h`, `engine.h`, `events.h`, `json.h` | C++17 | 2,041 | nlohmann/json 3.11.3 (header-only, 24,786 lines, MIT) | portable | `tests/engine_tests.cpp`, `tests/golden_control.txt` (145) |
| Engine: media crypto and packets | ChaCha20-Poly1305 (RFC 8439) on Monocypher, 24-byte packet header, `Sealer`, 128-packet replay window | `engine/crypto.*`, `engine/packet.*` | C++17 | 291 | Monocypher 4.0.2 (4,606 lines: `monocypher.c/.h` used, `optional/` unused; BSD-2/CC0) | portable | `engine_tests.cpp`, `tests/vectors/aead.txt` + `gen_vectors.py` |
| Engine: audio codec | Opus encoder/decoder wrapper (`codec` object, complexity, FEC, PLC) | `engine/codec.*` | C++17 | 212 | Opus 1.6.1 (329 files, 90,806 lines: 88,500 C/H; built float from its `.mk` lists, no RTCD/intrinsics, no DNN; BSD-3) | portable | `engine_tests.cpp` |
| Engine: jitter buffer, resampler, drift | `JitterBuffer` (a port of `dev/audio/proto/transport/media.py` plus `merge_expand` and `jitter_recalc_ms`), 16-tap Kaiser sinc resampler (512 phases), PI `DriftController`, SPSC rings, level meters | `engine/jitter.*`, `resampler.*`, `spsc.h`, `levels.h` | C++17 | 1,222 | none | portable | `engine_tests.cpp`, `tests/jb_crosscheck.py` (bit-for-bit against `media.py`) |
| Engine: transport | UDP sockets, `Flow` (probe, ping, RR, replay, link-fallback mode, `link_in`/`link_out` over the TCP link), the 2 ms network thread | `engine/net.*`, `engine/flow.*` | C++17 | 1,429 | none | portable (POSIX and Winsock) | `engine_tests.cpp`, `tests/two_helpers.py` |
| Engine: streams (capture → encode → send, receive → playback, mic) | `SendStream`, `RecvStream`, `LoopStream`, `TestInput`, one encoder thread per send stream | `engine/streams.*` | C++17 | 969 | none | portable | `engine_tests.cpp`, `two_helpers.py` |
| Engine: echo cancellation | `EchoCanceller` (speexdsp MDF plus residual echo suppression), `HostClock`, `AecPath` (reference alignment, drift), echo-path gate | `engine/aec.*`, `speexdsp_config_types.h` | C++17 | 627 | speexdsp 1.2.1 (32 files, 7,381 lines: echo/preprocess subset only; BSD-3) | portable | `engine_tests.cpp` |
| Engine: howl guard | Detects acoustic feedback through two machines on system-sound streams and turns it down | `engine/howl.*` | C++17 | 393 | none | portable | `engine_tests.cpp` |
| Engine: video (camera) | H.264 AU packetization, NACK/PLI/RR/TimeMap, per-flow pacer (token bucket), receive reassembly and playout, A/V lip sync and clock offset | `engine/video_{send,recv,packet}.*`, `pacer.*`, `sync.*`, `video_backend.h` | C++17 | 3,219 | none | portable | `tests/video_tests.cpp` (1,888), `tests/sync_tests.cpp` (812), `tests/two_helpers_video.py` |
| Engine: null backends and test signals | Virtual audio devices (ppm, mic, record-to-WAV) and a fake video encoder/decoder, so two helpers can run the whole protocol on one machine. Test signatures A/B, sine, noise | `engine/null_backend.*`, `null_video.*`, `test_signal.*` | C++17 | 1,311 | none | portable | `tests/null_main.cpp` (`beamer-audio-null`), `two_helpers*.py` |
| Engine build and test scripts | Source list, test builds (`--asan`, `--tsan`) | `engine/sources.txt`, `build-tests.sh`, `build-tests.cmd` | sh, cmd | 227 | n/a | mac, win | n/a |
| Mac audio backend | Core Audio IOProcs, device inventory and blocklist, default following, coreaudiod-restart recovery, process taps (macOS 14.2+) in a private aggregate, "Beamer Microphone" demand, mic permission, `main` | `native/audio/mac/{backend_mac.*,devices_mac.mm,tap_mac.mm,vmic_mac.mm,mac_internal.h,main.mm,blocklist.inc}` | ObjC++ | 1,754 | none | macOS | `mac/backend_check.mm` (494), `mac/acceptance.py` (458) |
| Mac video backend | AVFoundation camera inventory and capture, VideoToolbox H.264 encoder (low latency, LTR), 16:9 crop and VTPixelTransfer scaler, `--dev` synthetic camera | `mac/{camera_mac,videoenc_mac,scale_mac,synth_mac}.mm`, `video_backend_mac.*` | ObjC++ | 2,367 | none | macOS | `mac/video_check.mm` (1,220) |
| Mac build | Universal (arm64 + x86_64) `beamer-audio`, `backend_check`, `video_check`. Calls `mac-driver/build.sh` | `native/audio/build-mac.sh` | bash | 197 | n/a | macOS | n/a |
| Mac HAL driver (BeamerAudio.driver) | AudioServerPlugIn running in coreaudiod: "Beamer Microphone" (input, visible) and hidden "Beamer Mic Feed" (output) joined by a 16,384-frame loopback ring. Owns its clock, has input volume and mute controls. Only export is `BeamerAudio_Create` | `native/audio/mac-driver/src/BeamerAudio.c`, `Info.plist` | C11 | 1,206 + 46 plist | none | macOS (12.0+) | `test/test_host.c` (925, out-of-process host), `test/ca_util.swift` (78), `test/live_check.py` (380), `test.sh` (ASan/UBSan/TSan) |
| Mac HAL driver install and build | `build.sh` (universal, signed "Beamer Dev" or ad hoc, staged to `~/Library/Application Support/Beamer/hal-staging`). `install-driver.sh`, shipped in the app and run as root through an osascript admin prompt. `helper/beamer-hal` and `setup-beamer-hal.sh`: a NOPASSWD sudo installer for dev Macs | `mac-driver/build.sh`, `install-driver.sh`, `helper/*`, `test.sh` | bash | 293 | n/a | macOS | n/a |
| Windows audio backend | WASAPI shared, exclusive and loopback clients, process loopback (excludes a process tree), endpoint mute with an on-disk journal and a guard process, device classification (cfgmgr32 devnode chain), VB-CABLE virtual mic (128-frame IAudioClient3), session demand, MMCSS | `native/audio/win/{backend_win.*,devices_win,loopback_win,mute_journal_win,sessions_win,main}.cpp`, `blocklist.inc` | C++20 (MSVC) | 2,682 | none | Windows | `win/backend_check.cpp` (1,277), `tests/system_loss.py` (124) |
| Windows video sink | Media Foundation H.264 decoder MFT (D3D11), virtual camera creation through `MFCreateVirtualCamera` (resolved at run time, Windows 11), shared-memory writer (heartbeat, seqlocked slots), Direct2D/DirectWrite placeholder picture, camera-in-use app lookup (ConsentStore registry) | `win/{video_backend_win.*,videodec_win,vcam_win,placeholder_win,app_usage_win}.cpp` | C++20 | 2,288 | none | Windows 11 | `win/video_check.cpp` (1,244) |
| Windows camera install verbs | `beamer-audio vcam-install` and `vcam-uninstall` (elevated): copy the DLL to `%ProgramFiles%\Beamer\Camera\<v>`, SHA-256 check, HKLM COM registration, protected ACLs, reparse-safe folder handling, FrameServer service stop. Also `vcam-remove` and `vcam-status` | `win/vcam_install_win.cpp` | C++20 | 733 | none | Windows | `video_check.cpp` (test seams `--test-reg` and `--test-dir`) |
| Windows build | MSVC x64, static CRT. Builds `beamer-audio.exe`, `backend_check.exe`, `video_check.exe`, then calls `vcam-win/build.ps1` | `native/audio/build-win.cmd`, `win/build.ps1` | cmd, PowerShell | 176 | n/a | Windows | n/a |
| Windows virtual camera source (BeamerCameraSource.dll) | MF media source (`IMFMediaSourceEx`) loaded by Camera Frame Server: one NV12 stream at 720p, 1080p or 360p (30 fps). Owns the shared section and frame event, validates every field the helper writes, shows a built-in WIC-decoded image when no one feeds it, rate-limited log | `native/audio/vcam-win/source.cpp`, `source.def`, `builtin_image.h` | C++20 | 1,387 | none | Windows 11 | `tools/accept/*.ps1` (11 scripts + `common.ps1`), `tools/shmfuzz.cpp` |
| Shared-memory protocol (helper ↔ DLL) | Frozen contract: CLSID, `Global\BeamerCamera.<InstallGuid>.{Shm,Frame,Demand}`, magic `BCAM` v1, `kOffered` media types, 4 × 3,112,960-byte slots, seqlocked `Slot`/`Header` | `vcam-win/shm_protocol.h` | C++ header | 54 | none | Windows | (used by the DLL, `vcam_win.cpp`, the tools) |
| Built-in camera image | Generated "Beamer Camera / Waiting for Beamer" PNG as a C array (Hanken Grotesk text) | `vcam-win/placeholder_builtin.inc` (generated), `tools/make_builtin.py` | C array, Python | 1,982 generated + 135 py | font: Hanken Grotesk (OFL, from `win_app/assets`) | Windows | n/a |
| Camera DLL tools | `camprobe` (MF/DirectShow consumer, frame classifier), `feeder` (helper stand-in, vcam create/remove, inspect, squat, loadtest), `shmfuzz` (hostile writer), dev install/uninstall/in-session scripts | `vcam-win/tools/*.cpp`, `tool_common.h`, `*.ps1`, `accept/` | C++20, PowerShell | 1,181 C++ + 322 ps1 | none | Windows | acceptance suite in `tools/accept/` |
| Camera DLL build | `BeamerCameraSource.dll` + 3 tools. Refuses a DLL that imports the MSVC runtime | `vcam-win/build.ps1` | PowerShell | 48 | n/a | Windows | n/a |
| Python glue in native/ | Test and acceptance drivers only, no runtime glue: `two_helpers.py`, `two_helpers_video.py`, `jb_crosscheck.py`, `gen_vectors.py`, `system_loss.py`, `mac/acceptance.py`, `mac-driver/test/live_check.py`, `vcam-win/tools/make_builtin.py` | (paths listed) | Python | 1,891 | none | mac, win | n/a |
| Native docs | READMEs (engine and protocol extensions, driver design and live results, DLL behaviour, WASAPI notes, vendoring record) | `native/audio/README.md`, `mac-driver/README.md`, `vcam-win/README.md`, `win/README.md`, `third_party/README.md`, `vcam-win/tools/accept/README.md` | Markdown | 598 | n/a | n/a | n/a |

Vendored totals: Opus 90,806 + nlohmann 24,786 + speexdsp 7,381 + Monocypher 4,606 = 127,579 lines (370 files), plus `third_party/README.md` (28). Verification hashes are in `third_party/README.md`. Of the Opus tree, 15,161 lines (arm/x86/mips intrinsics) and 5,064 lines (`fixed/`) are not compiled.

### Tools, scripts and dev

| Item | What it does | Path | Lines | Used by |
|---|---|---|---|---|
| i18n extractor | Writes `locales/beamer.pot`, syncs each `.po`, `--check`/`--complete` | `tools/i18n_extract.py` | 309 | release, dev loop (`core/tests/test_i18n_coverage.py`) |
| Import probe | Imports every module inside a built bundle (mac `.app`, win `.exe`, flatpak) | `tools/import_probe.py` | 96 | build (`win_app/kvm_bridge_win.py --import-probe`, flatpak manifest) |
| Runtime probe | Run by `build_dmg.sh` with the bundle's hardened-runtime Python | `tools/runtime_probe.py` | 51 | build (mac) |
| Flatpak build | Builds the `.flatpak` per arch, refuses Flatpak 1.18.2 | `tools/build_flatpak.sh` | 32 | build, release (Linux-only) |
| Flatpak import probe | Imports the installed Linux app headless | `tools/flatpak_import_probe.py` | 31 | build check (Linux-only, installed into the flatpak) |
| Flatpak Wayland proof | Wraps `wayland_proof.sh` against the installed flatpak. Shims `flatpak-proof/{observer,python,wl-copy,wl-paste}` | `tools/flatpak_wayland_proof.sh`, `tools/flatpak-proof/*` | 7 + 27 | measurement and proof (Linux-only) |
| Wayland proof | Wayland back ends against a headless GNOME Shell and its portals. GTK 4 observer app | `tools/wayland_proof.{py,sh}`, `tools/wayland_observer.py` | 671 + 59 + 84 | measurement and proof (Linux-only) |
| X11 proof | X11 back ends against Xorg dummy or Xvfb | `tools/x11_proof.{py,sh}` | 536 + 104 | proof (Linux-only, referenced by `win_app/tests/test_{capture,inject}_x11.py`) |
| Phone test vectors | Byte-exact v5/v6 wire and pairing v2/v3 vectors for phone clients, generated from `core/protocol.py` and `core/pairing.py` | `tools/phone_vectors.py`, `tools/phone_vectors.json` | 1,191 + 2,887 | tests (`core/tests/test_v6_vectors.py`) |
| Older-peer check | 1.4.x tree vs this build refuse each other on loopback | `tools/older_peer_check.py`, `older_peer_side.py` | 95 + 82 | dev loop (`dev/audio/e2e/run.py`), release |
| Release notes | Prepares GitHub release body with notify metadata | `tools/release_body.py` | 24 | release (`core/tests/test_release_body.py`) |
| Portable qualification | Frozen portable acceptance (zip → `data/acceptance.json`) | `tools/portable_qualify.ps1`, `tools/portable_acceptance.py` | 346 + 261 | release (Windows portable) |
| README screenshots | Offscreen real settings windows, synthetic config | `tools/readme_shots_mac.py`, `readme_shots_win.py` | 193 + 229 | docs, release (no in-repo caller) |
| Colour tooling | `measure_colour_packs.py` writes `colour-measurements.json`. `render_colour_choices.py` renders Design colour controls offscreen | `tools/measure_colour_packs.py`, `render_colour_choices.py` | 117 + 485 | measurement (no in-repo caller) |
| Beam live offscreen | Measures live Beam layers without an NSWindow | `tools/beam_live_offscreen.py` | 236 | measurement (no in-repo caller) |
| Identify-screens smoke | Shows temporary screen labels natively | `tools/identify_screens_smoke.py` | 61 | dev loop (no in-repo caller) |
| UI audit (Qt) | Offscreen Qt audit per scale, control exercise, matrix runner | `scripts/ui_audit/qt_audit.py`, `qt_actions.py`, `run_qt_matrix.py` | 624 + 304 + 47 | measurement (`win_app/tests/test_audit_regressions.py`) |
| UI audit (Mac) | Offscreen AppKit audit, states, control and menu actions, contact sheets | `scripts/ui_audit/mac_render.py`, `mac_states.py`, `mac_actions.py`, `mac_menu_actions.py`, `mac_review.py` | 277 + 186 + 214 + 115 + 61 | measurement |
| UI audit (shared) | Review sheets, packing renders for cross-machine review, guard against touching the real settings tree | `scripts/ui_audit/review_sheets.py`, `pack_renders.py`, `guarded_tests.py` | 186 + 35 + 29 | measurement, tests (`guarded_tests` from `win_app/tests/test_qt_event_reentry.py`) |
| Two-machine deploy (fork) | Push HEAD to the PC, build and install on both, start the PC through its scheduled task | `dev/deploy.sh` | 84 | dev loop |
| Two-machine setup doc (fork) | Mac/PC table, SSH, `pc` remote, known test failures, branches | `dev/README.md` | 90 | dev loop |
| Audio design and status (fork) | PHASE1/PHASE2 spec (§ numbers cited throughout native/), STATUS, TEST-ALEX (French), WP1–WP17 reports and screenshots | `dev/audio/*.md`, `dev/audio/reports/`, `dev/audio/notes/` | 1,409 + 675 + 219 (md) | design record |
| Audio e2e harness (fork) | Mac↔PC end-to-end runs over SSH: pair check, clock, PC side, results tables, `agg.c` | `dev/audio/e2e/` | 1,605 | measurement |
| Audio prototypes (fork) | Phase 1 prototypes: Swift/C Core Audio tool (`beamer-audio` proto), Python transport (`media.py`: JB reference, `peer.py`, `opus.py`), scenarios, Windows proto `beamer-audio.cpp` (1,716), result logs | `dev/audio/proto/{mac,transport,scenarios,win}/` | 1,975 + 2,696 + 1,034 + 1,879 code | measurement, reference (`jb_crosscheck.py` uses `media.py`; `live_check.py` uses the Swift tool) |
| Camera design and status (fork) | PHASE1/PHASE2 (2,249), STATUS, TEST-ALEX, reports C1–C4/CW1–CW11/F3/Q1, reviews R1–R4 | `dev/camera/*.md`, `reports/`, `reviews/` | ~7,800 md | design record |
| Camera e2e harness (fork) | Cases, lip sync, analysis, PC agent, preflight, recorded runs | `dev/camera/e2e/` | 3,900 code (5,075 total) | measurement |
| Camera prototypes (fork) | Mac capture/encode proto (`.mm`), Win source and feeder proto (`source.cpp` 754, `feeder.cpp` 641, `vcamctl.cpp`), transport sim, e2e camsend/camrecv, H.264 recordings and trace data | `dev/camera/proto/{mac,win,transport,e2e}/` | 7,580 code | measurement, reference |
| Camera WIP follow-ups (fork) | Unapplied, unverified patches (F4 Mac clocks, F5 flow convergence) | `dev/camera/followups/*.patch` | 631 | none (parked) |
| Shared displays design (fork) | GN10 handover design | `dev/display/DESIGN.md` | 66 | design record |

### Locales and i18n assets

| Locale | File | Entries | Translated | Format |
|---|---|---|---|---|
| en (source, British English; US spelling applied at run time by `core/locale.americanise`) | `locales/beamer.pot` (template) | 1,500 msgids (2 plural, 11 with msgctxt) | n/a | GNU gettext PO |
| it (Italiano) | `locales/it.po` | 1,500 | 1,246 (254 empty), 0 fuzzy | GNU gettext PO |
| qps (pseudo-locale, not a file) | generated in code | n/a | n/a | runtime wrapper |

- Loader: `core/i18n.py`. `LANGUAGES = {"en", "it"}` (line 34). `locales_dir()` (line 90) resolves `RESOURCEPATH` (py2app), `_MEIPASS` (PyInstaller), or the source and Flatpak tree. `load()` (line 205) parses the `.po` with its own parser, builds `.mo` bytes in memory and reads them with stdlib `gettext.GNUTranslations`. OS language comes from `NSLocale.preferredLanguages` (Mac) or `GetUserPreferredUILanguages` via ctypes (Windows). API: `tr`, `trn`, `trc`, `N_`.
- Install sites: `win_app/kvm_bridge_win.py:6149-6151` (`i18n.install`). Packaging: `mac_app/setup.py:74` (globs `locales/*.po`), `win_app/build*.spec` (datas), flatpak manifest.
- Fork impact: the `.pot` and `it.po` gained about 1,500 and 1,400 lines over upstream (audio, camera and display strings). Fork rule: they are regenerated only at merge (`dev/camera/PHASE2.md:179`).

### Assets

| Asset | Path | Consumers |
|---|---|---|
| App mark master | `Beamer.svg` | `README.md:1`, flatpak manifest (installed as hicolor icon), `native/audio/vcam-win/tools/make_builtin.py` (drawn as paths) |
| macOS icon | `Beamer.icns` (98 KB) | `mac_app/setup.py:98` |
| Windows icon | `Beamer.ico` (21 KB) | `win_app/build.spec`, `build_portable.spec`, `Beamer-Setup.iss:46`, `win_app/kvm_bridge_win.py:195`, `win_app/widgets.py:1788` |
| PNG icon | `Beamer.png` (8 KB) | no code consumer (README mentions it as rendered from the SVG) |
| Fonts | `win_app/assets/HankenGrotesk-Variable.ttf`, `B612Mono-{Regular,Bold}.ttf` + 2 OFL texts (outside scope dir, listed for completeness) | `tokens.py:66-70`, `mac_app/setup.py:70-72`, `win_app/*.spec`, `core/tests/render_effect_sets.py`, `make_builtin.py` (bakes text into the DLL image) |
| Design tokens | `tokens.py` (palettes, sizes, faces, springs; byte-identical copy in `win_app/tokens.py`) | both apps, `theme.py`, `mac_app/{notch_beam,notch_island,previews}.py`, `mac_app/dmg/draw_background.py`, tests. Palette is hand-copied as constants into `native/audio/win/placeholder_win.cpp` |
| Mac theme wrapper | `theme.py` (AppKit/objc, wraps tokens) | about 10 `mac_app` modules (`audio_page`, `camera_section`, `diagram`, `effects_overlay`, `machines_panel`...) and the dmg background |
| Colour measurements | `colour-measurements.json` (sRGB stops per palette pack) | written by `tools/measure_colour_packs.py:111`; no reader in the repo |
| README and store screenshots | `assets/{mac,windows}-{overview,crossing,design}-{light,dark}.png`, `assets/beamer-crossing.gif` (3.7 MB) | `README.md` |
| Linux screenshots | `assets/linux-{overview,crossing,design}.png` | `flatpak/uk.co.kalkman.beamer.metainfo.xml` (pinned to upstream tag v1.5.3 URLs) |
| Social preview | `assets/social-preview.png` | none in the repo (GitHub repository setting) |
| DMG background | `mac_app/dmg/background{,@2x}.png` (drawn by `draw_background.py`) | `mac_app/build_dmg.sh` (outside scope, listed for packaging) |
| Renders | `renders/` (20 PNGs + 2 measurement JSONs here) | git-ignored, local output of `scripts/ui_audit` and tools; not shipped |
| Dev report screenshots | `dev/audio/reports/wp{6,7}-shots/`, `dev/camera/reports/cw{7,8}-shots/` | design record only |
| Sounds | none (no `.wav`, `.aiff`, `.caf` or `.mp3` in the repo; no NSSound or PlaySound use) | n/a |

### Packaging and distribution

| Stage | macOS | Windows | Linux (Linux-only) |
|---|---|---|---|
| App bundle | `mac_app/setup.py` (py2app, python.org universal2 3.13; bundles `VERSION`, `locales/*.po`, fonts) | `win_app/build.spec` (PyInstaller), `win_app/build_win_app.ps1` | `uk.co.kalkman.beamer.json` (flatpak-builder: KDE 6.11 runtime, PySide BaseApp 6.11; pinned and hashed libei 1.6.0, PyNaCl, cffi, jeepney, segno, python-xlib, six, pycparser, Jinja2/MarkupSafe), `flatpak/beamer` launcher (runs `win_app/kvm_bridge_win.py`), `flatpak/*.desktop`, `*.metainfo.xml`, `tools/build_flatpak.sh`, `docs/flatpak-build.md` |
| Native build | `native/audio/build-mac.sh` → `beamer-audio` (universal) + `BeamerAudio.driver` (`mac-driver/build.sh`) | `native/audio/build-win.cmd` → `win/build.ps1` → `beamer-audio.exe` + `vcam-win/build.ps1` → `camera\BeamerCameraSource.dll` (static CRT, checked with dumpbin) | none (no native audio or camera on Linux) |
| Signing | `mac_app/build_dmg.sh`: codesign with hardened runtime, entitlements (`mac_app/entitlements.plist`, `entitlements-audio.plist` for `beamer-audio`). Release identity "Developer ID Application: Toby Kalkman (YNL35YT683)". Local `BEAMER_SIGN_IDENTITY="Beamer Dev"` self-signed (from `mise.local.toml`, untracked). Driver is signed separately in `mac-driver/build.sh` | `build_win_app.ps1`: signtool and Authenticode verify (`Sign-BeamerArtifact`, also passed to Inno). Portable build must stay unsigned (`build_portable.ps1:92`) | none (flatpak bundle, no GPG signing in the repo) |
| Notarization | `build_dmg.sh --release`: `xcrun notarytool submit` (API key), `stapler staple` on the app and the DMG | n/a | n/a |
| Installer | DMG (`build_dmg.sh`, `mac_app/dmg/`), `mac_app/Install-Beamer-Mac.command` | Inno Setup `win_app/Beamer-Setup.iss` (ships the helper and camera DLL), `Install-Beamer.ps1`, `Install-Maintenance.ps1`, `Invoke-InstallMaintenance.ps1`. Portable zip: `build_portable.ps1` and `.spec`, `portable_entry.py`, `hooks/portable_storage.py`, qualified by `tools/portable_qualify.ps1` + `portable_acceptance.py` | `flatpak install --user dist/flatpak/<arch>/Beamer-<VERSION>-<arch>.flatpak`, x86_64 and aarch64 |
| Privileged component install | Virtual mic: `install-driver.sh` (in `Beamer.app/Contents/Resources/audio/`), run as root through an osascript admin prompt from `mac_app/audio_driver.py`, into `/Library/Audio/Plug-Ins/HAL`, then restarts coreaudiod. Dev Macs: `/usr/local/libexec/beamer-hal` + sudoers rule from `mac-driver/helper/setup-beamer-hal.sh` | Virtual camera: `beamer-audio vcam-install`/`vcam-uninstall` (elevated through UAC from `win_app/vcam_install.py`): `%ProgramFiles%\Beamer\Camera\<v>`, HKLM CLSID and `SOFTWARE\Beamer\Camera`, `%ProgramData%\Beamer\Camera`. Dev: `vcam-win/tools/devinstall.ps1`/`devuninstall.ps1`. Virtual mic: third-party VB-CABLE, not bundled | n/a |
| Autostart | (in `mac_app`) | `win_app/autostart_win.py` (scheduled task) | `win_app/autostart_x11.py` (XDG). Disabled in the Flatpak (needs the Background portal) |
| Version | `VERSION` (`1.5.19`), read by `setup.py`, `build_dmg.sh`, the apps, the specs, `build_*.ps1` and the flatpak manifest | same | same |
| Updates | `core/updates.py`: one HTTPS request to the GitHub releases API at start and daily; notice only, never downloads or installs | same | same |
| Release notes | `tools/release_body.py` | same | same |
| Dev deploy (fork) | `dev/deploy.sh mac` | `dev/deploy.sh pc` (SSH, `BEAMER_SKIP_TESTS`, start through the scheduled task) | n/a |
| CI | none: `.github/` holds only `ISSUE_TEMPLATE/{bug_report,feature_request,beta-feedback,config}.yml` (122 lines), no workflows | none | none |

### OS touchpoints (native)

Distinct OS API identifiers (functions, plus COM interfaces and ObjC classes where noted) in Beamer-written native code that ships. Test harnesses are excluded; harness-only extras are shown as "+N harness". Counted from source with a regex sweep. Foundation/ObjC runtime messaging, C++ std and libc are not counted.

| Framework | Mac helper | Mac HAL driver | Win helper | Win camera DLL | Portable engine |
|---|---|---|---|---|---|
| CoreAudio HAL (`AudioObject*`/`AudioDevice*`/`AudioHardware*` calls) | 15 (incl. process tap and aggregate create/destroy), 29 distinct `kAudio*Property*` selectors | n/a | n/a | n/a | n/a |
| CoreAudio taps (`CATapDescription` + aggregate keys) | 1 class + 6 keys | n/a | n/a | n/a | n/a |
| AudioServerPlugIn (driver side) | n/a | 1 exported factory, 22-slot `AudioServerPlugInDriverInterface` implemented, 2 host calls (`PropertiesChanged`, `RequestDeviceConfigurationChange`), 54 property selectors answered | n/a | n/a | n/a |
| AudioToolbox | 4 (`AudioConverter*`) | n/a | n/a | n/a | n/a |
| CoreFoundation | 12 | 6 | n/a | n/a | n/a |
| AVFoundation | 29 identifiers (10 classes/protocols: `AVCaptureSession`, `AVCaptureDevice`, `AVCaptureDeviceDiscoverySession`, `AVCaptureVideoDataOutput`...; 19 constants and notifications incl. authorization status) | n/a | n/a | n/a | n/a |
| VideoToolbox | 9 (+3 harness: decompression) | n/a | n/a | n/a | n/a |
| CoreMedia | 14 (+3 harness) | n/a | n/a | n/a | n/a |
| CoreVideo | 16 functions | n/a | n/a | n/a | n/a |
| libdispatch | 12 | n/a | n/a | n/a | n/a |
| mach / os_log | 3 (`mach_absolute_time`, `mach_timebase_info`, `mach_wait_until`) (+6 harness) | 4 (`mach_absolute_time`, `mach_timebase_info`, `os_log`, `os_log_create`) | n/a | n/a | 5 (`semaphore_*`, `mach_task_self`) |
| XPC / TCC | 0 (+10 harness: tccd probe in `video_check.mm`) | n/a | n/a | n/a | n/a |
| WASAPI / MMDevice / audio policy (COM interfaces + `ActivateAudioInterfaceAsync`) | n/a | n/a | 20 | n/a | n/a |
| MMCSS (avrt) | n/a | n/a | 2 | n/a | n/a |
| Media Foundation functions | n/a | n/a | 8 + `MFCreateVirtualCamera`, `MFIsVirtualCameraTypeSupported` (GetProcAddress from mfsensorgroup.dll) | 11 | n/a |
| Media Foundation interfaces | n/a | n/a | 9 (incl. `IMFTransform` H.264 decoder `CLSID_CMSH264DecoderMFT`, `IMFVirtualCamera`, `IMFDXGIDeviceManager`) + `CODECAPI_AVLowLatencyMode` | 20 (`IMFMediaSourceEx`, `IMFMediaStream2`, `IMFVideoSampleAllocatorEx`, `IMFMediaEventQueue`...) | n/a |
| Direct3D 11 | n/a | n/a | 4 | n/a | n/a |
| Direct2D / DirectWrite | n/a | n/a | 8 | n/a | n/a |
| WIC | n/a | n/a | 4 | 7 | n/a |
| KS (kernel streaming) | n/a | n/a | n/a | 2 (`IKsControl`, `PINNAME_VIDEO_CAPTURE`) | n/a |
| Registry | n/a | n/a | 8 | 1 | n/a |
| Security (SID, SDDL, ACL, token) | n/a | n/a | 12 | 9 | n/a |
| cfgmgr32 (devnode properties) | n/a | n/a | 7 | n/a | n/a |
| Kernel objects / process / timers (events, sections, mutex, CreateProcess, QPC, waitable timers) | n/a | n/a | 17 (+6 harness) | 16 | 6 (`CreateEventW`, `SetEvent`, `WaitForSingleObject`, `OpenProcess`, `SetThreadPriority`, `timeBeginPeriod`) |
| File system / NT native / services / version (`CreateFileW`, `MoveFileExW`, `SetFileInformationByHandle`, `NtCreateFile`, `NtSetInformationFile`, `RtlGetVersion`, `OpenSCManagerW`, `ControlService`, `GetFileVersionInfoW`...) | n/a | n/a | 18 | n/a | n/a |
| COM runtime | n/a | n/a | 6 | 5 (incl. `DllGetClassObject`, `DllCanUnloadNow`) | n/a |
| Sockets (BSD/Winsock) | n/a | n/a | 1 (`poll`) | n/a | 15 |
| Randomness / hashing | n/a | n/a | 1 (`BCryptHash`) | n/a | 3 (`BCryptGenRandom`, `arc4random_buf`, `getrandom`) |
| kqueue / signals | n/a | n/a | n/a | n/a | 3 (`kqueue`, `kevent`, `kill`) |

Linked frameworks (Mac helper): CoreAudio, AudioToolbox, CoreFoundation, Foundation, AVFoundation, CoreMedia, CoreVideo, VideoToolbox. Driver: CoreAudio, CoreFoundation. Windows libs: helper `ole32 avrt propsys cfgmgr32 shell32 advapi32 mmdevapi ws2_32 user32` + MF (delay-loaded) + `version bcrypt winmm`. DLL `mfplat mfuuid ole32 advapi32 windowscodecs`.

### Phone-peer code

No phone-serving runtime code exists in this scope. Native code has no phone path.

- `tools/phone_vectors.py:1` docstring: "Test vectors for phone clients of the v6 wire protocol"; `:167`; `:771` (key families: `ios` with mac, `android` with pc); `:955`, `:960` ("Vector iPhone", platform `ios`, port 0, QR secret); `:1113-1115` ("A phone pairing by QR: port 0, qr 1, and a 22-byte password").
- `tools/phone_vectors.json` (2,887 lines, 13 phone/ios mentions), consumed by `core/tests/test_v6_vectors.py`.
- `WIRE.md`: 43 mentions. Key ones: `:19`, `:24` (phone only initiates), `:57` (Android `org.json`), `:72` (platforms `ios`, `android`), `:113`/`:115` (`host ""`, `port 0` for a phone), `:249`, `:558`, `:572`, `:598` ("phone ports must keep this input-acknowledgement rule in parity"), `:612`, `:723`, `:784`, `:805`, `:863`, `:867`, `:877`, `:884`, `:902`, `:911-922` (section "A phone"), `:936`, `:1025`, `:1049` (iPhone broadcast entitlement), `:1101-1104` (QR re-pair; Bonjour left to phone apps), `:1151` (phone modifier row), `:1157-1158` (key families), `:1206`, `:1319`, `:1350` (`text` message "for a phone's keyboard"), `:1542`, `:1800` (`clipboard` capability "since a phone may take no clipboard"), `:1842`.
- `locales/beamer.pot`: `:1100` "a phone cannot be a jump target", `:4305` "iPhone", `:4376` "Pair a phone", `:4653` "The same code as a QR, for a phone to scan." (same msgids in `it.po`).
- `scripts/ui_audit/qt_audit.py:60`, `:73-74` ("Alex's iPhone…", platform `ios`, port 0), `:386`, `:432-433` (phone fixture state). `scripts/ui_audit/mac_render.py:62-64` ("Phone" entry, platform `android`).
- Not phone peers (false positives): `native/audio/win/backend_check.cpp:229,234,1095` and `native/audio/tests/engine_tests.cpp:1868` (`std::ios`). `dev/camera/e2e/cases.py:742,754,779` and `dev/camera/PHASE2.md:1816` (a phone stopwatch used as a latency measurement prop).

### Notes

- Fork-only: all of `native/` (514 files) and `dev/` (322 files) are absent from `upstream/main`. `tools/`, `scripts/`, `flatpak/`, `assets/`, `docs/`, `.github/` and the top-level files exist upstream with the same file counts. Fork diffs there: `WIRE.md` +193 lines (sections 11 Audio, 12 Camera, 13 Shared displays, all inside capability `audio`), `locales/*` (+about 2,900 lines), `tools/i18n_extract.py` (2 lines).
- The engine README says C++17 (`build-tests.sh`), but the Windows builds compile with `/std:c++20`.
- `third_party/README.md` says Opus `tests/` and meson files are not vendored. 17 such files are present (`celt/tests`, `silk/tests` = 2,324 lines, plus `meson.build` files). They are not compiled. Arch intrinsics (15,161 lines) and `fixed/` (5,064 lines) are vendored but not compiled either.
- Monocypher 4.0.3 exists (EdDSA timing fix). It is deliberately not taken, because only ChaCha20 and Poly1305 are used.
- Linux has no native audio or camera code. Audio, camera and the virtual devices are Mac and Windows only. The Windows virtual camera needs Windows 11 (`MFCreateVirtualCamera` resolved at run time). The Mac process tap needs macOS 14.2+; the HAL driver targets 12.0+ and the helper 13.0+.
- Windows has no Beamer virtual-mic driver. It relies on third-party VB-CABLE ("CABLE Input/Output", with roles detected in `devices_win.cpp`). Elgato Wave Link endpoints are blocklisted (`win/blocklist.inc`, `mac/blocklist.inc`).
- Dev-only privileged path: `beamer-hal` + a NOPASSWD sudoers rule, bound to the "Beamer Dev" cert SHA-1. The shipped path is `install-driver.sh` through the admin prompt.
- Generated code: `vcam-win/placeholder_builtin.inc` (1,982 lines) from `make_builtin.py`, which needs `mac_app/.venv`. Python palette values are duplicated as C++ constants in `win/placeholder_win.cpp`, and `tokens.py` is duplicated byte for byte in `win_app/tokens.py`.
- Dead or reference-only code under `dev/`: Phase-1 prototypes superseded by `native/`. `dev/audio/proto/win/beamer-audio.cpp` (1,716) is an earlier Windows helper. `dev/camera/proto/win/source.cpp` (754) is an earlier camera DLL. `dev/camera/proto/mac/src/*.mm` (1,309) is an earlier Mac capture/encode. Some are still load-bearing as references: `dev/audio/proto/transport/media.py` is the jitter-buffer oracle for `tests/jb_crosscheck.py`, and the Swift proto `beamer-audio` drives `mac-driver/test/live_check.py`. `dev/camera/followups/*.patch` (F4, F5) are unapplied and unverified WIP.
- Unreferenced or write-only items: `colour-measurements.json` (written only), `Beamer.png` and `assets/social-preview.png` (no code consumer), and several tools/scripts with no in-repo caller (`beam_live_offscreen.py`, `identify_screens_smoke.py`, `readme_shots_*`, `render_colour_choices.py`, `measure_colour_packs.py`, `mac_review.py`, `pack_renders.py`, `review_sheets.py`, `mac_actions.py`, `mac_menu_actions.py`).
- `.gitattributes` references `docs/ceo-17-10-2026/gh15-evidence/`, which does not exist in the tree. `renders/` and `mise.local.toml` are local and untracked (git-ignored or `.git/info/exclude`).
- `flatpak/*.metainfo.xml` screenshot URLs point at the upstream repo tag `v1.5.3`, not at the fork.
- Process note: the camera DLL's `shm_protocol.h` is marked "Frozen; change only with the operator". `kDllVersion` bumps force a reinstall.
- Design docs are extensive (`dev/audio/PHASE2.md` 1,006 lines, `dev/camera/PHASE2.md` 2,249 lines). Native code comments cite their § numbers throughout, so those documents are the spec a port would follow. `TEST-ALEX.md` files are in French.
