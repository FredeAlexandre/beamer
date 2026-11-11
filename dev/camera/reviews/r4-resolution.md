# R4 resolution (F1)

Review: `dev/camera/reviews/r4-python.md` (3 major, 5 minor). Rulings: the operator's F1 brief and two later
operator items ("Also fixed" below). Fixes in `42403fc` ("Camera F1: fix the R4 Python review findings") and
`965ad6e` (junction-safe uninstall). Every finding was re-checked in the code first;
all eight are correct as described. The new regression tests were run against the code before the fix
(a `git archive` of the parent with only the test file overlaid) and fail there; they pass after it.

| # | Verdict | Fix | Tests (fail before, pass after) |
|---|---|---|---|
| 1 major | **fixed** (`42403fc`) | (a) sink enforces its own route; (b) per-entry route validation; (d) warning | `test_camera_r4.OffAlwaysStopsTheMacCamera` (scenarios A, B, C), `PartTests.test_sink_closes_a_stream_its_route_no_longer_names`, `test_sink_refuses_an_open_for_another_camera`, `test_own_rev_never_jumps_far_ahead`; `test_camera_wire.RouteTests.test_bad_entries_are_dropped_one_by_one`, `test_at_most_max_extra_unknown_entries` |
| 2 major | **fixed** (`42403fc`) | demand only from a live path; hold only after a live end | `test_camera_r4.NoStaleDemand` (link outage, Mac Beamer restart, PC helper restart: camera never on, invariant checker clean), `PartTests.test_loaded_lists_have_no_demand`, `test_link_down_drops_demand_without_a_hold`; `test_a_live_end_still_holds_three_seconds` (guard, passes before too) |
| 3 major | **fixed** (`42403fc`) | the menu acts on its label | `mac_app/tests/test_camera_section.MenuTests.test_sharing_item_acts_on_its_label_with_two_quick_clicks`, `test_stop_sharing_never_turns_sharing_on`; `test_camera_view.MacMenu.test_sharing_item_acts_on_its_label` |
| 4 minor | **fixed** (`42403fc`) | fail closed | `test_camera_r4.PartTests.test_sharing_off_is_written_at_once`, `test_unreadable_or_other_version_file_starts_with_sharing_off`, `test_a_failed_save_is_surfaced_and_retried`, `test_missing_file_is_a_first_run_with_sharing_on`; `test_camera_view.MacMenu.test_store_notices` |
| 5 minor | **fixed** (`42403fc`) | bounded, purged | `test_camera_r4.PartTests.test_held_lists_are_bounded`, `test_a_machine_offline_for_the_ttl_is_purged` |
| 6 minor | **accepted risk**, partly hardened (`42403fc` + F2's native verb) | `--dll-sha256` | `test_vcam_install.InstallTests.test_verb_line_runs_the_helper_elevated_with_this_users_sid`, `test_the_dll_hash_is_the_one_the_helper_will_install` |
| 7 minor | **fixed** (`42403fc`) | `OwnerSid` respected | `test_vcam_install.MaintenanceCameraBehaviourTests.test_remove_keeps_a_camera_another_account_owns` (fails on the old script, on lebig), `test_remove_by_the_owner_removes_everything`, `test_remove_with_no_owner_recorded_removes_everything`; `PackagingContractTests` updated |
| 8 minor | **fixed** (`42403fc`) | quiet close | `test_camera_r4.SinkCloseIsNoError.test_route_close_from_the_sink` |

## What changed, per finding

### #1 "Off" on the PC always stops the Mac camera

Defence in depth, as ruled:

- **(a) The sink enforces its own route** (`core/camera/part.py` `_route_names`, `_on_camera_open`,
  `_check_recv`). A sink takes a stream only while its own route view names that sender as the camera machine
  and, when the route names a camera, the sender's `camera_open.camera` is that camera (same key, else same
  name). This is checked at every `camera_open` (else `camera_opened {ok:false, error:"not_source"}`) and at
  every `apply()`, so after every route change. When it no longer holds, the sink sends `camera_close
  {reason:"route"}`, stops its helper recv, and remembers its route view (`_not_source`). The sender stops
  its camera on that close at once (§6.5 refusal path) and re-opens only when its retry token changes. When
  the sink's route view changes again while it has demand, it publishes a new list rev, so a lagging sender
  can retry. `route` is the closest existing close code; the wire is unchanged.
