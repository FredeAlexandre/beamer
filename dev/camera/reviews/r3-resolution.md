# R3 resolution (native review): F2

Branch `feat/camera`. Review: `r3-native.md` (head `67ac12e`). Fix commits:

| Commit | Scope |
|---|---|
| `ef944c6` | engine: #2 (engine rule), #9, #10, #13, with engine tests |
| `dfd3052` | Windows: #1, #2 (sink), #7, #8 + operator ruling 4 (`--dll-sha256`), #11, #12, `video_check` additions |
| `b45f2ab` | Mac backend: #3, #4, #5, #6, `video_check reentry` |

Every finding was re-checked in the code before the fix; all 13 hold as described. "Before" below means the code
at `94c5357` (this branch's head before F2), built the same way, with only the test added.

## Findings

| # | Sev. | Verdict | Fix | Test (before -> after) |
|---|---|---|---|---|
| 1 | critical | fixed (`dfd3052`) | see "1" below | PC, see "1" below |
| 2 | major | fixed (`ef944c6` engine, `dfd3052` sink) | see "2" below | engine test `video_new_decoder_backlog`: first frame **1188 ms, 2 IDRs** -> **170 ms, 1 IDR**; PC `video_check scenario devloss` |
| 3 | minor | fixed (`b45f2ab`) | `~MacCameraHandle` starts with `handle_closing()` (erase from `handles_` under the backend `mu_`); `resolve_lost` keeps `mu_` until it is done with the handle (`emit_lost`, `camera_lost`); lost reports carry a per-handle generation (a later handle at the same address is not matched) | code only: the race needs a preferred-camera change during a stream stop (real cameras); no harness can drive it |
| 4 | minor | fixed (`b45f2ab`) | `CameraSource` is `enable_shared_from_this`; every block on `ctl_q_` (start, retry, the four notifications, the 1 s timer) holds a `weak_ptr` and locks it for its whole run; the start block logs with a copied key | code only (needs a real camera stopped during `startRunning`); Mac build clean |
| 5 | minor | fixed (`b45f2ab`) | `set_format` called inside the handle's own `EncoderSink` callback (thread-local marker) is deferred to the stream queue (drained by the destructor); destroying the handle there aborts with a message; class comment corrected | Mac `video_check reentry`: set_format in `on_frame` **deadlock (killed at 15 s)** -> returns in 0.03 ms, next IDR at 640x360; destroy in `on_frame` **silent, exit 0 (use after free)** -> SIGABRT with the message |
| 6 | minor | fixed (`b45f2ab`) | every wake attempt counts (max 20, also without a buffer); the wake encodes under a new encoder lock (`enc_mu_`), never the handle lock `mu_`; `set_bitrate`/`set_fps` never wait for an encode (applied by the next one) | `reentry`: no buffer -> 20 attempts at 5.5 s and at 7 s, 0 encodes; worst `request_idr`+`set_input_paused` during the wake **33.9 ms -> 0.70 ms** |
| 7 | minor | fixed (`dfd3052`) | the helper opens `.Shm` (and `.Frame`) with `READ_CONTROL` and uses them only when LocalService or SYSTEM owns them; else `vcam` `error: "squatted"`, retried every 1 s | PC, section owned by Administrators (`standin --foreign-owner`): old helper **in_use, error null** (would write pictures into it) -> `error "squatted"`, not used; owner SYSTEM (as the DLL's LocalService): used, `loaded_version 1` |
| 8 | minor | fixed (`dfd3052`) | `--dll-sha256` (ruling 4, below); source read with `FILE_SHARE_READ` only; `--test-dir` needs `--test-reg` (the real COM class never points into a test folder); version folder gets an explicit protected ACL (owner Administrators; SYSTEM/Administrators full, Users/LocalService/AppContainers read+execute) | PC: no hash / short hash rc 2; mismatch rc 4 `dll_hash_mismatch`, no DLL, no `.new`, no registry; `--test-dir` alone rc 2 |
| 9 | minor | fixed (`ef944c6`) | `Pacer::remove_stream` waits until the pacer thread holds no reference to that stream's client (in-flight marker + condition variable; never from the pacer thread itself); the pacer drops `client` outside its lock | engine test `video_pacer_never_last_owner`: **client destroyed on the pacer thread** -> on the owner's thread after `remove_stream` |
| 10 | minor | fixed (`ef944c6`) | `ingest` bumps a packet counter under the stream's `mu_`; the video thread waits only while it is unchanged | engine test `video_wakeup_not_lost` (packet injected between pump and wait, right after a tick): **26-34 ms** -> **0.03-1.3 ms** |
| 11 | minor | fixed (`dfd3052`) | linear output buffers use `MF_MT_DEFAULT_STRIDE` or the coded width as pitch (also for the buffer we allocate); every copy is bounded by `nv12_bytes_read()` against the locked length (2D: from `start + blen`; D3D: texture size); a short buffer is a decoder error, never shown | PC `video_check cropcheck` on a 1272x720 stream (coded 1280): software **1428/1442 rows sheared** -> 0; hardware 0 -> 0; 1280 control 0 -> 0; `unit` adds the extent checks |
| 12 | minor | fixed (`dfd3052`) | the delivery thread holds a COM reference to its `Stream` and a module reference (`FreeLibraryAndExitThread`), so the bounded `Shutdown` wait can never free it under the thread; `Source::Release` and `~Source` run under `g_parent_mu`, which `GetMediaSource` also takes, and `~Source` clears `parent_` first: no resurrection | PC: 20 quick opens/closes 20/20 ok (31 frames, 0 gaps each), one Frame Server process throughout, Frame Server stops 30 s after the last close (no thread or module left behind) |
| 13 | minor | fixed (`ef944c6`) | `VideoSendStream` keeps a registry handle -> (stream id, serial) while it holds the handle; `camera_lost` resolves `h` on the backend's call (handle alive) and the helper stops that id only if its stream has the same serial. Only a loss reported while `open_camera` is still returning falls back to the old pointer match (no reuse is possible in that window) | engine test `video_camera_lost_owner`: two streams whose handles share one address resolve to different (id, serial); a stopped stream's handle resolves to nothing |

### 1. `%ProgramData%\Beamer\Camera` takeover and link following

**Choice: refuse, not take over.** A folder that already exists with an owner other than Administrators/SYSTEM, or
that is a junction/symbolic link, stops `vcam-install` with `unsafe_folder: <path> ... delete it (as administrator)
and install again`. Taking ownership and resetting the ACL cannot undo what the folder's maker already did (child
objects they own, hard links, links deeper inside, handles opened before the reset), so the folder would still have
to be audited or deleted, and deleting attacker-controlled content as administrator is the very primitive being
removed. Refusing costs a denial of install that any administrator clears; it never runs code or writes files as an
administrator in a place a user shaped.

**Install** (elevated): `Program Files\Beamer`, `\Camera`, `\<v>` and `ProgramData\Beamer`, `\Camera` are each
created or opened with `NtCreateFile` relative to the parent's handle with `FILE_OPEN_REPARSE_POINT` (a link is
opened as itself), created with owner Administrators and a protected ACL, and kept open without delete sharing
while the next one is made. An existing one must be a real folder owned by Administrators or SYSTEM; its ACL is
then reset to today's protected ACL (an earlier install's `ProgramData\Beamer` had ProgramData's inherited ACL, in
which any user can create sub-folders). ACLs: `ProgramData\Beamer` SYSTEM/Administrators full, Users/LocalService
read; `\Camera` as §4.6.3 (SYSTEM, LocalService, Administrators full; Users read).

