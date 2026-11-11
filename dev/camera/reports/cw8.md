# CW8 report: Qt Camera module (Windows "Audio & Camera" page)

Branch `feat/camera`, shared checkout (own paths only). Edited on the Mac; tests and offscreen renders ran
on lebig in a private export (`git archive HEAD` + my files, unpacked in `C:\Users\Alex\camera-cw8\`, run
with the PC clone's `win_app\.venv` python, which was not modified). PySide6 is not installed on the Mac,
so every Qt test ran on the PC. Nothing built, deployed or installed; `vcam_install` was always mocked.

## Files
| File | What |
|---|---|
| `win_app/camera_section_win.py` (new) | `CameraModule` (a `widgets.Module`): selector (the Audio page's `MenuSelect` over `view.Choice`s), Quality (`widgets.Choice` 720p / 1080p, disabled while Off), status line in its tone with its own Retry, detail, hint, "This PC" row with Install / Update / Repair / Remove…, the module's note under the row ("Waiting for Windows to install…", "Installation was cancelled.", "Removal was cancelled.", failure / no-helper lines), the static multi-app help line with "Camera settings", camera notices (the page's `NoticeView`). `section_for(service, audio, own_label)` -> `CameraState(snapshot, section, extras)`, or `NO_CAMERA` without the flag, off Windows, without `camera_snapshot`, or when it raises. Map helpers used by the page: `paint_camera_glyph`, `camera_pen` (dotted), `lane_order` (mic / camera / sound), `tile_lines` (camera line above the mic line, priority as on the Mac). |
| `win_app/tests/test_camera_section_win.py` (new) | 27 tests (below) and `--render <dir> [states]` for the shots. |
| `win_app/audio_page_win.py` (hunk) | module inserted right after the notices strip, above Machines; `refresh` builds the camera state, passes `camera=extras` to `view.page_model`, shows the section; a refresh waits while the camera menu is open too; the map draws the camera line on tiles (glyph + text, accent while live) and the camera arrow dotted, lanes mic / camera / sound when a camera arrow is in a pair (audio-only maps unchanged); legend "Camera" between Microphone and Waiting while the module shows; `paint_arrow(..., dotted=)`. |
| `win_app/pages_win.py` (hunk) | key `audio` titled "Audio & Camera" with the §8.3 purpose (same msgids as the Mac page) when `feature_flags.CAMERA` is on **and** `sys.platform == "win32"`; Linux keeps "Audio". Same key, place, shortcut. |
| `dev/camera/reports/cw8-shots/` | 22 renders (below). |

## Acceptance (PHASE2 §12 CW8)
| Criterion | Result |
|---|---|
| Offscreen renders (`QT_QPA_PLATFORM=offscreen`) of the same 8 states plus "restart needed", light/dark | **DONE**: 8 states + restart_needed + outdated + install_cancelled, light and dark, the whole window as lebig shows it. Reviewed by me; operator review pending. One defect seen, not in my files: the sidebar shows "Audio_Camera" (Contract issue 1). |
| Install button calls `vcam_install.install(on_done)` (mocks) | **PASS** (`test_install_runs_vcam_install_and_relays_its_result_to_the_qt_thread`, `test_update_and_repair_use_the_same_call`): Install / Update / Repair -> `install(on_done)`; Remove… asks first (`confirm_remove`, mocked) then `remove(on_done)`. |
| Shows the cancelled state for `Result(False, "cancelled")` | **PASS**: "Installation was cancelled." under the row (amber), kept across refreshes that do not change the row, cleared when the install state changes (`test_a_new_install_state_clears_the_note`); "Removal was cancelled." likewise. |
| Calls `service.refresh_camera_install()` after `Result(True, "ok")` | **PASS** (install and remove). |
| `on_done` marshalled to the Qt thread | **PASS**: `on_done` is called from a real background thread; it only emits a queued signal; the handler ran on `app.thread()` and nothing ran before the event loop turned. An installer that raises leaves no row waiting. |
| Help link calls the URL opener with `ms-settings:camera` (mock) | **PASS** (`test_the_help_line_opens_camera_settings`); help line only while Beamer Camera is installed (model). |
| Keyboard reaches every control | **PASS**: Tab reaches the camera selector, the checked Quality cell, Retry, the row's button, Camera settings and notice buttons (plus every Audio page control as before); the other Quality cell is reached with the arrow keys, as in every `widgets.Choice` (Qt's exclusive group). |
| The page still opens with Ctrl+3 | **PASS** (`InTheWindow.test_audio_and_camera_opens_with_ctrl_3`: flag on, Windows; `audio` right after `crossing`, index 3, heading "AUDIO & CAMERA", the Ctrl+3 QShortcut opens it; an offscreen window is never active, so the shortcut is fired, not pressed, as in WP7). Without the flag or on Linux the page keeps "Audio". |
| `win_app` suite otherwise unchanged | see "Full suite" below. |
| On Linux the module stays hidden | **PASS** (`section_for` returns nothing off Windows; no camera marks on the map, page model identical to the audio-only one). |

Other tests: every PC fixture shows exactly its `camera_section` (selector title, line + tone role, detail,
hint, quality, rows, buttons, help line, notices); module order (notices, Camera, Machines); hidden without
the flag / off Windows / with no camera machine; map carries the camera (badge line, "Microsoft Teams uses
the camera", active dotted arrow, painted without error); selector and Quality call `set_camera` /
`set_quality`, headers never; Retry on `error` sets the route again; a camera change pushed from a worker
thread updates the module only on the Qt thread; the camera menu defers refreshes; lane order and tile line
placement (pure).

Re-run (PC, from the export root): `python -m unittest win_app.tests.test_camera_section_win`;
renders: `python win_app\tests\test_camera_section_win.py --render <dir> [state ...]`.

Targeted run: `test_camera_section_win` + `test_audio_page_win` + `test_pages_win`: 143 tests OK.

i18n (`core.tests.test_i18n_coverage`, Mac): 2 failures, both expected / not mine:
`test_catalogues_are_current_and_complete` (stale `locales/`, my new strings among CW1/CW7's: merge step) and
`test_no_hard_coded_user_visible_strings` on `win_app/vcam_install.py:91` (CW9's file, already reported by CW7).
My files pass the hard-coded, literal-`tr` and import-time checks.

## Full suite
`python -m unittest discover -s tests` in `win_app` (PC, offscreen, export of `8444f79` + my files):
**4283 tests, 3 failures, 6 errors, 126 skipped** (1187 s). The 7 known environmental ones of `dev/README.md`
(`test_admin_mode`, `test_admin_mode_ui`, `test_profiles_migration` x2, `test_tile_resize` x2,
`test_a_shifted_digit_gives_the_symbol_the_layout_prints`) plus two not caused by CW8:
`test_catalogues_are_current_and_complete` (stale `locales/`, allowed by the §12 rule) and
`test_no_hard_coded_user_visible_strings` on `win_app/vcam_install.py:91` only (CW9). **PASS** under those rules;
no other failure.

## Renders (`dev/camera/reports/cw8-shots/`, each `-light` and `-dark`, the whole window on lebig)
- `camera-off`: selector "Off", Quality disabled, "Off. Apps that open Beamer Camera see a 'camera off' picture." (muted); This PC "Installed [Remove…]", help line + "Camera settings"; no camera marks on the map.
- `camera-idle`: "MacBook Air · Default camera (MacBook Air Camera)", "Ready. No app on this PC is using Beamer Camera."; Mac tile "MacBook Air Camera" (grey glyph), this PC's tile "Camera ready".
- `camera-streaming`: "Microsoft Teams on this PC is using it." (live) + "1280x720 · 30 fps · 72 ms"; no Remove while the stream runs; map: Mac tile camera line bold, this PC "Microsoft Teams uses the camera" in the accent colour, dotted live camera arrow under the dashed mic arrow; legend has "Camera".
- `camera-permission-from-pc`: "Allow camera access on MacBook Air." (amber), no action on the PC.
- `camera-sharing-off`: "Turned off on MacBook Air." (muted).
- `camera-pc-not-installed`: "Beamer Camera is not installed on this PC." (amber), row "Not installed [Install Beamer Camera…]" (primary), no help line.
- `camera-install-cancelled`: the same after a refused UAC prompt: "Installation was cancelled." under the row.
- `camera-reduced`: "Microsoft Teams on this PC is using it. Reduced quality: the network is slow." (amber), "1280x720 · 15 fps · 180 ms · through the Beamer link".
- `camera-denied`: "MacBook Air does not allow Beamer to use its camera." (fault), no action on the PC.
- `camera-restart-needed`: row value "Restart Windows to finish updating Beamer Camera." (amber), no button.
- `camera-outdated`: "Beamer Camera needs an update on this PC.", row "Needs an update [Update Beamer Camera…]".

## Contract issues (choices made, most consistent with PHASE2)
1. **Sidebar shows "Audio_Camera"** (visible in every render): `widgets.Sidebar` builds `PageButton(tr(name))`
   and a QPushButton reads "&" as a mnemonic, so "Audio & Camera" loses its ampersand and underlines the
   space. The page heading (a QLabel) is right. Not fixable in CW8's files; one-line fix for the owner of
   `win_app/widgets.py` (operator): in `Sidebar.__init__`, `PageButton(shown_name.replace("&", "&&"))`
   (keep the tooltip / accessible name unescaped). Until then it shows only once CW6 sets `CAMERA = True`.
2. **Remove… confirmation**: §9.2 says the FrameServer pause is "warned in the confirmation" but gives no
   wording. Mine (`QMessageBox`, Cancel default): "Remove Beamer Camera?" / "Apps on this PC will no longer
   list Beamer Camera. Windows asks for administrator permission, and every camera on this PC may pause for
   about a second." [Remove] [Cancel].
3. **Row notes beyond "Installation was cancelled."** (§8.6 names only that one): while a step runs, "Waiting
   for Windows to install / remove Beamer Camera…" (button disabled); `failed` / `not_elevated` / `bad_args`
   -> "Beamer Camera could not be installed (removed). Try again, or see Beamer's log."; `no_helper` ->
   "Beamer's media helper is missing, so Beamer Camera cannot be installed (removed)."; "Removal was
   cancelled." A note stays until the model's row changes (new install state) or another step starts.
4. **"Audio & Camera" only on Windows**: CW8's goal says the module stays hidden on Linux; the title follows
   (Linux keeps "Audio"), so a Linux user is not promised a camera. Uses the Mac page's msgids.
5. **Status line style**: the Qt Microphone module shows its status as a coloured note under the control,
   without a dot, so the camera's line does the same (the §8.3 mockup's "●" is the Mac's style); detail and
   hint sit under it in the control column, the "This PC" row and help line span the module after a seam.
6. **Map lanes**: the lane order mic / camera / sound applies only to pairs that include a camera arrow, so
   audio-only maps (and WP7's renders) are unchanged.
7. `core.feature_flags.CAMERA` does not exist yet (CW6): the module reads it with `getattr(..., False)`, so
   until CW6 lands the PC page is exactly the Audio page; tests patch the flag.

## Needs Alex
H3 (by eye, on lebig's desktop): the "multiple apps" setting in Settings > Bluetooth & devices > Cameras >
Beamer Camera, and that "Camera settings" opens it. Also by eye later: the UAC prompt from "Install Beamer
Camera…" / "Remove…" (CW9's real `vcam_install`, never run here).

## Notes for CW6 / the operator
- The page reads `service.camera_snapshot()`, calls `set_camera`, `set_quality`, `retry_helper`,
  `refresh_camera_install` (and `set_camera_sharing` / `ask_camera_permission` only through notices, which a
  PC never gets); `subscribe` must fire on camera changes. A raising `camera_snapshot` is logged and hides the
  module.
- When `CAMERA` turns on, `test_i18n_pages` / `test_i18n_coverage` need the merge step's `i18n_extract` for
  the new strings.
