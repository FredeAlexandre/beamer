# R4: Python camera review (CW1, CW7, CW8, CW9)

Reviewer: R4 (independent). Branch `feat/camera`, reviewed at `8f9c9d3`. Contract: `dev/camera/PHASE2.md` §5-§9.
The reports (`cw1.md`, `cw7.md`, `cw8.md`, `cw9.md`) were read for intent only. Every claim below was checked
in the code. Findings marked "confirmed" were reproduced with throw-away tests on the cluster harness
(`core/tests/audio_cluster_harness.py`) in a private `git archive` export, not in the shared tree.

## Scope

`git log --oneline b7c5852..HEAD -- core mac_app win_app`:

```
dbeff7d Camera CW9b: real install, removal and update on lebig; vcam_install log fix; dev-install README
ba4ab04 Camera: after a DLL update, ask to close camera apps, not to restart Windows (CW5 measured)
59a2bfd Sidebar: show '&' in page names (Audio & Camera read as Audio_Camera)
4d99847 Camera CW6: link and service integration (...)          <- CW6: not reviewed (out of scope)
0449147 Camera CW7: Mac Camera module, map marks, menu items (PHASE2 §8.3-§8.5)
8c4a23f Camera CW8: Qt Camera module of the Audio & Camera page (PHASE2 §8.3-§8.6)
3e7991a Camera CW1: harness independent of CW6's names; CW1 report
fc73df0 Camera CW1: cluster sequences also refuse by install state and recv error
5b9f98f Camera CW1: CameraPart, fake video helper, cluster harness with the camera-on invariant checker
cde2cad Camera CW7: core/camera/view.py page model contract (PHASE2 §8.2), milestone 1
6887b20 Camera CW1: camera_* wire, planner and camera.json store (PHASE2 §5, §6.4, §7.3)
d6859c5 Camera CW9: packaging, install and permissions (PHASE2 §9)
bf21597 Camera CW1: placeholder texts (PHASE2 §7.5), milestone 2; i18n tools scan core/camera
b9116dd Camera CW1: core/camera/model.py contract (PHASE2 §7.2), milestone 1
```

Diff stat for the files in scope (CW6 files excluded):

```
 core/audio/fake_helper.py                |   32 +-
 core/audio/view.py                       |   39 +-
 core/camera/__init__.py                  |   14 +
 core/camera/fake_video.py                |  338 ++++
 core/camera/model.py                     |  366 ++++
 core/camera/part.py                      | 1003 ++++++++
 core/camera/planner.py                   |  196 ++
 core/camera/store.py                     |   74 +
 core/camera/texts.py                     |   92 +
 core/camera/view.py                      |  607 +++++
 core/camera/wire.py                      |  351 +++
 core/tests/audio_cluster_harness.py      |  316 ++-
 core/tests/camera_fixtures.py            |  249 ++
 core/tests/test_camera_*.py (8 files)    | 2847 ++++++++
 mac_app/Install-Beamer-Mac.command       |    6 +-
 mac_app/audio_page.py                    |  135 +-
 mac_app/build_dmg.sh                     |   12 +-
 mac_app/camera_section.py                |  244 ++
 mac_app/entitlements*.plist              |    9 +-
 mac_app/kvm_bridge_app.py                |   71 +
 mac_app/pages.py                         |    7 +-
 mac_app/setup.py                         |    8 +
 mac_app/tests/test_camera_section.py     |  397 +++
 win_app/Beamer-Setup.iss                 |   16 +
 win_app/Install-Beamer.ps1               |   23 +
 win_app/Install-Maintenance.ps1          |   72 +
 win_app/audio_page_win.py                |   79 +-
 win_app/build_portable.{ps1,spec}        |   11 +-
 win_app/build_win_app.ps1                |    9 +
 win_app/camera_section_win.py            |  508 +++
 win_app/pages_win.py, widgets.py         |   11 +-
 win_app/tests/test_camera_section_win.py |  549 +++
 win_app/tests/test_vcam_install.py       |  405 +++
 win_app/vcam_install.py                  |  196 ++
```

Not reviewed (CW6, still moving): `core/audio/service.py`, `core/link.py`, `core/receiver.py`,
`core/feature_flags.py`, `mac_app/bridge.py`, `win_app/sender.py`, `kvm_bridge_win.py`, `WIRE.md`. I read
`service.py` only to see how it calls the part (worker queue, `apply`, `_helper_info`, flow agreement). I
make no claims about it.

