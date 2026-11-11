# R1: review of `dev/camera/PHASE2.md` (Phase 2 design contract)

Date 2026-11-11. Reviewer R1 (independent). Branch `feat/camera` at `23319fa`. Checked against the code on this
branch (`native/audio/engine`, `native/audio/{mac,win}`, `core/audio`, `core/link.py`, `core/receiver.py`,
`mac_app`, `win_app`, packaging), `dev/audio/PHASE2.md` (A§), `dev/camera/PHASE1.md`, `reports/c1.md`,
`c2.md`, `c3.md`, `proto/`, `STATUS.md`. [C4] placeholders were not reviewed as gaps.

Counts: **1 blocker, 10 major, 14 minor.** Line numbers `P2:n` refer to `dev/camera/PHASE2.md` at `23319fa`.

## Findings

### 1. BLOCKER: a dropped AU silently corrupts the picture (frame numbering is contradictory)

- **Section**: §1 (P2:56-57), §2.4 (P2:200, P2:206), §4.3 (P2:370-371, P2:383-386), §4.5.2.
- **Evidence**: §1 defines the wire `frame` as "+1 per AU **sent** on a stream". §2.4 (frozen contract) says the
  engine numbers AUs "in **on_frame** order" and `acknowledge()` uses that number. §4.3 drops "the oldest queued
  non-IDR AU" when the 8-AU queue overflows. C1 Q3: "each frame is a reference (dropping any single frame ...
  corrupts every later frame)"; C2 Q5: corrupt frames decode with **no error**, so `keep` is the only guard.
- **Failure**: an implementer following §1 numbers the frames after a dropped AU contiguously. The receiver sees
  no frame gap, stays *clean*, and calls `decode(keep=true)` on every following P frame, which references the
  missing AU: corrupt pictures are presented until the refresh lands (C2 measured 14 dB garbage). In addition,
  the wire number and the `on_frame` number now differ by one, so `acked` from the RR is passed to
  `acknowledge()` shifted: the sender can acknowledge the token of the dropped AU (if it was an LTR) or of a
  frame the receiver never decoded, and a later `ForceLTRRefresh` then references a picture the decoder does
  not have, which the receiver also marks clean ("a complete refresh makes it clean").
- **Fix**: one numbering: wire `frame` = `on_frame` number; a missing number on the wire means "frame lost"
  (receiver: broken). Better, never drop encoded AUs: when the queue is full, skip *capture* frames before the
  encoder (as `set_fps(15)` does) so the reference chain stays intact. If an AU is dropped anyway, the sender
  must record a broken range starting at it (never acknowledge an LTR at or after it until its refresh). Add an
  engine test: overflow the queue with the null encoder and assert 0 incorrect kept pictures.

### 2. MAJOR: `sr` shares the media seq space but bypasses the pacer: spurious NACK/PLI, phantom audio loss

- **Section**: §4.2 (P2:351-352, P2:360), §4.4 (P2:393-394), §4.5.1, §2.2 `streams` hunk (P2:121).
- **Evidence**: `sr` is "in the sender direction (it shares the media seq space)", sent "with the first media
  packet" and every 1 s, and "bypass[es]" the pacer. NACK fires "at once when ... a packet arrives with seq >
  highest+1", and a hole > 64 goes straight to PLI. Audio loss today is `expected = seq_hi delta`, `got = rx`
  (`native/audio/engine/streams.cpp:476-481`); old helpers drop unknown ptypes as `n_unknown`
  (`native/audio/engine/flow.cpp:253-274`) without touching the stream's counters.
