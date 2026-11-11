# CW7 report: camera page model, Mac UI, menu items

Branch `feat/camera`, Mac only, shared checkout (own paths only). Tests and renders ran in a private
`git archive HEAD` export with my files overlaid; nothing built, deployed or installed.

## Commits
- `cde2cad` **milestone 1** (contract for CW8): `core/camera/view.py`, `core/tests/camera_fixtures.py`,
  `core/tests/test_camera_view.py`, the `core/audio/view.py` hunk (`page_model(..., camera=)`).
- the following commit: Mac UI (`mac_app/camera_section.py`, hunks in `mac_app/audio_page.py`, `pages.py`,
  `kvm_bridge_app.py`, `setup.py`), `mac_app/tests/test_camera_section.py`, the renders, this report.

## Files
| File | What |
|---|---|
| `core/camera/view.py` (new) | `camera_section(snap, audio, own_label, platform) -> CameraSection`, `map_extras(snap) -> CameraMapExtras`, `menu_model(snap, audio, own_label) -> CameraMenu`, `apply_camera`, `apply_quality`, `apply_camera_action`, `apply_camera_notice`, `apply_row`; `Row` (NamedTuple = the §8.2 `(label, value, tone, action, action_label)` tuple); `ACTION_*` names, `PRIVACY_URL`, `CAMERA_SETTINGS_URL` |
| `core/tests/camera_fixtures.py` (new) | 8 Mac-seen acceptance states (`FIXTURES`), the same seen from lebig plus `restart_needed` and `outdated` (`FIXTURES_PC`), `three_machines`, `seen_from_pc`, a `FakeService` with the §7.1 camera API (`camera_snapshot`, `push_camera`, `set_camera`, `set_quality`, `set_camera_sharing`, `ask_camera_permission`, `refresh_camera_install`) |
| `core/tests/test_camera_view.py` (new) | 31 tests: every §8.4 status from the Mac and from the PC, selector, quality, hint, help line, rows, notices, map extras + the `page_model` hunk, menu model, apply helpers, both shipped languages |
| `core/audio/view.py` (hunk) | `TileView` += `camera_line`, `has_active_camera`, `camera_live`, `watching_line` (defaults, so the audio page is unchanged without extras); `Arrow.kind` may be `"camera"`; `page_model(..., camera=None)` |
| `mac_app/camera_section.py` (new) | `CameraModule`: selector (the Audio page's `ChoicePopup`), Quality segmented control, status line + dot (+ Retry), detail, hint, "This Mac" rows (updated in place), PC help line (never shown on a Mac), camera notices |
| `mac_app/audio_page.py` (hunk) | module inserted after the notices strip; `camera_snapshot()` (flag + service API, guarded); camera extras into `page_model`; tile camera line (glyph `web.camera.fill`, accent while live) above the mic line with the tile's existing priority rules; camera arrow dotted (`[0.5, 4.5]`), accent / grey; three-lane spacing when mic, camera and sound join the same tiles; "Camera" legend entry while the module shows; `_pick_camera`, `_pick_quality`, `_camera_action` (privacy pane opened by the app) |
| `mac_app/pages.py` (hunk) | with `AUDIO` and `CAMERA`: title "Audio & Camera" and the §8.3 purpose; same key `audio`, same place and shortcut |
| `mac_app/kvm_bridge_app.py` (hunk) | menu: "Camera: <App> on <T>" line under the drive status (and in the menu-bar tooltip), "Stop Sharing the Camera" / "Share the Camera" after the two direction items, "Allow Camera Access…" while `needs_permission`; `_camera_menu`, `_camera_items` (status tick, 0.4 s), `toggle_camera_sharing`, `allow_camera_access` |
| `mac_app/setup.py` (hunk) | `"camera_section",` right before `"audio_page",` |
| `mac_app/tests/test_camera_section.py` (new) | 16 tests (page title, module, controls, map, menu, 4 Hz timing) and `render <dir>` |
| `dev/camera/reports/cw7-shots/` | the 16 renders (128-colour PNGs) |

## Acceptance (PHASE2 §12 CW7)
| Criterion | Result |
|---|---|
| `test_camera_view` passes | **PASS** (31 tests) |
| `test_audio_view` passes | **PASS** (36 tests, unchanged; the page model without `camera` is identical, also asserted in `test_camera_view`) |
| `test_i18n_coverage` (stale-catalogue rule) | **PASS under the rule**: `test_catalogues_are_current_and_complete` fails only because `locales/` is stale (my new strings and CW1's, expected); one other failure is **not mine**: `test_no_hard_coded_user_visible_strings` flags `win_app/vcam_install.py:91: log('%s exited %s (%s)')` (CW9's file: `log` is not a recognised logger name; operator/CW9). All my files pass the hard-coded, literal-`tr`, choice-table and import-time checks. |
| Offscreen renders of the 8 states, light and dark | **DONE**, 16 PNGs below, reviewed by me (iterated: module width, button padding, tile line priority, arrow lanes); operator review pending |
| Menu shows the sharing item in both states; "Allow Camera Access…" only while `needs_permission`; they call `set_camera_sharing` / `ask_camera_permission` (fake service) | **PASS** (`MenuTests`: both titles; allow hidden for idle/denied, shown for permission_from_pc; calls recorded; also hidden without the flag, without a service, on a machine without a camera, and when the service throws) |
| No main-thread call > 16 ms over a 60 s run with snapshots at 4 Hz | **PASS**: `CAMERA_UI_SECONDS=60`: 213 page refreshes, max 13.47 ms, median 3.85 ms; 1203 level updates, max 2.99 ms; 151 menu ticks (`_camera_items` + `_refresh_tray_state`), max 0.51 ms. (First run before the in-place rows: max 14.63, median 7.67; row rebuilding cost ~6 ms per state change, now ~0.1 ms.) Camera snapshots cycle through 6 states, so nearly every refresh changes the module. |
| Wording review of `texts.py` reported | **DONE**, below (no change required; two suggestions for CW1) |
| Existing Audio page tests still pass | **PASS**: `test_audio_page` 13, `test_pages`; full `mac_app` suite: no CW7 failure left (see "Full suite") |

### Full suite
`mac_app` (`discover -s tests -t ..`, 3998 tests incl. the imported core tests, export at `cde2cad` +
my Mac files): 7 failures + 7 errors, none caused by CW7 after one fix:
- known (dev/README.md, WP6 report): `test_profiles_migration` (2), `test_tile_resize` (2),
  `test_glow_falloff` (no pytest), `test_click_focus` tab ring;
- i18n: stale catalogue (accepted rule) and CW9's `win_app/vcam_install.py:91`;
- **pre-existing on a clean `cde2cad` export, same numbers**: `test_settings_layout_acceptance` (audio
  page 297.5 != 199 at 320 pt, both densities; notices module spacing 0 != 12) and `test_frame_clock`
  (0 ticks in an offscreen run): not from the camera module;
- `test_single_instance`: `AF_UNIX path too long` from my long export path; passes in a short-path export;
- `test_idle_refresh`: **mine, fixed** (`refresh_status` now calls `_camera_items` only when the tray
  has it, as the test's minimal tray does not); passes after the fix, with `test_camera_section`,
  `test_audio_page`, `test_pages`, `test_notices_mac`, `test_camera_view`, `test_audio_view` re-run.

Re-run: `python -m unittest core.tests.test_camera_view mac_app.tests.test_camera_section` from the
repo root; renders `python -m mac_app.tests.test_camera_section render <dir>` (then quantized to 128
colours with Pillow for the repo).

## Renders (`dev/camera/reports/cw7-shots/`, each `-light` and `-dark`, the whole page as this Mac shows it)
- `camera-off`: selector "Off", Quality disabled, "Off. Apps that open Beamer Camera see a 'camera off' picture." (muted); no camera marks on the map.
- `camera-idle`: "This Mac · Default camera (MacBook Air Camera)", "Ready. No app on lebig is using Beamer Camera."; Mac tile badge "MacBook Air Camera" (grey glyph), lebig tile "Camera ready"; no arrow.
- `camera-streaming`: "Microsoft Teams on lebig is using it." + "1280x720 · 30 fps · 72 ms"; badge in the accent colour, lebig tile "Microsoft Teams uses the camera" in the accent colour, live dotted camera arrow beside the dashed mic arrow.
- `camera-permission-from-pc`: route set by lebig, permission undetermined: "Allow camera access on this Mac." (amber), row "Camera access: Not asked yet", notice "Beamer needs camera access to share this Mac's camera." with "Allow Camera Access…"; grey camera arrow (Teams waiting).
- `camera-sharing-off`: "Camera sharing is off on this Mac." (muted), row "Camera sharing: Off [Share the Camera]".
- `camera-pc-not-installed`: "Beamer Camera is not installed on lebig." (amber) and the notice "lebig: Beamer Camera is not installed there. Install it in Beamer on lebig."; no watching line on lebig.
- `camera-reduced`: "Microsoft Teams on lebig is using it. Reduced quality: the network is slow." (amber), "1280x720 · 15 fps · 180 ms · through the Beamer link".
- `camera-denied`: "This Mac does not allow Beamer to use its camera." (fault), row "Camera access: Not allowed [Open Privacy Settings]".

## Wording review of `core/camera/texts.py` (CW1, milestone 2 `bf21597`)
All eight placeholder pairs match §8.4 word for word (`off`, `waiting`, `starting`, `waiting_permission`,
`sharing_off`, `denied`, `reconnecting`, `error`), "Test source" for the dev source, machine names
through `name`. **No change required.** Two suggestions for CW1 / the operator (not made):
1. `denied` detail "Allow it in Beamer on <S>": the fix is in macOS System Settings, not in Beamer
   (Beamer only has "Open Privacy Settings"). Suggest "Allow it in System Settings on {machine}". It is
   §8.4's wording, so it needs the operator.
2. With the route following the default camera (`device` null) the picture says "Default camera"
   while the page says "MacBook Air Camera" (the preferred camera's name, `view.camera_name`). Using S's
   preferred camera name when the list has it would make both agree.

## Contract issues (choices made, most consistent with PHASE2)
1. **§8.2 dataclass additions** (all defaulted, after the listed fields): `CameraSection.shown`,
   `quality_enabled`, `action`/`action_label` (the status line's own button: Retry on `error`, §8.4),
   `help_action_label` ("Camera settings"), `this_machine_label` ("This Mac"/"This PC"), `state`;
   `CameraMapExtras.live` (badge in the accent colour, §8.5). `help_action` is the action code
   (`open_camera_settings`), not the link text. `this_machine` rows are `view.Row` NamedTuples, i.e. the
   tuple §8.2 names.
2. **Retry on `error`**: the service has no camera retry. `ACTION_RETRY` sets the same route again
   (`set_camera(machine, device)`): a new stamp changes the route view, which is a §6.5 retry token.
   For `reason == "helper"` on the camera machine itself it calls `retry_helper()`. On a Mac with
   permission `undetermined` that `set_camera(own, …)` would also send `permission_request` (a local
   click, so allowed by §9.1).
3. **Detail line**: the mockup's "Wi-Fi" is not in the snapshot (`VideoStreamView.path` is `udp`/`link`);
   the detail names only the link fallback ("through the Beamer link").
4. **Status params**: the view reads `status.params["to"]` (space-separated sink ids, as CW1's
   uncommitted planner writes it) and falls back to `snap.sinks`; app names come from
   `machines[T].virtual_camera.app`. Not in §7.2's text; works with either source.
5. **"no_virtual_camera" overall state** (no linked PC can show it) has no §8.4 line; I wrote "No linked
   PC can show it: apps on a PC use Beamer Camera." (amber).
6. **"Allow Camera Access…" on the page**: the notice carries the button while `needs_permission`; the
   "Camera access" row carries it only when permission is undetermined and the route does not name
   this Mac (pre-granting), so the page never shows two identical buttons.
7. **Notices naming another PC** (missing / outdated / broken Beamer Camera) are shown only while a
   camera is chosen (not while Off), for online machines other than this one.
8. **Menu placement**: the line sits right under the drive status; the sharing and permission items
   after "Other machines drive this Mac" (no new separators, so hidden items leave no empty section).
9. Tile badge glyph `web.camera.fill` as §8.1 says; at 11 pt it reads close to the mic glyph. If the
   operator prefers, `video.fill` is a one-word change in `audio_page.py`.

## Needs Alex
Nothing for CW7 (renders reviewed by the operator). Later, by eye in Phase 3: the menu items in the
real menu bar, and the macOS prompt after "Allow Camera Access…" (H1).

## Notes for CW6
- The page and the menu use only the §7.1 API: `service.camera_snapshot()`, `set_camera`,
  `set_quality`, `set_camera_sharing`, `ask_camera_permission`, `retry_helper`, and `subscribe` firing
  on camera changes. Until the service has `camera_snapshot`, the module and the menu items stay hidden
  (also with `feature_flags.CAMERA` off).
- py2app `includes` must have `core.camera.view` and `core.camera.model` (the page and menu import them
  lazily); `"camera_section"` is in `py_modules` (my hunk).
- The menu tick calls `camera_snapshot()` and `snapshot()` every 0.4 s on the main thread: both must stay
  plain reads of the last published snapshots.

## Notes for CW8
- Build from `camera_section(snap, audio, tr("This PC"), "windows")` and `map_extras(snap)` (pass to
  `core.audio.view.page_model(..., camera=)`); fixtures: `camera_fixtures.FIXTURES_PC` (the 8 states +
  `restart_needed` + `outdated`), `seen_from_pc(...)`, `FakeService`.
- Row actions: `ACTION_INSTALL` / `ACTION_UPDATE` / `ACTION_REPAIR` -> `vcam_install.install(on_done)`,
  `ACTION_REMOVE` -> `vcam_install.remove(on_done)`; the model omits them while a video stream runs on
  this PC. "Installation was cancelled." is page state (after `Result(False, "cancelled")`), not in the
  model. `help_action` = `ACTION_CAMERA_SETTINGS` -> open `view.CAMERA_SETTINGS_URL`.
  `apply_camera_action` returns False for every app-run action (`view.APP_ACTIONS`).
- "Restart Windows to finish updating Beamer Camera." is the row's value when `vcam_local.loaded_version`
  and `dll_version` are both non-zero and differ.
- The Mac's look for reference: `cw7-shots/`; camera arrow dotted, lanes mic / camera / sound.