## Summary

| Severity | Count |
|---|---|
| critical | 0 |
| major | 3 |
| minor | 5 |

## Findings

### 1. major: an "Off" (or any route change) can silently fail to reach the camera machine, and the sink keeps the stream. The PC page then says Off while the Mac camera keeps streaming into the PC's apps (confirmed)

**Where**: `core/camera/wire.py:280-284` (route `extra` accepted with no stamp, size or depth check),
`core/camera/model.py:188-198` (`max_stamp` and `next_stamp` include `extra` stamps),
`core/camera/wire.py:134-142` (`_stamp` rejects `set_at > now + DAY`), `core/camera/part.py:187-197`
(`_post` drops a message over 16 KiB, with only a log line), `core/camera/part.py:924-936` (`_check_recv`
closes a recv only when the install breaks, never when the route no longer names the sender).

**Scenario A (confirmed)**: a peer sends a `camera_route` with an unknown top-level entry stamped
`{"set_at": 2**53, "by": ""}`. The rule says unknown entries are kept and forwarded (§6.2). `read_route`
accepts it because only `camera` and `quality` stamps are validated. From then on `next_stamp` on that
machine returns `2**53 + 1` for every local change. Every peer's `_stamp` rejects that value (it is greater
than `MAX_STAMP` and than `now + DAY`), so `read_route` drops the **whole** message. In the test, Alex set
Off on the PC while the PC app was using the Mac camera. The PC route became `machine=None`. The Mac route
still named the Mac, the Mac camera stayed on, and the PC's `_recv` stayed `running`. The PC status read
`off` and the Mac status read `streaming`. The bad entry is also persisted in `camera.json`, and the merge
answer sends it back to the author, so the divergence lasts across changes. The only fix is a change made
on the Mac.

**Scenario B (confirmed)**: a route whose unknown entry is about 17 KB. The PC accepts it, because nothing
bounds an incoming message (the link frame limit is 16 MiB, `core/protocol.py:82`). After that, every
`wire.route(...)` the PC builds is over `MAX_SIZE`, and `_post` drops it. Same result: Off on the PC never
reaches the Mac, and the camera keeps streaming.

**Scenario C (confirmed)**: the PC's wall clock is more than one day ahead of the Mac's, for example after a
CMOS reset or a VM snapshot. Every route stamp from the PC fails the Mac's `set_at <= now + DAY` check. Off
on the PC: the Mac keeps streaming, and the PC shows Off. A Mac clock more than one day ahead hits the same
check on `camera_devices` revs (`wire.py:229`), so the PC never sees the Mac's list.

Any of these, together with a sink that relies only on the sender to close (§6.4 rule 4), means the
choice Alex made on the PC is not enforced. The page shows a state ("Off") that is false. The fake-helper
invariant checker does not catch it, because the receiver still wants the stream.

**Fix**:
- (a) Validate `extra` in `read_route`: each value is a dict whose `set_at`, when present, passes `_stamp`;
  bound the encoded size of all extras, e.g. 4 KiB. Reject the whole message when this fails, as for a bad
  `camera` entry.
- (b) Clamp `max_stamp` to the stamps the wire would accept. Better: compute it only from validated
  entries.
- (c) Make the sink enforce its own view. In `_check_recv`, close a recv whose `peer !=
  self._route.camera.machine` with `camera_close {reason:"route"}` and a helper `stop`. That makes any
  route divergence fail safe: the sink stops taking the stream.
- (d) Log at warning level, at most once per peer, when a `camera_route` from a direct peer is rejected
  for its stamps. Today it is a debug line.

### 2. major: stale demand turns the Mac camera on for about 3 s after a link comes back or Beamer restarts, when the PC app closed during the outage (confirmed)

**Where**: `core/camera/part.py:341-355` (`link_down` leaves the peer's `camera_devices`, with its
`in_use`, in `_machines`), `core/camera/part.py:134` and `core/camera/store.py:51-56` (lists held from
`camera.json`, `in_use` included), `core/camera/part.py:795-803` (`_track_demand` starts the 3 s hold on
any True to False edge, including one between a stale list and a fresh one).