- **Failure**: (a) at stream start the IDR's 100-225 sealed packets sit in the pacer for ~130 ms; the `sr`
  sealed after them overtakes them, the receiver sees a seq jump of > 64, declares the hole unrepairable and
  sends PLI(loss) while the IDR is still arriving; the sender (no acked LTR yet) asks for another IDR (or hits
  the 2 s limiter). Every 1 s the same happens on a smaller scale (spurious NACKs and retransmissions of packets
  still in the bucket). (b) Audio: each `sr` consumes one audio seq that the receiver does not count as received:
  10 ms Opus frames (`native/audio/engine/codec.h:20`) give a constant ~1 % phantom `loss_pct` on every audio
  stream, including streams to old peers (asusrog), which feeds `dev/audio/e2e/results.py:160`.
- **Fix**: give `sr` its own nonce domain (e.g. a third direction value, or `stream | 0x40000000`) with its own
  seq and window, so it never moves the media `highest`; or send `sr` through the pacer in order. Gate audio
  `sr` on the peer's `video1`/`camera1` feature, and count it in neither `seq_hi` nor loss.

### 3. MAJOR: which thread seals NACK/PLI/RR (and `sr`) is unspecified: ChaCha20-Poly1305 nonce reuse

- **Section**: §3.1 threads (P2:281-284), §4.2 (P2:351-352), §4.5.5-6.
- **Evidence**: `Sealer` is "Not thread-safe: one owner thread" (`native/audio/engine/packet.h:42`); the seq is
  the nonce (`packet.h:2-3`, `make_nonce`). Today the receiver-direction sealer is used only on the network thread
  (`RecvStream::rr_sealer_`, `streams.cpp:332`, `make_rr` from `Flow::tick`). The design adds NACK (gap seen on
  the network thread or the video thread), PLI (deadline expiry on the video thread, decoder error from
  `on_decoded` on the backend thread, 300 ms repeats) and a 50 ms RR timer, all in the same receiver-direction
  seq space; and `sr` (timer) in the sender's media seq space owned by the sender thread.
- **Failure**: two threads sealing with one `Sealer` race on `seq_` and can emit two different plaintexts under
  the same (key, nonce): keystream reuse and Poly1305 key reuse, i.e. a forgery/decryption weakness on the media
  plane, plus one of the two packets rejected as a replay. TSan would catch it only if the test happens to
  race.
- **Fix**: state the owner thread of every sealer: e.g. all receiver-direction packets are built by the video
  thread and sealed on the network thread through a queue (or a mutex-protected `Sealer` with an atomic seq),
  and `sr` is sealed by the sender thread only. Add it to the CW2 acceptance (TSan run with forced PLI/NACK
  storms).

### 4. MAJOR: the source DLL trusts helper-written geometry: a user process can make Frame Server read out of bounds

- **Section**: §4.6.2 (P2:461-468, P2:489-494), §4.6.3.
- **Evidence**: `Slot` carries `w, h, stride` and `data_offset` ("from the start of the section"), all written by
  the helper; the DACL gives `OwnerSid` full access (P2:453), i.e. any unprivileged process of that user. The
  source "copies the newest slot into the pending sample ... and scales" (P2:492-494). `latest` and `mode` are
  also helper-written. The source runs in the shared Frame Server svchost (C2 Q1: `svchost -k Camera`,
  LocalService), which serves every camera on the PC.