- **(b) Route entries from peers** (`core/camera/wire.py` `check_route`, used by the part with its held
  route):
  - Each entry is validated on its own. A `camera` or `quality` entry whose stamp is invalid or more than
    `ROUTE_FUTURE` = 10 min ahead of the receiver's clock is dropped.
  - An unknown entry is kept only when it is an object of at most 1 KiB encoded, whose stamp, when it has
    one, passes the same check. At most 8 unknown entries are held; held names come first.
  - A dropped entry is replaced by the receiver's held entry. The merge therefore neither takes it nor
    answers the sender about it, so no reply loop starts, and the rest of the message still merges.
  - `model.max_stamp` ignores out-of-range stamps.
  - The route stored in `camera.json` is the merged, validated one. `store.load` re-reads it through the
    same checks, with the camera entry "never set" when its stamp is bad.
  - A `camera_devices` list describing this machine raises `own_rev` only when its rev is not far ahead of
    this clock. Otherwise one forged list would make every later list of ours refused.
- **(d)** A route with dropped entries is logged once per peer at warning level, with the names.

The scenario tests run on the cluster harness, with the stream running Mac -> PC:
- **A** (`{"set_at": 2**53}` extra) and **B** (17 KB extra): Off on the PC reaches the Mac. The camera is off
  within 0.3 s and stays off for 30 s with no re-open. Neither machine holds or saves the entry, and later
  changes (quality) propagate.
- **C** (PC wall clock 2 days ahead): the Mac drops the PC's camera entry and logs it. The PC closes the
  stream itself, so the Mac camera is off within 0.3 s, with no re-open. The two route views stay apart
  (the Mac page still names the Mac) until a change is made on a machine whose clock the others accept.
  See "Residual" below.

Ruling choice: **reject (drop the entry)**, not clamp. A clamped stamp differs from the author's, so every
merge would answer it. With a moving clock this would cause repeated re-floods.

### #2 No stale demand

- Lists loaded from `camera.json` come back with `in_use: false`, `app` and `want` null
  (`model.without_demand`, in `store.load` and in the part).
- `link_down(peer)`, and any machine that is no longer `online`, drop that machine's demand with no hold
  (`_forget_demand`).
- The 3 s `DEMAND_HOLD` starts only for a true -> false edge while that machine is online and its list says
  `status: ok`. A list from a restarting helper (`starting`) ends demand without a hold.
- On the PC, `_helper_gone` clears the published `in_use`/`app`/`want` until the new helper's first `vcam`
  event.
- The Mac's own helper restart keeps the peers' demand. That demand came over a live link, which is still
  up, and the re-plan on `ready` needs it (§5.3).

Test: Teams closes on the PC during a link outage, during a Mac Beamer restart, and during a PC helper
restart. For 6 s after each, checked every 50 ms, the Mac camera is never on and `camera_starts` does not
move. The invariant checker stays clean. A new call afterwards streams at once.

### #3 The menu acts on its label

`core.camera.view.SharingItem` remembers the action of the title it shows. A click sends exactly that action
and flips the title at once. A status tick that still carries the old snapshot does not bring the old title
back for up to 2 s, or until the snapshot agrees. A click before any title was shown does nothing.
`mac_app/kvm_bridge_app.py` uses it (`_camera_items`, `toggle_camera_sharing`). The click no longer reads the
snapshot.

### #4 Sharing persistence fails closed

- `set_sharing` writes `camera.json` at once, without the 500 ms debounce.
- `store.load` reads a missing file as a first run (sharing on, PHASE2 §6.1). An existing file that cannot be
  read, is malformed, is of another version or has no boolean `sharing` loads with **sharing off** and
  `problem = "unreadable"`.
- `CameraSnapshot.store_problem` (new, default None) feeds a notice on the Mac page: "Camera sharing is off
  because Beamer could not read its saved camera settings." The sharing row already carries "Share the
  Camera", so the notice has no button. It goes once Alex sets sharing.
- A failed save sets `store_problem = "save_failed"`, shown as a fault notice on any machine, and is retried
  every 5 s until it succeeds.
- `dev_status` includes `store_problem`.

### #5 Bounded peer lists

String sizes and counts inside a list were already bounded by `wire.read_devices`: 16 cameras, 16 formats,
key 200, name and app 128, platform 16, 32 feature words. New:
- At most `MAX_HELD = protocol.MAX_PEERS` (32) lists of other machines. A new machine takes the place of the
  offline one seen longest ago, and an online or direct one is never evicted.
- Last-online times are kept, saved at most hourly (`seen` in `camera.json`, ignored by older readers).
- A machine offline for 30 days (`settings_sync.SNAPSHOT_TTL`) has its list purged.