**Scenario (confirmed on the harness)**: the Mac streams to Teams on lebig. The link drops (Mac sleep,
Wi-Fi blip, or Beamer quits on the Mac). Teams closes on lebig while the link is down. The link comes
back, or the Mac's Beamer restarts. The Mac still holds lebig's list with `in_use: true`, in memory or
reloaded from `camera.json`. Then either:
- it plans and opens a `send` and `camera_open` before lebig's fresh list arrives; or
- lebig's fresh list (`in_use: false`) arrives first, and `_track_demand` sees True to False and grants the
  3 s `DEMAND_HOLD`.

In both cases the planner sends. The receiver accepts any `camera_open` from its route's camera machine
without checking its own demand (§6.4 rule 5), so the start PLI comes back. On the harness the Mac camera
was on for 3.0 s with no app using it. The invariant checker passed only because 3.0 s is under its
3.5 s bound, but §6.5 allows the start window only for "a T **with demand** that has not answered yet".
The green light comes on at every wake from sleep after a call ended during the sleep.

A related path, plausible and unverified: `_helper_gone` (`part.py:280-293`) does not clear `self._vcam`.
The service does not clear `_helper_info` features on a helper restart (`service.py:279/1084`). So during a
PC helper restart, the PC keeps publishing its last `in_use: true` until the new helper's first `vcam`
event. If the app closed during the restart, the same 3 s hold follows.

**Fix**:
- Treat demand as known only from a list received on a live path. On `link_down` of a peer (or when a
  machine leaves `online`), set the held `virtual_camera.in_use` to false. Do not do it in `_in_use` and do
  not start a hold.
- On load, store `in_use: false` for every held list, or drop the `virtual_camera` demand fields.
- Only start the `DEMAND_HOLD` for a True to False edge whose True was observed while that machine was
  online.
- Clear `self._vcam` in `_helper_gone`.
- Optionally (spec change), let a sink refuse `camera_open` with no demand and no hold, as a second guard.

### 3. major (low probability): the menu's "Stop Sharing the Camera" can turn sharing ON

**Where**: `mac_app/kvm_bridge_app.py` `toggle_camera_sharing` (line 5762).

The item's title is set from the snapshot by the 0.4 s status timer. The rumps timer runs in
`NSDefaultRunLoopMode` only (`rumps.py:712`), so the title is frozen while the menu is open. The click
handler does not act on the title. It re-reads the **current** snapshot and inverts it
(`ACTION_STOP_SHARING if menu.sharing_on else ACTION_SHARE`).

**Scenario**: Alex clicks "Stop Sharing the Camera". The worker turns sharing off at once, but the title is
only rewritten on the next tick (`_camera_items()` right after the click still reads the old snapshot,
because the setter is queued). Alex reopens the menu within about 0.4 s and sees "Stop Sharing the Camera"
again, as if the click had not worked. Alex clicks it again. The handler now reads `sharing_on == False`
and sends **Share**. The same inversion happens when sharing is turned off on the page just before the
menu is opened. The kill switch then does the opposite of what its label said.

Confirmed by reading; not run, because it needs AppKit. The page's row buttons are not affected: they
carry explicit `ACTION_SHARE` and `ACTION_STOP_SHARING` values from the section that was shown.

**Fix**: remember the action the title promised when `_camera_items` sets it, for example
`self._camera_sharing_action = ACTION_STOP_SHARING if menu.sharing_on else ACTION_SHARE`, and send that.
Also set the opposite title optimistically right after the click.

### 4. minor: the sharing switch fails open on persistence problems

**Where**: `core/camera/part.py:559-564` (`set_sharing` only schedules the 500 ms debounced save),
`core/camera/store.py:44-48,60` (a missing, unreadable, malformed or other-version file, or a non-bool
`sharing`, loads as `sharing=True`), `core/camera/part.py:210-215` (a failed save is only logged).

**Scenarios**:
- Beamer is killed (crash, force quit) less than 500 ms after "Stop Sharing".
- `camera.json` is corrupt, or was written by a later format version after a downgrade.
- The save fails because the disk is full or of a permission error.

In each case the Mac restarts with sharing **on**. The route held by peers still names the Mac and comes
back by flooding, so the next PC app that opens Beamer Camera turns the Mac camera on. Alex turned it off
and saw no error.

**Fix**:
- Save immediately (not debounced) when `sharing` changes.
- When the file exists but cannot be read, or has an unknown version, load `sharing=False`; only a
  missing file means "never set".
- Surface a failed save of a sharing change as a notice.