- **Failure**: a buggy or malicious same-user process writes `data_offset`/`stride`/`h` past the 12.5 MB view
  or `latest = 7`: Frame Server reads outside the section (crash of every camera on the PC) or copies service
  memory (possibly other cameras' frames) into the user's Beamer Camera picture. That is a user -> service
  boundary.
- **Fix**: in `shm_protocol.h`, drop `data_offset` (derive it from the slot index), and require the source to
  validate every helper-written field before use: `latest < kLiveSlots`, `mode` in range, `w,h` even and
  `<= kMaxW/kMaxH`, `stride >= w`, `stride*h*3/2 <= kSlotBytes`; on failure show the built-in image. Add these
  to CW5's acceptance (fuzz the header from a user process).

### 5. MAJOR: the frozen `video_backend.h` cannot express "show the reconnecting placeholder" after 2 s of hold

- **Section**: §2.4 (P2:212-220, P2:248), §4.5.4 (P2:421-423), §4.6.4 (P2:513-517), §8.4 placeholder texts.
- **Evidence**: the engine owns "hold -> 2 s -> waiting" (§4.5.4, tested in §10.2), but `VideoSinkHandle` only
  has `decode/present/hold/drop`; `vcam_placeholder(Placeholder)` is backend-wide and its text must come from
  Python ("the helper never invents wording"). The `vcam` command carries one placeholder, so while a stream
  runs Python has no defined moment to hand the helper the `reconnecting` text, nor to switch from `starting`
  to `reconnecting`. The header is frozen ("never changed without the operator", P2:1114-1116).
- **Failure**: CW2 and CW4 each invent the hand-off (the engine calls `vcam_placeholder` with an empty text, or
  the backend times 2 s itself and duplicates the engine rule); CW1 does not know it must push the reconnecting
  text while streaming. Result: the 2 s fallback shows the wrong text or no placeholder at all.
- **Fix**: add to the contract before freezing it: `VideoSinkHandle::show_placeholder(const std::string& state)`
  (or `set_mode(Live|Hold|Waiting)`), and let `vcam` carry a map of texts per state
  (`{"starting":{...},"reconnecting":{...},...}`) so the helper can switch states itself.

### 6. MAJOR: PLI `start` repetition rule is contradictory: double IDR at every start; decoder reopen can be refused an IDR

- **Section**: §3.2 `recv` (P2:291) vs §4.2 `kPli` (P2:359); §4.3 rules 1 and 4 (P2:377-382); §4.5.6 (P2:425-426).
- **Evidence**: §3.2: the receiver "sends PLI `start` every 100 ms **until a complete IDR arrives**"; §4.2:
  "every 100 ms **until the first IDR's first packet**". The IDR is paced ~130 ms (§4.4) plus encode 37-56 ms
  (C1), so a second `start` PLI arrives while the first IDR is in flight. Rule 1 answers every `start` with
  `request_idr()`; rule 4's limiter applies only "outside the stream start", which is undefined. After 3
  decoder errors the receiver reopens its decoder and sends PLI `start`; if that is "outside the start", rule 4
  turns it into a refresh, which a fresh decoder (no SPS/PPS, no LTR) cannot decode.
- **Failure**: two 120-256 KB IDRs at each start (the measured cause of keyframe-burst holes that also take audio
  packets, C3 Q2) and +130-270 ms on the start budget; or a decoder-reopen loop (refresh -> decoder error ->
  reopen -> refresh ...).
- **Fix**: one rule: PLI `start` repeats until the first packet of an IDR (frame >= the requested one) arrives;
  the sender ignores `start`/`size` PLIs while an IDR is queued or was sent less than RTT + IDR pace time ago;
  `start` (decoder reopen) and `size` are always answered by an IDR and exempt from the 2 s refresh conversion
  (keep them rate-limited to 1 per 1 s instead).

### 7. MAJOR: `busy` cannot be detected: the source never sees the refused second app

- **Section**: §6.4 rule 6 (P2:725), §8.4 `busy` (P2:925), §5.2 `camera_opened.error` "busy" (P2:641), §11 risk 6.
- **Evidence**: C2 Q6: a second app gets `0xC00D3704` "whether it keeps the type or asks 1080p ... **The source
  never sees the second app**". Nothing in §4.6 (shm fields, `vcam` event) reports a refusal.
- **Failure**: CW1/CW7/CW8 implement a state and a status line that never fire (or CW4 invents a heuristic from
  ConsentStore timestamps that misfires on PCs with a physical camera). Alex's second app shows an error and
  Beamer says "Microsoft Teams on lebig is using it", with no hint about the multi-app toggle.
- **Fix**: drop `busy` as a detected state; instead show the §8.6 multi-app note whenever `multi_app` is false
  and `in_use` is true ("Only one app at a time can use Beamer Camera"), and keep `busy` out of `camera_opened`.
  If a detection is wanted, it is a C2 follow-up measurement first (e.g. a second `ActivateObject` visible to the
  source when sharing is on).

### 8. MAJOR: "Stop Sharing the Camera" is not a kill switch owned by the Mac; the TCC prompt is triggered from the PC

- **Section**: §6.1 (P2:661-663), §8.5 (P2:938-941), §9.1 "When the prompt appears" (P2:962-966).
- **Evidence**: STATUS.md (operator decisions): "Stop Sharing the Camera ... (privacy kill switch **owned by the
  camera's machine**)". The design makes it a write of the cluster-wide route (`set_camera(None, None)`), which
  any machine's page can overwrite with a newer stamp (A§6.2 merge). C1 Q6: "When to ask: **not** when a PC app
  first opens Beamer Camera (the person is probably looking at the PC). Ask from the Mac UI"; the design sends
  `permission_request` as soon as "the route names this Mac ... (from either machine's page)".
- **Failure**: Alex stops sharing on the Mac; someone at the PC re-selects the Mac camera, and the Mac camera
  turns on again at the next app open with no Mac-side action (only the green light). A TCC prompt naming Beamer
  pops up on an unattended Mac because the PC page chose its camera.
- **Fix**: add a Mac-local, Mac-authored flag in its own `camera_devices` (e.g. `capture.allowed: bool`, set only
  by that machine, persisted in `camera.json`): the menu item sets `allowed=false`; the planner treats it as
  `denied` (new state `stopped_on_source` with the text "Sharing stopped on <S>"); only the Mac's page/menu can
  set it back. Send `permission_request` only when the route was set *by* S itself (`route.camera.by == S`),
  else show "Waiting for camera access on <S>" with the Allow button on S.

### 9. MAJOR: dev command files (§7.4) have no owner, and CW11 depends on them

- **Section**: §7.4 (P2:815-819), §2.2 `core/audio/service.py` hunk (P2:128), §12 CW1/CW6/CW11.
- **Evidence**: the dev commands are parsed in `AudioService._dev_command` / `_dev_poll`
  (`core/audio/service.py:1150-1228`, status file `:1167`). `service.py` is "CW6's alone" (P2:1285), but CW6's
  goal/inputs are §2.2, §5, §7.1 only and the service hunk in §2.2 does not list dev commands; CW1 lists §7 as
  input but may not edit `service.py`. CW11 "Depends on ... dev command files" (P2:1263).
- **Failure**: nobody implements `set_camera`, `set_quality`, `camera_test_source` or the `camera` key of
  `audio-dev-status.json`; CW11 starts in wave 3 and cannot drive any case. (`helper_dev` already exists,
  `core/audio/service.py:1224-1226`; §7.4 lists it as new.)
- **Fix**: add §7.4 to the §2.2 service hunk and to CW6's goal and acceptance (a test that each command reaches
  the part / helper), or give the part a `dev_command(dict)` entry point (CW1) that the service hunk calls.