"Forgotten" is mapped to this TTL. The part has no view of the pairing book (`MediaContext` has no such call),
and adding one would touch the service hunk.

### #6 Elevated install reads from user-writable `%LOCALAPPDATA%`

**Accepted risk.** `beamer-audio.exe` and `camera\BeamerCameraSource.dll` live in `%LOCALAPPDATA%\Beamer`,
which any process of the user can write. Alex's UAC "Yes" runs that `beamer-audio.exe` elevated. The DLL it
registers is then loaded by Frame Server (LocalService) in every camera app. Medium-integrity malware can
replace either file, so one Install click can become elevated code execution. The same pattern already
exists for `Invoke-InstallMaintenance.ps1` in `{app}`.

**Hardening done:**
- `win_app/vcam_install.py` finds the DLL by the helper's own lookup rule (`camera\`, then beside the exe).
- It hashes the DLL before the prompt and passes `--dll-sha256 <hex>`. When no DLL is found, it fails before
  any prompt.
- F2 makes the elevated verb copy the DLL into `%ProgramFiles%\Beamer\Camera\<v>\`, hash the copy there, and
  refuse to register on a mismatch.

This closes swapping the DLL while the prompt is up, and between copy and registration. It does **not** close
a replaced `beamer-audio.exe`: the elevated process is the attacker's. It also does not close a DLL replaced
before Beamer hashed it.

**Mitigation proposal:**
1. Sign the helper and the DLL (Certum identity, PHASE2 §9.2 / `Sign-BeamerArtifact`).
2. Run the elevated step from a copy in a protected location. For example, the per-machine Inno
   `PrivilegesRequired=admin` component installs `beamer-audio.exe` and the DLL under `%ProgramFiles%\Beamer`
   at setup time, and the page's Install elevates `%ProgramFiles%\Beamer\beamer-audio.exe`, never the
   `%LOCALAPPDATA%` one.
3. Until then, have the elevated verb check the Authenticode signer of its own image and of the DLL against
   a pinned publisher before copying, and refuse unsigned files in release builds.

Record it in PHASE2 §11. That file is not F1's.

### #7 Uninstall respects `OwnerSid`

`Install-Maintenance.ps1`:
- `Test-BeamerCameraOwner` reads `OwnerSid` from `HKLM\SOFTWARE\Beamer\Camera`.
- `Remove-BeamerCamera -UserSid $UserSid` removes the machine-wide camera (COM class, HKLM key, Camera folders,
  and the Frame Server stop when it is needed) only when that account owns it (SIDs compared normalised) or
  no owner is recorded.
- Otherwise it writes a warning and leaves everything, including Frame Server, alone.
- The uninstaller's unelevated `vcam-remove` still removes the uninstalling user's own camera object.

Tested with the real script on lebig against a fake install (HKCU key and temp folders, `Stop-Service` stub):
- other owner -> kept, and also kept with no `-UserSid`;
- owner (case differs) -> removed;
- no owner -> removed.

Not changed: the page's **Remove…** (`vcam_install.remove` -> native `vcam-uninstall`) still removes the
registration whoever owns it. That is an explicit action of an administrator on that PC, behind UAC. A guard
there belongs in the native verb (F2/CW4) or a later ruling.

### #8 A normal close is not an error

A `camera_close` from the sink with reason `route`, `demand` or `shutdown` stops the send at once, like a
refusal: helper `stop`, no re-open until the retry token changes. It is marked quiet: the Mac page shows that
sink as `idle`, not `error`, with no Retry. A `demand` close also drops the 3 s hold. `error`, `helper`,
`no_receiver` and the other reasons still show as failures.

## Also fixed (operator items after F2, `c697ad1`)

1. **`--dll-sha256` is mandatory on the helper's elevated `vcam-install`** (F2: exit 2 without it, exit 4 on a
   mismatch). `win_app/vcam_install.py` already computes and passes it since `42403fc`:
   - It picks the DLL by the helper's own lookup rule (`find_dll` in `native/audio/win/vcam_install_win.cpp`:
     `<exe>\camera\BeamerCameraSource.dll`, then `<exe>\BeamerCameraSource.dll`).
   - It sends the 64 lowercase hex digits, which `hex64_ok` accepts.
   - It fails with `failed` before any UAC prompt when there is no DLL.

   `win_app/tests/test_vcam_install.py` asserts the new line (`vcam-install --owner-sid <SID> --dll-sha256
   <hex>`), the hash of the file the lookup rule picks, and no prompt without a DLL.
2. **No deletion through a junction** (`965ad6e`, `win_app/Install-Maintenance.ps1`). The elevated
   `Remove-BeamerCamera` and `Remove-BeamerCameraStaleVersions` deleted folders with `Remove-Item -Recurse`,
   and Users may create folders under `%ProgramData%`. Folders are now deleted by `Remove-BeamerTree`:
   - It reads each entry's attributes with `[IO.File]::GetAttributes`, which also works on a dangling link.
   - It removes a reparse point (junction or symbolic link) as a link: `Directory.Delete(path, false)` or
     `File.Delete`. It never enumerates through it.
   - It recurses into real directories only.

   Two further guards:
   - A camera folder whose `Beamer` parent is a link is not ours: it is left alone, with a warning.
   - Stale versions are skipped when `Camera` or its parent is a link.

   Registry keys keep `Remove-Item`.

   Tests in `MaintenanceCameraBehaviourTests`, run on lebig with the real script under Windows PowerShell
   5.1.26100, in the ssh session. That session is elevated, like the uninstaller; creating the junctions needs
   no privilege. Each test plants a victim folder and checks it is untouched:
   - junctions inside `Data\Beamer\Camera` and inside a version folder: removed, victim kept;
   - the `Camera` data folder itself a junction: the link is removed, victim kept;
   - the `Beamer` parent a junction: nothing is deleted through it;
   - junctions among stale versions: links removed, victim kept.

   **Before/after:** with the previous script, only the parent-junction test fails: it deleted the victim's
   `Camera\keep.txt`. On this Windows build, `Remove-Item -Recurse` already removed child junctions as links.
   The new code does not rely on that behaviour of a particular PowerShell build.

## Residual and notes

- **Clock skew > 10 min.** Route changes made on a machine whose clock is more than 10 min ahead are dropped
  by the others, with a warning in their log. The camera still stops (sink enforcement), but the pages
  disagree on the route until a change is made on a machine with an accepted clock. Before, the limit was one
  day, and a stamp up to a day ahead poisoned every later change, so the limit is now tighter on purpose. A
  page notice for "this machine's clock is far ahead" would need a new notice and a wire signal: not done.
- `camera_devices` revs keep the one-day check (`wire.py`). A list from a machine more than one day ahead is
  still refused (fail closed: no demand seen, camera off).
- New page strings (two notices) are not in `locales/`: the catalogue check was already failing on this
  branch, and F1 must not commit `locales/`.

## Test runs

Mac (`mac_app/.venv` python, private `git archive` export of HEAD with F1's files overlaid):
- `core/tests -p 'test_camera_*.py'`: **180 tests OK**. This includes the 200-seed convergence run with the
  invariant checker.
- `core/tests` (all): 2196 tests. Failures:
  - `test_profiles_migration` (2) and `test_tile_resize` (2) error because the export has no git history to
    `git archive` old revisions from. They fail the same way on an untouched HEAD export.
  - `test_i18n_coverage.test_catalogues_are_current_and_complete` fails. The catalogue was already stale at
    HEAD (`i18n_extract.py --check` rc 1 there).
- `mac_app tests.test_camera_section`: **18 OK**.
- `win_app tests.test_vcam_install`: **38 OK** (17 Windows-only skipped).

lebig (`C:\Users\Alex\camera-f1\`, `win_app\.venv` python):
- `core/tests -p 'test_camera_*.py'`: **180 OK** (1 skipped).
- `win_app tests.test_vcam_install`: **38 OK**, nothing skipped, including the PowerShell `OwnerSid` and
  junction tests against the real script.
- `win_app` full suite (`discover -s tests`, `git archive` of `965ad6e`): 4340 tests, failures=2, errors=6.
  This is the **same set as a `git archive` of the commit before F1** (4309 tests):
  - `test_admin_mode` and `test_admin_mode_ui` fail to load (loader errors).
  - `test_profiles_migration` (2) and `test_tile_resize` (2) error: there is no git history in an export.
  - `test_capture_win.LayoutTests.test_a_shifted_digit_gives_the_symbol_the_layout_prints` fails (keyboard
    layout).
  - `test_i18n_coverage.test_catalogues_are_current_and_complete` fails (catalogue already stale).

  An earlier run from a macOS `tar` also showed `test_i18n_coverage`, `test_no_copies` and `test_wire_vectors`
  errors. They came from AppleDouble `._*` files in that tar, and the clean export does not have them.
