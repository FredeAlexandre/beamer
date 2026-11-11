# CW9 report: packaging, install, permissions (PHASE2 §9)

Date 2026-11-11. Branch `feat/camera`, commit `d6859c5` (code) + this report. Mac agent, PC over `ssh lebig`
in `C:\Users\Alex\camera-cw9\` only. Nothing installed or deployed on either machine, no camera registered,
no firewall rule added.

## Files

| File | Change |
|---|---|
| `mac_app/setup.py` | plist keys only: `NSCameraUsageDescription` (the §9.1 sentence verbatim), `NSCameraReactionEffectGesturesEnabledDefault` = false |
| `mac_app/entitlements.plist` | `com.apple.security.device.camera` (app bundle and its `python`) |
| `mac_app/entitlements-audio.plist` | `com.apple.security.device.camera` (helper `beamer-audio`) |
| `mac_app/build_dmg.sh` | after signing, the build stops unless the app, `python` and `beamer-audio` all carry `device.camera`; comment updated. Signing otherwise unchanged |
| `mac_app/Install-Beamer-Mac.command` | `Camera` added to the TCC services reset for ad-hoc builds (and to the message) |
| `win_app/vcam_install.py` (new) | the §9.2 API exactly: `Result(ok, code)`, `install(on_done, helper=None)`, `remove(on_done, helper=None)` |
| `win_app/tests/test_vcam_install.py` (new) | 30 tests: API, verb line, SID, cancel, exit codes, versions, remove order, Windows seams, installer hunks, Install-Beamer.ps1's camera function, Install-Maintenance.ps1's camera step |
| `win_app/Beamer-Setup.iss` | `CameraDll` define (warns when the DLL is missing); `[Files]` DLL -> `{app}\camera`; `[UninstallRun]` `{app}\beamer-audio.exe vcam-remove` (runhidden, skipifdoesntexist) **before** the maintenance entry |
| `win_app/Install-Beamer.ps1` | `Install-BeamerCameraDll`: copies `native\audio\build\win\camera\BeamerCameraSource.dll` to `%LOCALAPPDATA%\Beamer\camera\`, removes it when the build has none; no uninstall path added |
| `win_app/build_win_app.ps1` | signs the DLL with `Sign-BeamerArtifact` when `build-win.cmd` produced it, else warns |
| `win_app/build_portable.spec` | `("camera\\BeamerCameraSource.dll", <path>, "EXECUTABLE")` in `COLLECT` (warns when missing) |
| `win_app/build_portable.ps1` | `'camera'` added to the payload allow-list (contract issue 1) |
| `win_app/Install-Maintenance.ps1` | `Remove-BeamerCamera` on `-Remove` (CLSID, `HKLM\SOFTWARE\Beamer\Camera`, `%ProgramFiles%\Beamer\Camera`, `%ProgramData%\Beamer\Camera`, empty `Beamer` parents; `Stop-Service FrameServer` only when a folder could not be deleted, then retries); `Remove-BeamerCameraStaleVersions` on install/repair (deletes version folders other than the registered `DllPath`'s, never stops Frame Server, never fails the install) |

`vcam_install` behaviour: the helper is the one given, else `core.audio.helper.locate()` without the dev switch
(the switch may name `beamer-audio-null`, which has no vcam verbs); `install` reads the user SID from the
process token (`TokenUser`), refuses anything that is not `S-1-…` digits, then `ShellExecuteExW(runas,
<helper>, "vcam-install --owner-sid <SID>", hidden)`, waits (bounded 120 s so `on_done` always comes) and maps
the exit code; `remove` runs `vcam-remove` unelevated (30 s bound, best effort: a failure is logged and the
elevated step still runs) then `vcam-uninstall` elevated. `on_done` is called exactly once on the background
thread, also when a step raises (`failed`) and never twice when `on_done` itself raises.

## Acceptance

| Criterion | Result | Evidence |
|---|---|---|
| `dev/deploy.sh` both succeeds | **Not run** (the brief forbids deploying) -> operator | Equivalent Mac build below; PC build pieces verified separately |
| `codesign -d --entitlements -` shows `device.camera` on the app, `python`, `MacOS/beamer-audio` | **Pass** | Private export of `d6859c5` + `mise.local.toml`, `mise exec -- ./build_dmg.sh` exit 0, signed "Beamer Dev" (log: `Authority=Beamer Dev`, no ad-hoc fallback); the new post-sign check passed. `codesign -d --entitlements - <x> \| grep -o 'com.apple.security.[a-z.-]*'`: app and `python` -> `allow-unsigned-executable-memory, disable-library-validation (self-signed local build), device.audio-input, device.camera`; `beamer-audio` (`Identifier=uk.co.kalkman.beamer.audio`) -> `device.audio-input, device.camera`. The bundle was deleted after the check |
| `plutil -p Info.plist` shows both camera keys | **Pass** | `"NSCameraReactionEffectGesturesEnabledDefault" => false`, `"NSCameraUsageDescription" => "Beamer sends this Mac's camera to another computer while an app there uses Beamer Camera, after you choose it on the Audio & Camera page."` |
| Inno install has `%LOCALAPPDATA%\Beamer\camera\BeamerCameraSource.dll` | **Partly**: script checked, not compiled | `[Files] Source: "{#CameraDllSource}"; DestDir: "{app}\camera"` (`{app}` = `{localappdata}\Beamer`), text test `test_inno_ships_the_dll_in_camera_and_removes_the_camera_before_maintenance` OK. **Inno Setup is not installed on lebig** (`ISCC.exe` absent at all three usual paths), so no installer was compiled; a real install is forbidden anyway |
| `Install-Beamer.ps1` install has the same file | **Pass (function), install not run** | `ScriptInstallBehaviourTests` on lebig: the script's own `Install-BeamerCameraDll` + `Copy-WithRetry`, lifted with the PowerShell parser, run against a temp install folder: DLL lands in `<install>\camera\`, a build without it removes the folder |
| Portable build has `camera\BeamerCameraSource.dll` next to `beamer-audio.exe`, not under `_internal` | **Pass** (with stand-in binaries) | lebig, `C:\Users\Alex\camera-cw9\win_app\build_portable.ps1 -OutputDirectory ..\out` (full script: PyInstaller, licences, payload allow-list, zip) with `where.exe` as `beamer-audio.exe` and `version.dll` as the DLL (CW5's DLL does not exist yet): `Beamer-portable\` = `camera, licences, _internal, beamer-audio.exe, Beamer.exe, Beamer.portable, LICENSE, THIRD-PARTY-NOTICES.txt`; `camera\BeamerCameraSource.dll` present; `Test-Path _internal\camera` = False; the zip holds `Beamer-portable\camera\BeamerCameraSource.dll`. So the §9.2 "unverified" sub-folder placement of an EXECUTABLE entry works (PyInstaller in `requirements-win.txt`) |
| `vcam_install.py` unit-tested with mocks (verb line, SID lookup, cancel = 1223, versions, remove order) | **Pass** | Mac: `cd win_app && ../mac_app/.venv/bin/python3 -m unittest tests.test_vcam_install` -> `Ran 30 tests … OK (skipped=10)` (Windows-only classes skipped). lebig (own venv, `requirements-win.txt`, elevated ssh): `.venv\Scripts\python.exe -m unittest tests.test_vcam_install` -> `Ran 30 tests … OK`, nothing skipped: real `_current_user_sid()` equals `whoami /user`, real `_run_unelevated` returns `cmd /c exit 4` = 4, real `ShellExecuteEx runas` (no prompt from an elevated process) returns `cmd /c exit 3` = 3 |
| `Install-Maintenance.ps1 -Remove` camera step tested on lebig against a dev install (keys and folders gone, FrameServer stopped only when locked) | **Pass against a fake install; real dev install pending** | `MaintenanceCameraBehaviourTests` (6) on lebig dot-source the real script and run `Remove-BeamerCamera` / `Remove-BeamerCameraStaleVersions` with temp folders and an HKCU key standing in for `%ProgramFiles%`, `%ProgramData%`, HKLM; `Stop-Service` stubbed: keys and folders (and empty `Beamer` parents) gone, `Stop-Service FrameServer` called 0 times when nothing is locked and exactly once with the DLL held open (`FileShare None`), a non-empty `Beamer` parent kept, no-op twice on a clean machine, stale versions deleted except the registered one, a locked stale folder kept and deleted by the next run without stopping Frame Server. The run against a real `vcam-install` needs CW4's verb and CW5's DLL, and registering on the PC is CW5's in this wave (brief) |
| Existing tests unchanged | **Pass** | lebig: `tests.test_installer_maintenance` `Ran 12 … OK` (my code keeps "no `Start-Process`" in the maintenance script); `core.tests.test_portable_packaging` `Ran 4 … OK` on both machines |

## Waiting on CW3 / CW4 / CW5

At the time of this report `git log -- native/audio/mac native/audio/win native/audio/vcam-win` shows only CW5's
`shm_protocol.h` (`5adb0d8`). What remains for CW9 once they land:

1. **`native/audio/vcam-win/README.md`** is CW5's file and does not exist yet. The dev-install section to add
   (agreed text, for CW5 to include or for CW9 to append once the file exists):

   ```markdown
   ## Installing and removing Beamer Camera by hand (dev)

   `ssh lebig` sessions are elevated, so no UAC prompt. Owner = the account that runs Beamer (Alex):

       $sid = (New-Object Security.Principal.NTAccount 'lebig\alex').Translate([Security.Principal.SecurityIdentifier]).Value
       & "$env:LOCALAPPDATA\Beamer\beamer-audio.exe" vcam-install --owner-sid $sid      # or native\audio\build\win\beamer-audio.exe
       & "$env:LOCALAPPDATA\Beamer\beamer-audio.exe" vcam-status

   Without `--dll` the helper installs `<its folder>\camera\BeamerCameraSource.dll`. The running Beamer
   creates the camera at its next `vcam` command or helper start.

   Remove (script-installed or dev machines; the Inno uninstaller does this itself):

       & "$env:LOCALAPPDATA\Beamer\beamer-audio.exe" vcam-remove      # in Alex's session: Remove() his camera
       & "$env:LOCALAPPDATA\Beamer\beamer-audio.exe" vcam-uninstall   # elevated: COM class, HKLM key, Camera folders

   `vcam-remove` acts on the camera of the account that runs it (CurrentUser access); whether an ssh session of
   the same account is enough is CW4's to confirm (`vcam-remove works unelevated`). Exit codes: 0 ok, 2 bad args, 3 not elevated,
   4 failed. To make Frame Server load a new DLL version without a reboot: `Restart-Service FrameServer -Force`.
   ```

2. **With CW4 + CW5 merged** (build outputs exist): a PC build in a private folder to confirm
   `build-win.cmd` -> `native\audio\build\win\camera\BeamerCameraSource.dll` is picked up and signed by
   `build_win_app.ps1`, and the portable check repeated with the real DLL; then, coordinated with CW5 (who
   owns the PC's registration this wave), `Install-Maintenance.ps1 -Remove` against a real `vcam-install` dev
   install (keys, folders gone; FrameServer stopped only with the DLL loaded).
3. **`dev/deploy.sh both`**: operator (deploys are forbidden to this WP).
4. **Inno compile**: needs Inno Setup 6 on lebig (`%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`, as
   `build_win_app.ps1 -Release` expects); not installed by this WP.

Nothing in CW3 changes CW9's files: the helper's entitlements are already in the signed build.

## Contract issues

1. **`build_portable.ps1` is not in CW9's file list** but its payload allow-list throws "Unexpected files in
   portable payload" on any top-level item it does not know, so the §9.2 portable layout could not build.
   Added `'camera'` to that list (one word); listed here for the operator.
2. **"versions" in the `vcam_install.py` acceptance**: the API carries no version, so it is tested as "install,
   update and repair send the same line, with no `--dll` and no version": the helper installs the DLL from its
   own `camera\` folder at its compiled `kDllVersion` (§9.2 lookup rule).
3. **Page "Remove…" when `vcam-remove` fails**: §9.2 orders the two steps but says nothing on failure.
   Chosen: best effort, as in the uninstaller (`vcam-remove` exit/timeout is logged, the elevated
   `vcam-uninstall` still runs; the result is the elevated step's).
4. **Which helper `vcam_install` runs**: `core.audio.helper.locate()` without the dev switch, because
   `audio-dev.enabled` may name `beamer-audio-null`, which has no vcam verbs.
5. **Acceptance vs brief**: "`dev/deploy.sh` both succeeds" and "`-Remove` against a dev install" conflict with
   "do not deploy / do not register the DLL on the PC"; both are left to the operator / a coordinated run
   (above).
6. **Where the PowerShell tests live**: `win_app/tests/test_installer_maintenance.py` is not CW9's, so the
   `Install-Maintenance.ps1` camera tests and the `Install-Beamer.ps1` function test are in
   `test_vcam_install.py` (Windows-only classes).
7. **`Remove-BeamerCamera` ignores `OwnerSid`**: it removes the camera whoever installed it (multi-user is out
   of scope, §11.7).
8. `Install-Maintenance.ps1` resolves `%ProgramFiles%` / `%ProgramData%` with
   `[Environment]::GetFolderPath`, which in the 64-bit Windows PowerShell the installers call is
   `C:\Program Files`, the same folder the x64 helper writes.

## Needs Alex

**H6, first real install on the PC (after CW4, CW5, CW8 are merged and the operator has deployed):**
1. Signed in on lebig, open Beamer > Audio & Camera. The Camera module says Beamer Camera is not installed:
   click **Install Beamer Camera…**.
2. The UAC prompt names `beamer-audio.exe`: click **Yes**. Note whether a **Windows Firewall** prompt appears
   for `beamer-audio.exe` at any point (§2.3 packaging risk; it should not on this PC) and what you clicked.
3. Within ~5 s the page shows the camera installed; open the Windows **Camera** app: "Beamer Camera" is
   listed and shows the Beamer placeholder.
4. Tell the operator; the agent then checks over ssh: `Get-ItemProperty HKLM:\SOFTWARE\Beamer\Camera` (OwnerSid =
   your SID, DllVersion, DllPath under `C:\Program Files\Beamer\Camera\<version>\`).
   Saying **No** at step 2 must leave the page on "cancelled" with nothing changed (worth one try).

**H7, uninstall removes the camera** (only on an Inno install; it uninstalls Beamer, the operator reinstalls
after): Settings > Apps > Installed apps > Beamer > Uninstall; accept the one UAC prompt. Then the Camera app (and
Teams/Chrome) no longer list "Beamer Camera"; the agent checks over ssh that the CLSID, `HKLM\SOFTWARE\Beamer`,
`C:\Program Files\Beamer` and `C:\ProgramData\Beamer\Camera` are gone.

**H1, first Camera prompt on the Mac** (after CW3 + CW7 are merged and the operator has deployed Beamer.app):
1. Before anything else, choose the Mac camera from the **PC's** page: no prompt may appear on the Mac (the
   Mac shows "Allow Camera Access…" in the menu and a notice on its page).
2. On the Mac, open Audio & Camera and choose "MacBook Air Camera" (or click **Allow Camera Access…** in the
   menu): the macOS prompt names **Beamer** and shows "Beamer sends this Mac's camera to another computer
   while an app there uses Beamer Camera, after you choose it on the Audio & Camera page." Click **Allow**.
3. System Settings > Privacy & Security > Camera lists Beamer, on. If you clicked Don't Allow by mistake:
   `tccutil reset Camera uk.co.kalkman.beamer`, then repeat step 2.

## Part 2 (CW9b): real build, real install round trips, update

Date 2026-11-11. Mac agent, PC over `ssh lebig` (elevated), all work in `C:\Users\Alex\camera-cw9\` (plain folder
from `git archive` of `6180ae8` + the `vcam_install.py` fix below; the N+1 build in `camera-cw9\v2\`). Nothing
deployed; the installed Beamer, its tasks and the PC clone were not touched. Every "in session" step ran in Alex's
signed-in session, unelevated, through a temporary scheduled task (CW5's `tools/insession.ps1`, tasks `CW9-*`,
removed after each run). Drivers (not committed): `run\state.ps1` (every trace: devnode, CLSID, keys, folders,
Frame Server and the DLL it has loaded, tasks, firewall rules, processes), `run\helper_session.ps1` (the built
`beamer-audio.exe --control` in the session: `vcam`, wait, `vcam`, `quit`), `run\remove_maint.ps1` /
`run\stale_maint.ps1` (dot-source `win_app\Install-Maintenance.ps1`, call `Remove-BeamerCamera` /
`Remove-BeamerCameraStaleVersions` on the real paths, `Stop-Service` wrapped to count calls), `run\py_vcam.py`
(the real `vcam_install.install()` / `remove()`).

| Remaining item (part 1) | Result | Evidence |
|---|---|---|
| Windows build with CW5's DLL and CW4 + CW2's helper, signing step | **Pass** (unsigned: no Certum certificate on lebig) | `build_win_app.ps1` (`BEAMER_SKIP_TESTS=1`) exit 0: `dist\Beamer.exe`, `native\audio\build\win\beamer-audio.exe` (1,666,048 B, imports incl. MFPlat delay-load), `camera\BeamerCameraSource.dll` (276,992 B; MFPlat, ole32, ADVAPI32, KERNEL32), "Built …BeamerCameraSource.dll" from the camera hunk. `Get-BeamerSignTool`: "Windows signing: unsigned (Certum certificate absent from CurrentUser\My)", so `Sign-BeamerArtifact` returns for all three, as for the helper today; DLL `Get-AuthenticodeSignature` = NotSigned |
| Portable build with the real DLL: `camera\BeamerCameraSource.dll` beside `beamer-audio.exe`, not under `_internal` | **Pass** | `build_portable.ps1 -OutputDirectory ..\out` exit 0. `Beamer-portable\` = `camera, licences, _internal, beamer-audio.exe, Beamer.exe, Beamer.portable, LICENSE, THIRD-PARTY-NOTICES.txt`; `camera\BeamerCameraSource.dll` and `beamer-audio.exe` byte-identical (SHA-256) to the build outputs; `_internal\camera` and `_internal\BeamerCameraSource.dll` absent; zip holds `Beamer-portable\camera\BeamerCameraSource.dll` (276,992) and `Beamer-portable\beamer-audio.exe` |
| Real `vcam-install` (dev install) | **Pass** | Before: `vcam-status` `install:"missing"`, no trace (state.ps1). `beamer-audio.exe vcam-install --owner-sid S-1-5-21-…-1001` rc 0: `{"ok":true,"dll":"C:\\Program Files\\Beamer\\Camera\\1\\BeamerCameraSource.dll",…,"dll_version":1,"stale_left":0}`. HKLM CLSID `InprocServer32` = that path, `ThreadingModel=Both` (no HKCU / WOW6432 copy); `HKLM\SOFTWARE\Beamer\Camera` InstallGuid / OwnerSid (Alex) / DllVersion 1 / DllPath; `C:\ProgramData\Beamer\Camera` ACL `D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;LS)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)` (protected); the DLL inherits Program Files' ACL (Users RX). `vcam-status` -> `install:"ok"` |
| Camera present (MF enumeration) | **Pass** | The built helper started in the session (`--control`, then `vcam`): `creating:true` -> `present:true, loaded_version:1`, log "vcam: camera create ok (175 ms)" (143-146 ms on later runs). `camprobe list` in the session: MF `Beamer Camera (Caméra virtuelle Windows)` (`swd#vcamdevapi#…`), DirectShow the same + the pre-existing OBS Virtual Camera; devnode `SWD\VCAMDEVAPI\3C8B…` OK; Frame Server loaded `C:\Program Files\Beamer\Camera\1\BeamerCameraSource.dll`. `camprobe read` 3 s: 90 frames 29.9 fps, 0 gaps, `kOffered` types |
| `Install-Maintenance.ps1 -Remove` camera step against the dev install, DLL loaded | **Pass** | Inno order: `vcam-remove` in the session rc 0 `{"removed":true}` (devnode gone, MF none); then `Remove-BeamerCamera` with Frame Server still running the DLL: **1** `Stop-Service FrameServer`, 842 ms, then nothing left (no device in any state, no CLSID, no `HKLM\SOFTWARE\Beamer`, no `Program Files\Beamer`, no `ProgramData\Beamer`), Frame Server Stopped |
| Same, DLL not loaded | **Pass** | After the update run: Frame Server stopped by itself, `vcam-remove` (it starts Frame Server, which does not load our DLL), `Remove-BeamerCamera`: **0** `Stop-Service`, 550 ms, everything gone, Frame Server back to Stopped 28 s later |
| Page path: `vcam_install.install()` / `remove()` for real | **Pass** | `py_vcam.py` over ssh (elevated, so `runas` shows no prompt): `install` returned at once, `Result(ok=True, code='ok')` on thread `beamer-vcam-install` after 0.1 s, keys and folder as above; camera created by the helper in the session; after Frame Server idled, `remove`: `Result(ok=True, code='ok')` on `beamer-vcam-remove` after 0.2 s, no warning logged, so **`vcam-remove` run from an ssh session (session 0, same account) does Remove() the camera** (part 1's open question): no devnode, no CLSID, no keys, no Camera folders. `vcam-uninstall` left the empty `Beamer` parents (contract issue 9); `Remove-BeamerCamera` then removed them with 0 `Stop-Service` |
| Update N -> N+1 | **Pass** | v1 installed and created (above). N+1 = the same export with `kDllVersion = 2` in `shm_protocol.h` (private, `camera-cw9\v2`, never committed), helper and DLL rebuilt. (1) v2 helper `vcam-status` before its install: `install:"outdated","dll_version":1,"bundled_version":2`; v1 helper: `ok`. (2) An app (`camprobe read` 40 s) has the camera open on v1; v2 `vcam-install` rc 0: `dll_version 2, stale_left 1`; CLSID and `DllPath` -> `…\Camera\2\`, `DllVersion` 2, `InstallGuid` kept, folders `1` and `2`; Frame Server still runs `…\1\`. (3) v2 helper in the session: `install:"ok","dll_version":2,"loaded_version":1,"in_use":true`. (4) `Remove-BeamerCameraStaleVersions` while the app reads: warning "…\Camera\1 is still in use; the next maintenance run deletes it.", folder kept, 0 `Stop-Service`. The app ran through the whole update: 1193 frames, 29.8 fps, **0 gaps**. (5) App closed; Frame Server stopped by itself **30 s** later; `Remove-BeamerCameraStaleVersions` again: folder `1` deleted, only `2` left, 0 `Stop-Service`. (6) Reopen with the v2 helper running: Frame Server loads `…\Camera\2\BeamerCameraSource.dll`, helper `loaded_version:2`, 117 frames all the helper's pictures (0 built-in) |
| Page "outdated" / "restart needed" logic sees the right versions | **Pass** | The captured `vcam` events run through `CameraPart._compose_vcam`, the part's version extraction and `core/camera/view._pc_rows` (Mac, fixtures for the rest): v1+v1 helper -> "Installed" / "Remove…"; v1 installed + v2 helper -> "Needs an update" / "Update Beamer Camera…"; v2 installed, app on v1 -> "Close apps that use the camera to finish updating Beamer Camera." (no action); reopened on v2 -> "Installed"; removed -> "Not installed" / "Install Beamer Camera…" |
| i18n failure `win_app/vcam_install.py:91` | **Fixed** | Log strings are exempt only when called on a logger name (`test_i18n_coverage.LOGGERS` matches the receiver: `LOGGER.info(...)`), not through a local alias `log = LOGGER.info`; now `LOGGER.log(logging.INFO if result.ok else logging.WARNING, "%s exited %s (%s)", …)`. `test_no_hard_coded_user_visible_strings` OK on both machines; `test_vcam_install` 30 OK on lebig (nothing skipped), 30 OK (10 skipped) on the Mac. The other failure of that module (`beamer.pot` stale, `it.po` missing camera strings) is the merge step's (§12) |
| README dev-install section | **Done** | `native/audio/vcam-win/README.md` "Dev install and removal with the helper's verbs", with what was measured here |
| No firewall prompt / rule | **Pass** | Firewall rules 632 before and after, none for a `camera-cw9` path (the helper bound no socket: no `flow_open`) |
| `dev/deploy.sh both`, Inno compile | **Not run** (unchanged from part 1): deploy forbidden; Inno Setup not installed on lebig | |

### Contract issues (part 2)

9. **`vcam-uninstall` (CW4) leaves empty parents**: `HKLM\SOFTWARE\Beamer`, `%ProgramFiles%\Beamer`,
   `%ProgramData%\Beamer` stay (empty) after the page's "Remove…". Harmless; the uninstaller's
   `Remove-BeamerCamera` removes them. Suggest CW4 deletes a `Beamer` parent it leaves empty, as the PowerShell does.
10. **Full `Install-Maintenance.ps1 -Remove` not run**: besides the camera it unregisters the `Beamer-*`
    autostart task for the given exe, the `Beamer-GameInputRecovery` task (unconditionally) and the `Beamer.exe`
    firewall rules, i.e. the installed Beamer's. Its camera step was run as the real function on the real paths;
    the whole script runs in H7 (Inno uninstall).
11. **`vcam-remove` starts Frame Server** (MFCreateVirtualCamera + Remove() go through it) without loading our
    DLL; it stops again ~30 s later. So `Remove-BeamerCamera` right after it normally finds nothing locked.
12. **"Restart Windows" wording is gone** (STATUS decision after CW5): the page says "Close apps…", which
    matches what was measured here (new DLL at the first open > 30 s after the last app closed).

### PC cleanup (verified at the end, `run\state.ps1`)

- No `SWD\VCAMDEVAPI*` devnode in any state, nothing named Beamer in `Win32_PnPEntity`; `camprobe list`: MF none,
  DirectShow only "OBS Virtual Camera" (pre-existing).
- No CLSID `{19FCB9D1-…}` (HKLM, HKCU, WOW6432Node); no `HKLM\SOFTWARE\Beamer`.
- No `C:\Program Files\Beamer`, no `C:\ProgramData\Beamer`.
- Frame Server Stopped, start type Manual (as before).
- No `CW9*` scheduled task; Beamer's `Beamer-lebig-Alex` (Running) and `Beamer-GameInputRecovery` (Ready) as
  before; no process from `camera-cw9`; firewall rules 632 as before.

What stays: the work folder `C:\Users\Alex\camera-cw9\` (export, its venv, builds, portable zip, `run\` drivers;
the N+1 build `v2\` was deleted). Remove with `Remove-Item -Recurse C:\Users\Alex\camera-cw9` when Phase 2 closes.

### Needs Alex (unchanged)

H6 (first install through the page's UAC prompt, any Windows Firewall prompt) and H7 (Inno uninstall removes the
camera) as in part 1; both need a deploy first.