### 10. MAJOR: cameras without a 16:9 1280x720/1920x1080 format overflow the PC side or distort

- **Section**: §4.11 (P2:605-609), §2.4 `CameraInfo.formats` (P2:174), §4.6.2 `kMaxW/kMaxH` (P2:457), §6.3 multi-sink.
- **Evidence**: §4.11: "A camera without the requested size: the nearest larger format, else the largest; the
  actual size goes into the AU (SPS)". C1 Q1: the built-in camera also has 1760x1328, 1552x1552, 1080x1920; C1
  says "do not scale in AVFoundation". STATUS.md operator default: "any AVFoundation camera must work". The
  source DLL offers only 16:9 types and scales nearest-neighbour; slots hold at most 1920x1080. §4.11 shares one
  capture session per device across streams of different sizes (720p to one PC, 1080p to another) with no
  scaler.
- **Failure**: a webcam or Continuity Camera whose best match is 2560x1440 or 1920x1440 sends that size; the PC
  helper decodes a picture larger than a slot (overflow in the helper or a dropped stream), or a 4:3 picture is
  stretched to 16:9 by the source. A second PC at 720p gets 1080p at the 720p bitrate.
- **Fix**: the Mac backend must always deliver exactly the planned size: pick a capture format >= the size with
  the same aspect, then scale/crop with `VTPixelTransferSession` (centre-crop 4:3 to 16:9) per stream; the PC
  helper rejects (decoder error -> `size` PLI) any SPS larger than `kMaxW x kMaxH`. Add a CW3 acceptance with the
  synthetic source at 1920x1440.