**Uninstall** (elevated): `Beamer\Camera` is deleted through handles (`FileIdBothDirectoryInfo` enumeration, each
entry opened relative and without following, disposition delete). A junction or symbolic link anywhere (including
`Beamer` itself) is deleted as the link; its target is never entered. Empty `Beamer` parents and the empty
`HKLM\SOFTWARE\Beamer` go too. Files left until reboot are scheduled only under `Program Files\Beamer`.

**DLL log** (LocalService): `Beamer`, `Camera` and `source.log` are opened relative to the ProgramData handle
without following links and without delete sharing; both folders must be real folders owned by
Administrators/SYSTEM, and `source.log` a plain file with one link (no symbolic link, no hard link). Rotation
renames the open handle in place (`NtSetInformationFile(FileRenameInformation)` with a bare name: no path is
resolved; `source.log.1` is replaced as a directory entry, a link there is replaced, never followed). Anything
unsafe: the line is not written. Not moved elsewhere: the log stays where §4.6.3 and the docs put it.

Found on the way: `SetFileInformationByHandle(FileRenameInfo)` resolves a bare name against the **process's current
directory** (kernel32 turns it into a path), which moved the copied DLL to `C:\Users\Alex` in the first PC run
(file removed). Both renames now call `NtSetInformationFile` directly.

