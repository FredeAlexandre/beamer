# Beamer Camera source (`BeamerCameraSource.dll`)

The Windows half of "Beamer Camera": a Media Foundation media source that the Windows Camera Frame Server
(`svchost -k Camera`, LocalService) loads for the virtual camera the helper creates with
`MFCreateVirtualCamera` (System lifetime, CurrentUser access). The helper (`beamer-audio.exe`, Alex's session)
decodes the Mac's stream and writes pictures into the shared memory this DLL owns. Design: `dev/camera/PHASE2.md`
§4.6.2-4.6.4 and §9.2; measurements: `dev/camera/reports/cw5.md`.

| File | What |
|---|---|
| `shm_protocol.h` | The contract with the helper (CW4): section layout, names, CLSID, offered media types. Frozen; change only with the operator. |
| `source.cpp`, `source.def` | The DLL: activate -> source -> one shared NV12 stream at `kOffered`; owns the section and the frame event; validates everything the helper writes; built-in image when nobody feeds it. |
| `builtin_image.h`, `placeholder_builtin.inc` | The built-in image ("Beamer Camera / Waiting for Beamer", PNG decoded with WIC) and the moving dot. The `.inc` is generated: `mac_app/.venv/bin/python native/audio/vcam-win/tools/make_builtin.py` on the Mac. |
| `build.ps1` | Builds `<out>\camera\BeamerCameraSource.dll` and `<out>\camprobe.exe`, `feeder.exe`, `shmfuzz.exe` (out = `native\audio\build\win`). Called by `native\audio\win\build.ps1`; runs alone too (finds MSVC). |
| `tools/camprobe.cpp` | A consumer: `list` (MF + DirectShow), `read` (IMFSourceReader; classifies every frame: live picture with the C2 stamp, slot 3, C1 code strip `--strip c1`, built-in image, uniform, other; latency from the stamp), `yuvcheck` (raw NV12 from ffmpeg dshow). |
| `tools/feeder.cpp` | The helper stand-in: `run` (demand, beat, live or waiting pictures, wedge simulation), `vcam create/remove`, `inspect` (owners, DACLs, header), `squat`, `loadtest`, `cpu`. |
| `tools/shmfuzz.cpp` | The hostile writer: random and boundary values in every helper-written field at 1 kHz. |
| `tools/devinstall.ps1`, `devuninstall.ps1` | Elevated dev stand-ins for `vcam-install` / `vcam-uninstall` (same paths, keys and ACLs as §9.2). |
| `tools/insession.ps1` | Runs a command in the signed-in user's session from ssh (scheduled task), as the helper runs. |

## How the DLL behaves

- **Only inside Frame Server.** Any process whose token is not LocalService (the helper, when
  `MFCreateVirtualCamera` validates the DLL) gets an inert source: the three media types, built-in image if
  started, no kernel object, no log, no registry access.
- **Objects.** `Global\BeamerCamera.<InstallGuid>.Shm` (`kSectionBytes`) and `.Frame` (auto-reset) are created by
  the first source object in Frame Server and kept while any source object lives (Frame Server re-creates the
  source 2-3 times on every close, so between app sessions they usually do not exist; the helper polls every
  1 s and opens them on the `.Demand` event). DACL `D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;<OwnerSid>)`. An object
  that already exists must be owned by LocalService or SYSTEM, else it is refused (`squatted` in the log) and
  the camera shows the built-in image. `InstallGuid` is used verbatim as stored in the registry (the dev
  install writes `{UPPERCASE-GUID}`); `OwnerSid` must be a valid SID that is not a broad group.
- **Demand.** Start/Stop/Shutdown publish `streaming`, `want_*`, `state_qpc`, `state_seq + 1` and set
  `.Demand` (opened with `EVENT_MODIFY_STATE` each time). Every Stop and Shutdown publishes, so repeated stops
  are visible and harmless.
- **What it shows** (checked on every delivery, §4.6.4): `beat_qpc` older than 500 ms, zero or invalid ->
  built-in image + dot; `mode` waiting -> slot 3 + dot; live/hold -> the `latest` slot, repeated (counted in
  `repeats`) when no newer picture arrives within 1.5 frame periods. A fresh picture is delivered as soon as a
  request is pending (frame event), placeholders and repeats on a 30 fps high-resolution timer.
- **Hostile input** (§4.6.3): geometry from constants only; `beat_qpc`, `mode`, `latest` and each slot's
  `w`/`h`/`stride` read once and validated; the slot copied under its seqlock into a private buffer (two
  attempts, then the previous picture); scaling reads only that buffer. Every failed check adds 1 to
  `rejected` and logs at most once per second. The frame event is listened to at most once per 2 ms, so a
  writer setting it in a loop cannot spin the delivery thread. An unchanged slot (same index, `seq` and
  `written_qpc`) is not copied again: **the helper must keep incrementing `seq` (never reset it) and set
  `written_qpc` on every write.**
- **Log**: `%ProgramData%\Beamer\Camera\source.log` (folder made by `vcam-install`), token bucket of 3 lines
  refilled at 1 line/s, rotated to `source.log.1` at 1 MB.

## Updating the DLL

A new `kDllVersion` installs into a new `%ProgramFiles%\Beamer\Camera\<version>\` folder and re-points the
CLSID; Frame Server keeps the old DLL until it unloads it. What unloads it on this PC is measured in
`dev/camera/reports/cw5.md` ("DLL update").

## Test recipes (PC, from ssh; elevated)

```powershell
# build into a plain folder
powershell -ExecutionPolicy Bypass -File native\audio\vcam-win\build.ps1 -OutDir C:\Users\Alex\camera-cw5\bin
# install (dev stand-in for vcam-install), then create the camera in the user's session
powershell -ExecutionPolicy Bypass -File native\audio\vcam-win\tools\devinstall.ps1 -Dll C:\Users\Alex\camera-cw5\bin\camera\BeamerCameraSource.dll
powershell -ExecutionPolicy Bypass -File native\audio\vcam-win\tools\insession.ps1 -Command "C:\Users\Alex\camera-cw5\bin\feeder.exe vcam create"
# feed it and read it
insession.ps1 -Name CW5-feeder -NoWait -Command "...\feeder.exe run --follow --seconds 80"
insession.ps1 -Name CW5-probe -Command "...\camprobe.exe read --seconds 60 --size 1920x1080"
# fuzz it (while a camprobe read runs in the session)
insession.ps1 -Name CW5-fuzz -Command "...\shmfuzz.exe --seconds 600 --hz 1000"
# remove everything
powershell -ExecutionPolicy Bypass -File native\audio\vcam-win\tools\devuninstall.ps1 -Feeder C:\Users\Alex\camera-cw5\bin\feeder.exe
```

## Dev install and removal with the helper's verbs

This is what the page's "Install Beamer Camera…" and "Remove…" do (`win_app/vcam_install.py`), by hand.
`ssh lebig` sessions are elevated, so there is no UAC prompt. The owner is the account that runs Beamer (Alex):

```powershell
$sid = (New-Object Security.Principal.NTAccount 'lebig\alex').Translate([Security.Principal.SecurityIdentifier]).Value
$h = "$env:LOCALAPPDATA\Beamer\beamer-audio.exe"     # or a build: native\audio\build\win\beamer-audio.exe
$dll = Join-Path (Split-Path $h) 'camera\BeamerCameraSource.dll'
$sha = (Get-FileHash $dll -Algorithm SHA256).Hash
& $h vcam-install --owner-sid $sid --dll-sha256 $sha  # elevated; one JSON line, exit 0
& $h vcam-status                                      # install ok, dll_version, loaded_version, bundled_version, present
```

`--dll-sha256` is required: the helper copies the DLL into the protected version folder first, hashes the copy
there and registers nothing on a mismatch (`dll_hash_mismatch`), so a DLL swapped in a user-writable folder
between the hash and the elevated copy is never registered. Every folder (`Program Files\Beamer\Camera\<v>`,
`ProgramData\Beamer\Camera`) must be a real folder owned by Administrators or SYSTEM (it then gets a protected
ACL); one that a non-administrator made, or a junction/symbolic link, stops the install with `unsafe_folder`
and its path: delete it as administrator and install again (R3 #1).

Without `--dll` the helper installs `<its folder>\camera\BeamerCameraSource.dll` (the build and every package
put it there) into `%ProgramFiles%\Beamer\Camera\<kDllVersion>\`. The install creates no camera: a helper
running in the user's session creates it at its start or at its next `vcam` command (the running Beamer, or
`refresh_camera_install()` after the page's install). From ssh, start one in the session with
`tools\insession.ps1` and check with `camprobe list` (also in the session; the camera is CurrentUser).

Remove (script-installed or dev machines; the Inno uninstaller does this itself):

```powershell
& $h vcam-remove        # Remove() the account's camera: works unelevated in the session and from ssh
& $h vcam-uninstall     # elevated: COM class, HKLM\SOFTWARE\Beamer\Camera, the two Camera folders
```

`vcam-uninstall` deletes through handles and never follows a link (a junction there is removed itself, not
its target); it also removes the `Beamer` parents (`HKLM\SOFTWARE\Beamer`, `%ProgramFiles%\Beamer`,
`%ProgramData%\Beamer`) when they are empty. `Remove-BeamerCamera` from `win_app\Install-Maintenance.ps1` (dot-source the script
elevated, then call the function) removes the same items and those parents. Both stop `FrameServer` only when
the DLL is still loaded (an app has a camera open, or less than ~30 s since one closed). Exit codes: 0 ok,
2 bad arguments, 3 not elevated, 4 failed.

Updating: a helper built with a higher `kDllVersion` reports `install: outdated` until its `vcam-install` runs;
then `dll_version` is the new one and `loaded_version` the one Frame Server runs. The old version folder stays
while Frame Server holds it (`stale_left` in the install's JSON line) and goes at the next elevated action
(install, repair, uninstall, `Install-Maintenance.ps1`) once Frame Server has stopped, ~30 s after the last
app closed the camera. To load the new DLL at once on a dev machine: `Restart-Service FrameServer -Force`.