### 11. MAJOR: the engine keeps no clock offset today, and link-state feedback (acks) is UDP-only

- **Section**: §4.5.3 (P2:413-416), §4.2 "in link state they go through `link_out` too" (P2:352-354), §2.2
  `flow` hunk (P2:120).
- **Evidence**: the pong handler computes only a 5-sample median RTT (`native/audio/engine/flow.cpp:293-307`); no
  offset is stored, and pings go only over UDP (`Flow::send_ctrl` -> `udp_send_raw`, `flow.cpp:164-171`).
  `Flow::send_rr` sends only by UDP (`flow.cpp:339`). §4.5.3 speaks of "the flow's clock offset from ping/pong:
  lowest-RTT sample of the last 10 s" as if it existed. The §2.2 `flow.*` hunk lists dispatch, RR cadence, the
  pacer and `link_out`, not the offset estimator nor RR/PLI over `link_out`.
- **Failure**: CW2 may miss both; in link state (no UDP for >= 2 s by definition) no RR reaches the sender, so
  no LTR is ever acknowledged and every PLI ends as an IDR over the 1 Mbit/s link (80-190 KB, C1), exactly what
  Deviation 2 wanted to avoid. Also, with Python's 32-message rule (§4.10) applied to every event marked
  `video:true`, RR/PLI of the video stream may be the packets dropped.
- **Fix**: add to the CW2 hunk: offset estimator (min-RTT of the last 10 s, from the existing 24 B pong),
  receiver-direction video packets (RR/PLI) queued to `link_out` in link state in events *not* marked
  `video:true` (never dropped by the 32 rule), and a link-state test that LTR refreshes still happen.

## Minor

12. **Placeholder/backlog/pool sizes ignore the lip-sync hold** (§4.5.4 P2:420-421, §4.6.1 P2:436-437, §4.9
    P2:579-581). Backlog "older than D + 150 ms" is dropped, but with `hold` up to 150 ms the present time is
    capture + D + hold, so at max hold frames are discarded at their own present time. The kept-picture pool of
    6 NV12 buffers holds (D + hold)/33 ms pictures: D = 150 + hold 100 needs ~8. Fix: backlog threshold
    D + hold + 150 ms; pool = ceil((150 + 150)/33) + 2 = 12, or bound `hold` by the pool.

13. **`cam_stop` pass mark contradicts the design's own budget** (§10.5 P2:1068 vs §4.7 P2:555-557). 300 ms debounce
    + 3 s hold + link + <= 500 ms capture stop = up to ~3.85 s, and §4.7 itself says "at most ~4 s"; the pass
    mark is <= 3.5 s. Fix: <= 4.0 s, or measure from S's `in_use:false` receipt.

14. **Heartbeat liveness** (§4.6.2 P2:480, §4.6.4 P2:509). "Helper dead or stuck" is detected only if the beat is
    written by the thread that presents frames; the spec just says "every 100 ms". A beat on its own timer
    thread keeps the last live frame on screen while the engine is wedged, until the Python watchdog (ping 5 s,
    pong timeout 10 s, `core/audio/helper.py:131`) restarts the helper. Fix: beat from the presenter tick (or
    beat only if the presenter ran in the last 200 ms).