**PC tests** (lebig, Windows 11 25H2, elevated over ssh; "user" = Alex's unelevated token through a scheduled task
in session 1; victims were folders I created under `C:\Users\Alex\camera-f2`):

| Case | Before (`94c5357` helper) | After |
|---|---|---|
| user pre-creates `ProgramData\Beamer` | install **ok**; `Beamer` stays owned by the user, who can still re-grant themselves full access | `unsafe_folder ... owned by lebig\Alex` rc 4, nothing created inside |
| user makes `ProgramData\Beamer` a junction to a victim folder | (install not run) | `unsafe_folder ... is a link` rc 4; victim untouched |
| uninstall with `ProgramData\Beamer` = junction to a victim with `Camera\keep.txt` | admin-made junction: **victim's `Camera` tree deleted** (keep.txt, sub\deep.txt). User-made junction: not followed on this build (see note) | link removed, victim intact (user-made and admin-made) |
| old-style `Beamer` (admin, ProgramData's inherited ACL) + user plants `Camera` as a junction | (old install would follow it) | `unsafe_folder ... Camera is a link`; uninstall removes the link only, victim intact |
| after a real install, the user tries: folder in `Beamer`, file in `Camera`, rename `Camera`, junction in `Camera`, write `source.log`, folder in `Program Files\Beamer`, overwrite the DLL, `icacls /grant` on `Beamer` | n/a | all **denied** (icacls exit 5) |
| DLL log, normal | | written (`stream: start ...`) |
| `source.log` > 1 MB and `source.log.1` = symbolic link to a victim file | (MoveFileEx path rotation) | rotated: `source.log.1` is now a real 1.1 MB file owned by LocalService, the link entry replaced, victim unchanged |
| `source.log` = symbolic link to a victim file | | not written, victim unchanged |
| `source.log` = hard link to a victim file | | not written, victim unchanged |
| `Camera` = junction to a victim folder | | nothing written in the victim |
| back to a normal `Camera` | | log written again |

Note: on this Windows build an elevated process cannot traverse a junction that a non-administrator created
(Get-ChildItem as administrator through it: "path not found"; the old uninstall did not delete through it), while
an administrator-made junction is followed. The fix does not rely on that (it never follows any link, for any
process, including LocalService), and the user-owned-`Beamer` path (rename `Camera` away and plant one) is open on
the old code regardless.

### 2. Second IDR when a decoder is (re)created while the IDR waits in the sink

**Engine rule** (`VideoRecvCore::on_sink_event`): on `decoder_hw`/`decoder_sw`, ask for an IDR only if AUs were
handed since the last (re)start **and** no IDR handed to the sink is still without a result. The sink reports every
AU it discards (`on_decoded(ok=false)`) before the decoder event through the same mailbox, so an IDR without a result
at that point is being held for (or queued to) the new decoder. Otherwise the request still goes through
`need_idr`, coalesced with a pending start/size request. The engine's existing start-PLI rules are unchanged.

**Hardware return after a GPU loss: choice: keep software until the stream's next IDR.** The standby hardware
decoder no longer reports `decoder_hw` when it is created; the sink switches to it at the next IDR it is handed (as
before) and reports `decoder_hw` just before that IDR's result, so the engine sees an IDR in the sink and asks for
nothing. No IDR is forced: an IDR is a burst the pacer spreads over ~100 ms and the frames behind it wait, i.e. a
small visible hitch for a gain the viewer cannot see, and IDRs come anyway at every stream start, size change and
failed refresh. Cost: software decoding (CW4: 10-15 % of a core at 720p, 15-27 % at 1080p) until then, at most for
the rest of that stream; each new stream gets a new sink and a hardware decoder.

**Measurements.**
- Engine test `video_new_decoder_backlog` (SenderRecovery + VideoRecvCore + a sink model with the Windows sink's
  event order, synthetic time; it is the reviewer's scratch reproduction). Decoder ready at +170 ms after the IDR
  arrived at +120 ms: **before: first frame 1188 ms, 2 IDRs, 11 PLIs after the decoder was ready; after: 170 ms,
  1 IDR, 0 PLIs**. Ready at +900 ms (slow GPU): 1221 ms / 2 IDRs -> 900 ms / 1 IDR. Device lost at 3.0 s, software
  decoder at 3.25 s with the requested IDR already kept: 3 IDRs, picture back after 1155 ms -> 2 IDRs, back after
  250 ms. Hardware take-over (`decoder_hw` with its IDR in the sink): PLI and revoked picture -> no PLI, shown.
- PC, real Windows sink (`video_check scenario devloss`, 75 s, C1's 720p recording looped, engine stand-in following
  the same rule): device lost -> software picture 126 ms later; **1 IDR asked for in total** (the loss); events
  `decoder_hw@100, decoder_sw@3090, decoder_hw@63121`: hardware back at the file's next IDR, 60.1 s after the loss
  (the 30 s retry was ready just after the loop's IDR at ~33 s), 2246 of 2248 AUs presented, built-in image never
  seen. CW4's run of the old sink had answered the standby `decoder_hw` at 30.0 s with a forced IDR.

## Tests run

| | Mac | PC (lebig) |
|---|---|---|
| Engine tests (audio + video), plain | **55 cases, 2563 checks, 0 failures** | **55 cases, 2563 checks, 0 failures** (146 s) |
| ASan/UBSan (`build-tests.sh --asan --run`) | 55 cases, 0 failures, no sanitizer report | n/a |
| TSan (`--tsan --run`, `halt_on_error=1`) | 55 cases, 0 failures, no TSan report (incl. `video_storm` and the 4 new cases) | n/a |
| Native build | `build-mac.sh --no-driver`: helper, `video_check`, `backend_check`, no warning | `build-win.cmd`: helper, DLL, tools; `build-tests.cmd` |
| `video_check` | `reentry` 4/4 | `unit` ok; `cropcheck`; `scenario status` (owner SYSTEM / `--foreign-owner`), `scenario devloss` |
| Install / DLL | | the PC tests of #1, #8, #12 above |

55 = the 51 cases CW2 had + `video_new_decoder_backlog`, `video_pacer_never_last_owner`, `video_wakeup_not_lost`,
`video_camera_lost_owner`.

PC cleanup verified at the end (see the final report): no `SWD\VCAMDEVAPI` device, no CLSID, no
`HKLM\SOFTWARE\Beamer` or `BeamerTest`, no `Program Files\Beamer` or `ProgramData\Beamer`, no test task, Frame
Server Stopped, firewall rules 632 before and after (no rule added), `C:\Users\Alex\camera-f2` removed. The PC clone
and the installed Beamer were not touched.

## For the operator / F1

1. **`--dll-sha256` is required** by `vcam-install` (rc 2 without it). I kept no hash-less path: the only caller is
   `win_app/vcam_install.py` (F1 passes the hash; `win_app/tests/test_vcam_install.py` asserts the old command line
   and needs the new one), plus the dev recipe in `native/audio/vcam-win/README.md` (updated). New error codes in the
   JSON: `unsafe_folder: ...`, `dll_hash_mismatch: ...`.
2. **`win_app/Install-Maintenance.ps1` `Remove-BeamerCamera`** (not in F2's paths) deletes `ProgramData\Beamer` with
   `Remove-Item -Recurse -Force`. Windows PowerShell 5.1 is known to recurse into junctions with `-Recurse`; it runs
   elevated. Suggest: remove a link with `cmd /c rmdir` (or `[IO.Directory]::Delete($p, $false)` on the link) before
   any recursion, or call `beamer-audio.exe vcam-uninstall`, which is now link-safe.
3. **PHASE2 wording** (not edited, per the brief): §4.6.1 says a new decoder "always gets an IDR ... the stream then
   sends PLI start"; the rule is now "PLI start only when the new decoder has no IDR to start from", and the hardware
   retry "takes over at the stream's next IDR". §2.4's `decoder_*` detail is unchanged.
4. `kDllVersion` stays 1: the DLL changed (log, teardown) but nothing about its contract, and no installed copy
   exists outside lebig. A machine with the old DLL keeps it until its next install.