### 5. minor: held camera lists are unbounded and never pruned

**Where**: `core/camera/part.py:365-378` (`_on_camera_devices` keeps a list for every machine id it
hears of), `store.py` (all of them are persisted), `part.py:688-700` (`_greet` sends all of them to every
new peer).

A buggy or hostile peer can flood distinct machine ids, each message under 16 KiB. `camera.json`, memory
and every greeting then grow without limit. Lists of machines that left the cluster are kept forever.

**Fix**: cap held lists, for example at `protocol.MAX_PEERS`, evicting the oldest offline one, and drop
lists of machines not seen online for a long time (the audio part's TTL, if it has one).

### 6. minor (plausible, design level): the elevated install runs a user-writable binary and copies a user-writable DLL into Frame Server

**Where**: `win_app/vcam_install.py:115-128,160-187`, `win_app/Beamer-Setup.iss:35,42`
(`DefaultDirName={localappdata}\Beamer`, `PrivilegesRequired=lowest`), and the PHASE2 §9.2 lookup rule.

`beamer-audio.exe` and `camera\BeamerCameraSource.dll` live in `%LOCALAPPDATA%\Beamer`, which any process of
the user can write. Alex's UAC "Yes" runs whatever `beamer-audio.exe` is there, elevated. It also
registers whatever DLL sits in `camera\` as the COM server that Frame Server (LocalService) loads.
Medium-integrity malware can therefore turn one Install click into SYSTEM-adjacent code execution. The
same pattern already exists for `Invoke-InstallMaintenance.ps1` in `{app}`, so this is not new to the
camera, but the camera adds a service-loaded DLL.

**Fix**: when a signing identity exists, have `vcam-install` verify the Authenticode signer of the DLL
(and the helper of itself) before copying. Or stage the DLL from an admin-only location. This is native
code (CW5/CW9 helper verb); record it as a §11 risk if it is not fixed.

### 7. minor (plausible, unverified): uninstalling Beamer for one Windows account removes the machine-wide camera of every account

**Where**: `win_app/Install-Maintenance.ps1` `Remove-BeamerCamera` (line 167), run from the
per-user Inno uninstaller (`[UninstallRun] ... -Remove`).

Beamer installs per user, but the camera registration is machine-wide: the HKLM CLSID,
`%ProgramFiles%\Beamer\Camera` and `HKLM\SOFTWARE\Beamer\Camera` with a single `OwnerSid`. Uninstalling
Beamer in account A deletes all of it without checking `OwnerSid`, so account B's Beamer Camera breaks.
The uninstaller may also stop `FrameServer`, pausing every camera on the PC, and unlike the page it does
not warn about that. `vcam-install` from account B likewise overwrites the `OwnerSid` of A.

**Fix**:
- Make `Remove-BeamerCamera` remove the machine-wide parts only when `OwnerSid` is the uninstalling
  user's, or when no other profile has Beamer installed.
- Otherwise only `vcam-remove` this user's camera.
- Say in the uninstaller that cameras may pause for about a second.

### 8. minor: a sink's normal close (route, demand, shutdown) is shown as a camera error on the source

**Where**: `core/camera/part.py:452-463` together with `_compose_snapshot` (`part.py:966-972`).

A `camera_close` from the sink with reason `route` (the sink accepted another source's stream), `demand`
or `shutdown` marks the send as failed with that reason as its error. The Mac page then shows "The camera
stopped on this Mac." with a Retry button, and `REASONS` has no text for `route`, `shutdown` or `demand`.
For `shutdown` the link-down that follows clears it. For `route` (two machines both believe they are the
source during a route race) the error stays until a token change. The route merge brings one soon, so
this is cosmetic, but Alex sees a fault tone for a normal handover.

**Fix**: for close reasons `route`, `demand` and `shutdown`, drop the send (`_close_send` without a post)
instead of marking it failed. Keep the failure path for `error`, `helper` and `no_receiver`.

## Checked and found sound

- **Sharing switch cannot be set from outside the Mac.** `set_sharing` is reachable only from the local
  service queue (page row, menu, dev file). `_on_camera_devices` ignores a list that describes its own
  machine, except to raise `own_rev` (`part.py:366-371`). `self._machines` never holds a peer's version of
  the own list, because the store load filters it out. The planner reads S's `sharing` from the list S
  composes itself (`_compose_own`), so a forged or forwarded list with `sharing: true` for the Mac changes
  only what other pages display, never what the Mac opens. Sharing off yields `sharing_off` before the
  test-source and permission checks (`planner.py:80-83`). The cluster test `test_restart_keeps_route_and_sharing`
  and my reading agree that it survives a normal restart (see #4 for the abnormal ones).
- **Permission prompt.** `permission_request` is sent only from `ask_permission`, which is called from the
  page or menu action, or from `_set_camera(..., local_action=True)` when the machine is its own and
  permission is undetermined. `_on_camera_route`, the dev `set_camera` (`local_action=False`) and every
  helper or peer handler never reach it. Sends require `permission == "granted"` (or the dev synthetic source,
  which needs a `--dev` helper announcing `camera_synth`). `video_warm` follows the same gate.
- **`camera1` gating.** Every `camera_*` post goes through `_post`, which requires the peer to be in
  `peers_with("camera1") & direct()`. That covers greetings, forwards, `camera_close` on teardown and
  answers. Video `link_out` frames go only to peers that have a send, and sends exist only for gated peers.
  A peer that re-hellos without `camera1` has its send closed (with the helper `stop`) and is sent nothing.
- **Malformed peer input.** `wire.read` catches `TypeError`, `ValueError`, `KeyError` and
  `AttributeError` around every validator. The handlers then use only validated fields. Limits on cameras,
  native formats, key, name and app lengths, effects, codec ranges, stream ids, flow shape and the
  base64 key size match §5.1. Unknown fields are dropped. `camera_close` and `camera_opened` act only
  when both peer and stream id match. In the service hunk, the worker wraps every event in a `try`.
- **Refusal and backoff (§6.5).** A refusal sends a helper `stop` in the same handler (`_send_failed`,
  with `notify=False`). A helper `send` error reply marks the send failed without a second `stop`. Re-opens
  require a changed `(route, T rev, flow)` token, or, for `no_receiver` only, the 10/30/60 s backoff. The
  backoff resets on `running` and when the demand ends. Wakes are scheduled only for failed sends with a
  future backoff. The `not_source` republish (`_not_source`) bumps the sink's rev only when its route
  view's camera machine changes while it has demand.
- **Answerer and offerer flow agreement versus `camera_open`.** The answerer agrees before it posts
  `audio_flow answer`, and the offerer agrees on receipt, so a `camera_open` (posted after the sender's
  agreement, on the same link) never reaches a receiver that has not agreed. No `helper` refusal loop
  comes from this.
- **Threading.** All part methods run on the worker. The service setters only enqueue.
  `camera_snapshot()` returns the last published object, and the page and menu models are pure and cheap.
  `vcam_install` calls `on_done` once, on its thread, and catches exceptions in both the steps and the
  callback. The Qt module relays the result through a `QueuedConnection` signal on an object owned by
  the GUI thread, and `refresh_camera_install` is called only from `_finished` on that thread.
- **`vcam_install.py`.** The SID is checked against `S-1-\d+(-\d+)+` before it is passed. Arguments are
  built with `list2cmdline` from fixed words. The `runas` cancel maps to `cancelled` (1223 via `errno`). A
  timeout or a missing process maps to `failed` rather than hanging. The unelevated `vcam-remove` failing
  does not block `vcam-uninstall`. The helper path comes from `audio_helper.locate()`, never the dev null
  helper.
- **PowerShell.** Paths are used with `-LiteralPath`. The `Beamer` parent is deleted only when it is
  empty and has no values. Stale-version cleanup skips the registered folder (compared case-insensitively
  on full paths) and never fails an install. `Remove-BeamerCamera` runs after the firewall and task steps,
  so a camera failure does not leave them behind. `{app}` is `%LOCALAPPDATA%\Beamer`, so
  `%ProgramFiles%\Beamer\Camera` can never collide with or delete the shipped `{app}\camera` copy.
  `Install-Beamer.ps1` touches only `{InstallDir}\camera`.
- **Store.** Writes are atomic (temporary file, `fsync`, `os.replace`). Load re-validates the route and
  every list through the wire readers, and drops a list whose id is not its key.
- **Page models.** The Windows install, update, repair and remove buttons are hidden while any video
  stream runs. Retry re-stamps the same route, so the token changes. A PC Retry targets the Mac and so can
  never prompt there. The PC row's restart/close-apps note follows `loaded_version != dll_version`.