15. **Receivers accept `camera_open` from any direct peer** (§6.4 rule 5, P2:720-721; §3.2 `recv`). Nothing says
    the opener must be `route.camera.machine`, nor what happens to an existing video recv stream when a second
    one opens (switching the active camera from one Mac to another, or a stale route during a partition): two
    `VideoSinkHandle`s would feed one virtual camera. Fix: accept only from the route's S, close the previous
    recv stream first, and have `open_sink` fail `busy` while one exists.

16. **LTR acknowledgement relies on the sender having received a PLI for every broken period** (§4.3 P2:383-386,
    §4.2 RR `acked`). If every PLI of a broken period is lost and the receiver becomes clean through a
    sender-initiated refresh (queue overflow, rule 3), the LTRs decoded corrupt in between are `<= acked` and in
    no broken range, so they can be acknowledged and later referenced. Fix: RR carries `clean_since u32` (frame
    that started the current clean run); the sender acknowledges only LTRs in `[clean_since, acked]`.

17. **"Dropping whole frames from the oldest" is impossible in Python** (§4.10 P2:597-598). Video headers are inside
    the AEAD payload and `link_out` events carry up to 4 opaque packets (`flow.cpp:352-356`); already posted
    messages cannot be withdrawn from the link queue. Fix: the engine tags video `link_out` events with the frame
    number(s) and starts each frame in a new event; Python drops the newest frame(s) when over 32 and tells the
    engine (`video_link`), which marks them as a broken range.

18. **DLL update path is unmeasured** (§9.2 "Versioned path", P2:982-985). C2 only showed that Frame Server keeps
    the DLL while a source exists and that replacing it needs the service stopped (`proto/win/scripts/update-dll.ps1`
    stops FrameServer). "Loads the new file at its next activation after it idles" is **unverified**, and with a
    System-lifetime camera the old DLL may stay until reboot; `vcam.install` reads `DllVersion` from HKLM, not the
    loaded source's `Header.dll_version`, so it reports `ok` while the old code runs. Old folders "retried at the
    next install" may never be retried. Fix: report `loaded_version` from the header and say "Restart needed";
    retry folder deletion at every helper start with an unelevated check (or at the next elevated action); add
    a C2-style measurement to CW5.

19. **The source DLL also runs inside the helper** (§4.6.3, §4.6.5). C2 Q1: `MFCreateVirtualCamera` "loads the DLL
    in the calling process once (activate, Initialize, Shutdown) to validate it". In the user's helper, creating
    `Global\` sections fails (error 5, C2 Q4) and logging to `%ProgramData%\Beamer\Camera\source.log` from two
    accounts collides: whichever creates the folder owns it, and a service writing/rotating a log in a folder the
    user can write is a link-redirection target. Fix: the source does nothing at activation outside Frame Server
    (check the process token is LocalService), and `vcam-install` creates `%ProgramData%\Beamer\Camera` with a
    SYSTEM/LocalService/Administrators-only write ACL.

20. **Squatting of `Global\...Demand`** (§4.6.2 P2:452). `InstallGuid` is in HKLM (readable by every user) and any
    user may create a `Global\` event (C2 Q4). Another session's process can create the Demand event first; the
    helper's `CreateEvent` then silently opens it. Fix: create with `CreateEventExW` and fail on
    `ERROR_ALREADY_EXISTS` unless the existing object's owner is the helper's SID; same check by the source on the
    section it reopens.

21. **Uninstall from Beamer's (unelevated, per-user) uninstaller cannot remove the camera** (§9.2 P2:986-988, §3.1
    verbs P2:276-278). `Remove()` is reachable only through the control protocol; there is no unelevated verb, and
    `vcam-uninstall` needs UAC from `PrivilegesRequired=lowest` Inno (`win_app/Beamer-Setup.iss:34`). After an
    uninstall, "Beamer Camera" stays listed in every app showing "Waiting for Beamer" forever, and the DLL stays
    in Program Files. Fix: add `beamer-audio vcam-remove` (unelevated `Remove()`), call it from the uninstaller,
    then offer the elevated cleanup; document the leftover state otherwise.

22. **Bundled DLL location differs between build trees** (§2.5 P2:265-267 vs §9.2 P2:970-972). Dev/source runs put
    the DLL in `native\audio\build\win\` beside the exe (`core/audio/helper.py:85-87` finds the helper there), but
    shipped builds use `camera\BeamerCameraSource.dll`; the PyInstaller portable build puts non-`EXECUTABLE`
    entries under `_internal` (`win_app/build_portable.spec:52-67`). `vcam_install.py` needs one lookup rule; the
    portable layout is **unverified**. Fix: specify the lookup (`<helper dir>\camera\`, then `<helper dir>`) and the
    COLLECT typecode.

23. **`locales/` is a shared generated file with one listed owner** (§12 CW7 P2:1216). `test_i18n_coverage` fails
    when `beamer.pot` is stale (`tools/i18n_extract.py --check`, `core/tests/test_i18n_coverage.py:224-227`); CW8
    (Qt strings), CW9 (`vcam_install.py` messages) and possibly CW1 (placeholder texts, if built outside
    `view.py`) add strings too, and regenerating the `.pot` in several branches always conflicts. Fix: one owner
    regenerates `beamer.pot`/`it.po` after merges; WPs run the check with `--check` tolerated as "pending".

24. **Small statements to align** (no behaviour change, but agents will read them literally):
    - §2.2 lists `sources.txt` "the new engine .cpp files" while §2.5 adds `null_video.cpp` only to the test build;
      `--video null:<opts>` "any helper" (§3.1) needs it in `sources.txt` like `null_backend.cpp`
      (`native/audio/engine/sources.txt`).
    - §2.2 omits `build-tests.{sh,cmd}`, `null_main.cpp` and `golden_control.txt`, which CW2 owns (P2:1142).
    - `MediaContext.new_stream_id` is "shared with audio streams", but `AudioService._new_stream` only excludes its
      own sends/recvs/loop (`core/audio/service.py:496-503`): the hunk must add the camera's stream ids.
    - §4.3 last bullet "the sender discards encoder output" vs §2.4 "no AU is produced before `start_encoding()`".
    - §4.11: `NSCameraReactionEffectGesturesEnabledDefault` and the reactions APIs are macOS 14+ while the helper
      targets 13.0 (`native/audio/build-mac.sh:22`): `@available` guards needed.

25. **Acceptance items needing a person are not flagged as such**: CW3 "`stop` -> capture session stopped <= 500
    ms" needs a real capture session, i.e. camera permission (H1); and C1 Q6 found that a helper started from an
    agent shell is attributed to T3 Code, not Beamer, so H1 needs a deployed Beamer.app (CW9 + `dev/deploy.sh`),
    which CW3 does not list as a dependency.

## Checked and found correct

- Packet facts: header 24 B, `kFlagMarker = 2` is bit1 (`packet.h:15`), ptypes 1-6 used so 7/8/9 are free, codec
  ids 0/1 so 2 is free, `kMaxPayload` 1400, `kReplayWindow` 128 (`packet.h:23`). 12 B video header, 1148 B
  fragment, 1200 B UDP payload, 24 B video RR, 12 B `sr`: sizes add up; `kSlotBytes` = 3,110,400 rounded to
  4096 = 3,112,960; `kMagic` bytes read "BCAM" in memory.
- Retransmitting identical sealed bytes is nonce-safe; the 1024 window covers a 225-packet 1080p IDR + NACK RTT.
- Flow dispatch (`flow.cpp:253-274`): `kMedia` -> recv map, `kRR` -> send map; extending to NACK/PLI (send) and
  `sr` (recv) is consistent. Engine link outbox 64 with drop-oldest (`flow.cpp:155-158`), `link_in` cap 256,
  link flush every 20 ms in batches of 4 (`flow.cpp:352-356`), Python `OUTBOX_LIMIT = 64`
  (`core/audio/service.py:37`, `:1038-1051`), `MAX_FRAME_PACKETS = 4`, `MAX_PACKET_TEXT = 2048` (fits a 1240 B
  packet in base64), link `QUEUE_LIMIT = 2048` (`core/link.py:41`): 1 Mbit/s of video does not starve input.
- Caps: 13 base caps + `audio` = 14 (`mac_app/bridge.py:104-106,516`, `win_app/kvm_bridge_win.py:2052`),
  `MAX_CAPS = 16` (`core/protocol.py:446`); unknown types are sent unless in `_CAP_FOR` (`core/link.py:790`), so
  the `_CAP_FOR` hunk is needed and sufficient on the initiator side; inbound filtering is by `audio_` prefix in
  `core/receiver.py:93,431,1106`, `mac_app/bridge.py:2645`, `win_app/sender.py:1086`, all listed as hunks.
- `audio_hello.features` already carries the helper's ready features (`core/audio/service.py:521-523`,
  `core/audio/wire.py:76-79`), validated as words of <= 32 chars, <= 32 entries (`wire.py:185-188`):
  `camera1`/`video1` fit.
- Deviation 3 matches A§6.4 rule 3/4 and the code (`send` only after `audio_opened`, `service.py:750-773`).
- Run-helper overload by arity does not clash with `backend.h`'s `run_helper(Backend&, int, char**)`; unknown
  argv words only warn (`helper.cpp:918-920`), so `vcam-*` verbs belong in `win/main.cpp` before `run_helper`.
- Windows build: `build-win.cmd` -> `win/build.ps1`, `/std:c++20 /MT`, `$backendSrc` (`build.ps1:19,58`); Mac
  `MAC_BACKEND`, frameworks, `MIN=13.0` (`build-mac.sh:22,29-30,123`); `systemPreferredCamera` is 13.0+.
- Mac packaging: entitlements files and the helper signing with `entitlements-audio.plist`
  (`build_dmg.sh:215-220`), TCC reset list (`Install-Beamer-Mac.command:32`), `setup.py` `py_modules` order
  (`"audio_page"` after `"notch_island"`) and the three non-adjacent hunks (includes ~l.89, py_modules ~l.48,
  plist ~l.108) merge cleanly.
- Windows packaging: per-user Inno `{localappdata}\Beamer` (`Beamer-Setup.iss:27,34`), so the elevated step is
  needed as C2 found; CLSID in HKLM, DLL outside the profile, System/CurrentUser without admin: all as C2 Q1.
- Pages: audio is the 3rd PC page (Ctrl+3, `win_app/pages_win.py`) and the Mac page is flag-gated
  (`mac_app/pages.py:27-30`); the Mac and Qt pages have Microphone -> notices -> Machines in that order
  (`mac_app/audio_page.py:817-849`, `win_app/audio_page_win.py:776-801`), so "after the notices strip" is a
  well-defined insertion point. `Choice`, `Notice`, `Arrow` exist in `core/audio/view.py` (58, 142, 93).
- Phase 1 numbers quoted in §0, §4.4, §4.7, §4.8, §4.10 (IDR sizes, encode/decode times, pacing times, D ~70 ms,
  step-down levels, TCP 1 Mbit/s) match c1/c2/c3; NACK + LTR + no FEC/intra refresh follows C3 Q3; decode in the
  helper, `AVLowLatencyMode`, NV12 only, DACL to the owner SID follow C2.
- Lip-sync numbers: Mac mic -> PC CABLE 172 ms shared / 120 ms low-latency (dev/audio/STATUS.md WP16) support
  the ~20-100 ms hold estimate.
- Not verified (no PC access by rule, no measurement exists): whether `LocalService` reads `%ProgramFiles%` through
  `BUILTIN\Users`, whether `multi_app` is readable, Frame Server unload timing (finding 18), PyInstaller placement
  (finding 22).
