# R2: re-check of `dev/camera/PHASE2.md` (revision D3) before implementation

Date 2026-11-11. Reviewer R2 (independent). Branch `feat/camera` at `1889717`. Checked against the code on this
branch (`native/audio/engine/{packet.h,flow.h,flow.cpp,streams.*,helper.cpp,util.*,sources.txt}`,
`native/audio/{build-mac.sh,win/build.ps1}`, `core/audio/{service.py,wire.py}`, `core/link.py`,
`core/receiver.py`, `core/protocol.py`, `mac_app/{audio_mac.py,bridge.py,setup.py,entitlements*.plist}`,
`win_app/{Beamer-Setup.iss,Install-Maintenance.ps1,Invoke-InstallMaintenance.ps1,Install-Beamer.ps1,build_portable.spec}`),
`dev/audio/PHASE2.md` (A§), `dev/audio/TEST-ALEX.md`, `dev/camera/{PHASE1.md,STATUS.md}`, `reports/c1-c4.md`,
`reviews/r1-phase2.md`, `reviews/r1-resolution.md`, and the prototypes (`proto/e2e/win/camrecv.cpp`,
`proto/win/feeder.cpp`, C4's raw logs `proto/e2e/out/raw/q3_loss720.recv.jsonl`). `P2:n` = line n of
`PHASE2.md` at `1889717`.

Counts: **0 blocker, 6 major, 15 minor.** R1 regressions: none fully regressed; 5 R1 resolutions are only partly
right (#5, #6, #10, #11, #25), see the table.

## Findings

### 1. MAJOR: every real-camera start asks for two IDRs (deferred `start` PLI fires after the first IDR)

- **Section**: §4.3 class 1 (P2:476-479), §4.5.6 (P2:594-596), §4.7 (P2:786-792, P2:803-804), §2.4
  `start_encoding` (P2:248). R1 #6 resolution.
- **Evidence**: the start PLI reaches S ~10-30 ms after `send`, while the camera's own start takes 0.2-0.8 s
  ("runs in parallel with the next row", P2:803-804; C4 Q2: real camera cold start typically 0.2-0.8 s). The
  receiver repeats `start` every 100 ms until an IDR's first packet arrives (P2:594-595). The sender treats a
  `start` as a duplicate only "while an IDR is queued or being paced, or when the last IDR's first packet left
  less than RTT + its pace time + 50 ms ago"; otherwise "at most one per 1 s: a request inside that window is
  served when it ends" (P2:476-479). While the backend waits for the camera's first frame after
  `start_encoding()`, no AU exists, so no IDR is "queued or being paced".
- **Failure**: PLI at t = 0 -> `start_encoding()`; PLIs at 0.1 ... 0.4 s are not duplicates by the text, fall
  inside the 1 s window and are deferred; the camera delivers at 0.5 s, IDR emitted at 0.53 s, T stops its PLIs.
  At t = 1.0 s the deferred request is served: the last IDR's first packet left 0.47 s ago (> RTT + 100 + 50 ms),
  so `request_idr()` runs again: a second 120-256 KB IDR at 4x (another ~100 ms freeze, the burst type C3 linked
  to audio loss). It happens on every start whose camera start exceeds 100 ms, i.e. every real-camera start,
  and violates "exactly one IDR per start" (P2:813, P2:1522). Nothing catches it: §10.2 tests repeated starts
  only "during a paced IDR" (P2:1467), `cam_start` uses the synthetic source (camera start ~5 ms), the real
  camera is only H1.
- **Fix**: in §4.3 class 1, count "IDR requested and not yet emitted (including waiting for the first captured
  frame after `start_encoding()`)" as *queued*; drop the "served when it ends" queue for class 1 (the receiver
  repeats every 100 ms anyway), or re-apply the duplicate test at service time. Add a §10.2 case: fake
  `CameraHandle` whose first AU comes 800 ms after `start_encoding()` -> exactly 1 IDR; give the null backend a
  `cam_start_ms=` option so `two_helpers_video.py` covers it.

### 2. MAJOR: the camera `send` turns on is never turned off when T refuses or never sends its start PLI

- **Section**: Deviation 3 (P2:48-51), §3.2 `send` (P2:360), §4.7 step 2 (P2:786-788), §5.2 `camera_opened`
  (P2:976), §6.4 rules 4-5 (P2:1064-1071), §0 #15 (P2:33).
- **Evidence**: S issues helper `send` (camera starts at once, P2:787) and `camera_open` together. The design
  says what T answers (`ok:false` with `not_installed`/`outdated`/`helper`/`unsupported`/`not_source`) but not
  what S does then; the only rule is "The sender retries when the receiver's list or the route changes"
  (P2:1070-1071). The audio code that CW1 will mirror does **not** stop anything on `audio_opened ok:false`
  (`core/audio/service.py:754-757`: state failed + retry token), because in audio `send` comes after
  `audio_opened`; Deviation 3 reverses that order. The retry token is (route, peer list rev, flow id)
  (`service.py:831-835`). There is no timeout for a `send` that never gets a PLI (C4's `camsend` stopped after
  3 s without a receiver report, c4.md "Method").
- **Failure**: (a) T refuses (helper restarting, install race, or `not_source` because the route set on a third
  machine reached S before T): the Mac's capture session keeps running with nothing encoded, camera light on,
  for as long as T's demand lasts, which breaks "the camera is on only while a PC app uses it" (P2:942). (b)
  `not_source` with T's list and S's route unchanged (T merges the same route later; it does not re-flood it to
  S) never retries: the PC app shows the `waiting` placeholder until the app is reopened. (c) UDP broken while
  the link is up and the flow still `up` on S: no PLI ever arrives, camera on, no error.
- **Fix**: §6.4 rule 4: on `camera_opened ok:false` S sends helper `stop` at once (camera off within 500 ms),
  marks the stream failed and retries on T's list change, route change **or** a 2 s timer with backoff (cap
  10 s). §3.2: a video `send` that has received no PLI 3 s after `send` ends with `stream_state failed`
  (`no_receiver`) and closes the capture. Tests in `test_camera_part` (refusal -> `stop`, retry timer) and §10.2.

### 3. MAJOR: nobody can deliver the placeholder texts: CW1 needs CW7's `placeholder_texts`, CW7 depends on CW1

- **Section**: §4.6.4 (P2:759), §8.2 (P2:1219), §8.4 (P2:1307), §10.1 `test_camera_part` (P2:1445), §12 CW1
  (P2:1613-1626), CW7 (P2:1741-1750), wave plan (P2:1608-1611).
- **Evidence**: the PC helper never invents wording; texts per state come from Python in `vcam placeholders`
  (P2:368, P2:759) and are made by `core/camera/view.py:placeholder_texts` (P2:1219), owned by CW7 (wave 2,
  "Depends on: CW1 milestone 1"). The part (CW1, wave 1, "Depends on: nothing") must push them: CW1's
  acceptance includes "placeholders pushed on every sink status change" (P2:1445-1446). The texts need machine
  labels ("Waiting for alexandres-macbook-air", P2:389) but `MediaContext` (P2:1095-1112) has no way to name a
  machine, and `placeholder_texts(snap, own_label)` takes only the own label. On the PC the Qt page may be
  closed, so the push cannot live in the UI.
- **Failure**: CW1 either invents its own texts (duplicated wording, missing from CW7's i18n review) or ships
  without pushes and fails its acceptance; at merge, two text sources or none. Apps then show the built-in
  "Beamer Camera" title with no detail line for every state (P2:760).
- **Fix**: move `placeholder_texts` into a CW1-owned pure module (e.g. `core/camera/texts.py`, scanned by
  i18n) with a `names: Callable[[str], str]` argument, add `peer_name(peer)` to `MediaContext` (the hosts
  already have it: `mac_app/audio_mac.py:174`), state in §7.1 that the part sends `vcam placeholders` on every
  `snapshot` change of the texts, and let CW7 only review wording.

### 4. MAJOR: the dev synthetic source is still gated on camera permission, so the E2E harness cannot run unattended

- **Section**: §6.4 rule 2 (P2:1055-1057) and rule 8 (P2:1078-1081), §7.4 `camera_test_source` (P2:1187-1189),
  §10.5 (P2:1513-1514), §12 CW6 acceptance (P2:1736-1739), CW11 (P2:1801-1807).
- **Evidence**: rule 2 requires `permission == granted` and a resolvable camera before any `send`; `warm`
  needs the same. `camera_test_source` "sends the synthetic source instead of the camera" but no rule lifts the
  permission gate. Beamer.app has never been granted Camera (STATUS.md:23-28; c4.md:5-6 "`not_determined`";
  C1 Q6: an agent-run helper is attributed to T3 Code, so the grant must go to a deployed Beamer.app, H1).
  CW11 depends only on CW1-CW6 + CW9 and must "run unattended" (P2:1806).
- **Failure**: every §10.5 case (all synthetic) stops at `waiting_permission`; CW11 and CW6's live check either
  block on Alex or each agent invents a bypass (in the helper, the planner or the harness). An unreviewed
  bypass is also a privacy risk if it is not `--dev`-only.
- **Fix**: rule 2: when the own dev test source is set (only with `audio-dev.enabled` and a `--dev` helper),
  permission and camera resolution are replaced by "synthetic source available"; the Mac backend opens the
  synthetic source without touching `AVCaptureDevice`. Add a planner test, and state that release builds ignore
  it.

### 5. MAJOR: the provisional clock offset (stream started in link state) has the wrong sign

- **Section**: §4.5.3 (P2:557-559 definition, P2:552-554 mapping, P2:566-568 provisional seed). R1 #11 resolution.
- **Evidence**: offset = `((t1 - t0) + (t2 - t3)) / 2`, "peer clock minus ours"; encoder output on the receiver
  clock = `sender_ns + enc_delay - offset`. Pong fields match (`flow.cpp:284-307`). Setting the first frame's
  `d` to 20 ms gives `offset = sender_ns + enc_delay + 20 ms - completion`; the text says `completion -
  sender_ns - enc_delay - 20 ms`, the negative. `now_ns()` is `steady_clock` (`util.h:13`): Mac and PC epochs
  are unrelated (uptimes differ by hours or days).
- **Failure**: a stream that starts while the flow is in link state (UDP blocked, exactly the fallback case) maps
  every frame's present time off by twice the clock difference. Either everything is "late" (D pinned at 150,
  stats garbage) or, half the time, every picture is due days ahead: nothing is presented, the pool fills, the
  app sees `starting` then holds forever. Pings are UDP-only (`flow.cpp:400-404`), so no pong replaces the seed
  while the link state lasts. `cam_fallback` drops UDP mid-session (pongs already exist), so no test hits it.
- **Fix**: write `offset = sender_ns + enc_delay + 20 ms - completion`; add a §10.2 case "stream starts in link
  state, no pong" (present times within 25 ms of completion + D) and an E2E variant of `cam_fallback` that opens
  the camera after `drop_udp`.

### 6. MAJOR: 1080p is refused by the stated size check (H.264 codes 1080 as 1088)

- **Section**: §2.4 `size_rejected` comment (P2:262), §4.6.1 (P2:610-613), §4.5.6 `size` (P2:595-596),
  `shm_protocol.h` slot layout (P2:644-645). R1 #10 resolution.
- **Evidence**: H.264 codes 1080p as 1920x1088 with frame cropping. The prototypes read the MF output type as the
  coded size and take the display height from `MF_MT_MINIMUM_DISPLAY_APERTURE`
  (`proto/e2e/win/camrecv.cpp:181-185`, `proto/win/feeder.cpp:217`), and copy the UV plane from the coded offset
  (`feeder.cpp:313`: "the UV plane follows the coded (possibly padded, 1080 -> 1088) luma plane"). PHASE2 refuses
  "an output type ... larger than `kMaxW x kMaxH`" and "SPS larger than kMaxW x kMaxH", and its slot says "UV at
  + stride*h" with `kSlotBytes` sized for exactly 1920x1080.
- **Failure**: an implementation that checks `MF_MT_FRAME_SIZE` or the SPS's coded height rejects every 1080p
  stream: `size_rejected` -> PLI `size` -> IDR -> rejected, at 1 per 1 s forever (§4.3 class 1 has no
  escalation), at 20 Mbit/s bursts. One that accepts it but copies the coded planes writes 1920x1088x1.5 =
  3,133,440 B into a 3,112,960 B slot, or puts UV 8 rows off (the source then shows a corrupt picture or rejects
  it).
- **Fix**: §2.4 and §4.6.1: the check applies to the *display* size (SPS cropping / display aperture); the helper
  writes Y rows 0..h-1 and the UV plane read from the decoder's coded offset (`stride * coded_h`) to the slot's
  `stride * h`. Add a 1080p round trip to the null sink and to CW4's acceptance wording; after N `size` rejections
  of the same size, surface `error` instead of looping.

### 7. MINOR: "sealed by the next tick (<= 2 ms)" is false for an `up` flow; video timers are not in `next_deadline`

- **Section**: §4.2 "Who seals" (P2:438-441), §4.4 (P2:521-522), §2.2 flow hunk (P2:156).
- **Evidence**: `NetLoop::run` sleeps until the earliest flow deadline, at most 10 ms (`flow.cpp:555`);
  `Flow::next_deadline` returns 2 ms only in link mode or before the remote is set, else up to 10 ms
  (`flow.cpp:425-428`). Nothing wakes the loop when another thread posts work. (`flow.h:148` says "2 ms",
  which misleads the same way.)
- **Failure**: PLIs from the video/backend thread and the immediate RR wait up to 10 ms; NACK retries (15 ms
  minimum), the 50 ms video RR and the time-map timer drift unless CW2 adds them to `next_deadline`.
- **Fix**: the flow hunk adds the video timers to `next_deadline` and a wake (socket pair or event) when a
  feedback mailbox is posted; correct "<= 2 ms".

### 8. MINOR: the frozen `video_backend.h` cannot carry fields §3.3 requires

- **Section**: §2.4 `VcamState` (P2:278-286), `EncoderSink::on_camera_event` vocabulary (P2:236-237); §3.3
  `vcam` (P2:379) and `camera` (P2:380).
- **Evidence**: the `vcam` event has `bundled_version`; `VcamState` has no such field, and the portable engine
  cannot include `vcam-win/shm_protocol.h` (CW5's Windows file) to read `kDllVersion`. The `camera` event needs
  `encoder` (standby/warm/cold), `encoder_ms`, `camera_ms`; the header's `what` list has none of them.
- **Fix**: add `int bundled_version` to `VcamState` and `"encoder"` (detail `"standby|warm|cold:<ms>"`) and
  `"camera_ms"` to the `on_camera_event` list before CW2's first commit.

### 9. MINOR: the offered media types are not in the shared contract

- **Section**: §4.6.3 (P2:682-683, P2:717-718), `shm_protocol.h` (P2:618-669).
- **Evidence**: the source offers 1280x720, 1920x1080, 640x360 "(640x480/4:3 only if an app needs it)"; the
  helper must accept `want_*` only if it is "one of the offered types". The list lives in no shared header.
- **Failure**: CW5 adds 640x480 for an app; CW4's helper maps it to 1280x720 and reports a wrong `want`.
- **Fix**: `constexpr` table of offered types in `shm_protocol.h`.

### 10. MINOR: receiver bootstrap is undefined (no time map yet, unseen frames, initial D)

- **Section**: §4.2 time map (P2:447), §4.5.2 hole rule (P2:533-534), §4.5.3 target (P2:569-571).
- **Evidence**: `d` and present times need the stream's `time_map` entry; the first one is sent "in the first tick
  after a covered stream sent its first media packet", by UDP, then every 1 s; if lost, the receiver has no
  mapping for up to 1 s. A hole is lost when "this number got none before its present time", but a never-seen
  frame has no `ts`, so no present time. D before 10 s of samples is not given (C4 used 70 ms until 20
  samples, `camrecv.cpp:484-487`; behind-IDR frames are excluded, so the first samples come late).
- **Fix**: state: until the first time map, present on decode; an unseen frame's present time = the next seen
  frame's minus 33 ms per number (C4's `deadline_unseen`, `camrecv.cpp:589-591`); initial D 70 ms until 20
  samples; send the first time map twice 20 ms apart.

### 11. MINOR: link-state video has unpaced IDRs and a "lost" rule that races NACK repair

- **Section**: §4.3 (P2:461), §4.4 (pacer only on UDP), §4.10 (P2:874-891), §4.5.2 link rule (P2:534).
- **Evidence**: in link state AUs go straight to the video outbox (no pacer). A start/size/recovery IDR (~105
  packets, 27 events) lands in Python's link outbox at once; Deviation 2 itself says one IDR is ~1 s of link time
  at 1 Mbit/s, and the drop rule cuts at 32 messages (P2:885-890). The receiver declares a frame lost "as soon as
  a newer frame completes" in link state, yet §4.10 promises that a packet lost at the UDP -> link transition "is
  repaired normally" (P2:883-884); NACK repairs arrive behind newer frames on the in-order link. Each side
  applies its own flow state, and the two differ for up to `recover_ns` (1 s, `flow.h:37`) at the link -> up
  transition, when TCP-delivered packets arrive after newer UDP ones.
- **Fix**: pace IDRs into the video outbox at `idr_pace_x` x `link.bitrate`; apply the link "newer frame" rule
  only to frames sent entirely after the switch (or let NACK finish first); add a "camera opened while in link
  state" case to `cam_fallback`.

### 12. MINOR: the verified-LTR set is not cleared by an IDR or a new VT session

- **Section**: §4.3 class 2 and acks (P2:480-492), §4.11 (P2:927-929 session re-created on a size change;
  P2:952-953 device switch -> IDR).
- **Evidence**: verified LTRs are the union over all RRs and never expire; an IDR clears every reference on both
  sides, and a re-created session has an empty token map. After a quality change or a recovery IDR, "a verified
  LTR exists" stays true for LTRs the encoder can no longer use, so a loss asks for `request_ltr_refresh()`, which
  the backend turns into an IDR "inside" (P2:250).
- **Failure**: these IDRs bypass the class-2 limit (at most one per 2 s, P2:483-484) because the engine counted a
  refresh: with PLIs every 300 ms, IDR bursts every ~300 ms until a new LTR is verified.
- **Fix**: on every AU with `idr` set, drop verified LTRs older than it; count an AU with `idr` set toward the IDR
  limits whatever was requested.

### 13. MINOR: the Mac backend should itself refuse capture unless Camera is authorized

- **Section**: §9.1 (P2:1353-1360), §6.4 rule 2, §3.3 `permission` (P2:381).
- **Evidence**: the "prompt only from a Mac action" rule is enforced only by Python's planner, from the helper's
  last `permission` event; the helper re-reads only an `undetermined` state. Creating an `AVCaptureDeviceInput`
  while not authorized shows the prompt.
- **Failure**: after `tccutil reset Camera` (which `Install-Beamer-Mac.command` does for ad-hoc builds) or a
  stale `granted`, a PC demand makes `open_camera` show the prompt with no Mac action.
- **Fix**: §2.4/§4.11: `open_camera` checks `authorizationStatus == authorized` itself and fails `permission`,
  then emits a fresh `permission` event.

### 14. MINOR: the heartbeat thread also performs decodes and decoder (re)creation

- **Section**: §3.1 (P2:351-352), §4.6.1 (P2:609-611), §4.6.4 (P2:728-731).
- **Evidence**: the per-stream video thread does "decode calls, presenter timer, heartbeat"; the decoder is
  re-created after 3 errors, on `DXGI_ERROR_DEVICE_REMOVED`, and when it retries hardware every 30 s.
- **Failure**: an MFT/D3D11 re-creation that blocks for > 500 ms makes the DLL show "Waiting for Beamer"
  during a healthy stream (a false "helper stuck").
- **Fix**: create or re-create the decoder off the presenter thread, and keep presenting/holding while it
  happens; CW4 acceptance: device removal shows no built-in image.

### 15. MINOR: the service's public camera API has no owner in §2.2

- **Section**: §7.1 (P2:1137-1139), §2.2 service hunk (P2:165), CW6 (P2:1724-1739).
- **Evidence**: §7.1 says the service gains `set_camera`, `set_quality`, `set_camera_sharing`,
  `ask_camera_permission`, `camera_snapshot()` and camera notifications through `subscribe`; neither the
  `service.py` hunk nor CW6's goal/acceptance lists them; CW7/CW8 test against a "fake service" (P2:1755,
  P2:1767).
- **Fix**: add them to the §2.2 hunk and to CW6's acceptance (each setter reaches the part on the worker).

### 16. MINOR: "`beamer-audio.exe` already has the installer's rules" is false

- **Section**: §2.3 (P2:190-193), Deviation 4 (P2:52-55), §9.2 (P2:1408-1410).
- **Evidence**: the installer creates rules only for `Beamer.exe` on 24820/24821
  (`win_app/Install-Maintenance.ps1:66-86`); audio added none (A§ `dev/audio/PHASE2.md:28, :758`). The
  allowance for `beamer-audio.exe` is the rule Alex accepted at a prompt (`dev/audio/TEST-ALEX.md:24`).
- **Consequence**: keeping video in the helper is still right, but a fresh install, the portable build and the
  dev tree (`native\audio\build\win\`) will prompt once. Reword, and say so in the H6 install check.

### 17. MINOR: `Install-Beamer.ps1` has no uninstall path

- **Section**: §9.2 (P2:1404) "`Install-Beamer.ps1`'s uninstall path does the same".
- **Evidence**: the script has no parameters and no removal branch (`win_app/Install-Beamer.ps1`, 120 lines;
  it only replaces files and startup entries).
- **Fix**: drop the sentence, or give CW9 a `-Uninstall` switch to add.

### 18. MINOR: the binding operator decisions still say 250 ms

- **Section**: Deviation 1 (P2:38-44), §13 (P2:1859-1861).
- **Evidence**: `STATUS.md:37` ("Phase 2 design decisions (operator)") still says "the 250 ms placeholder only
  when nothing feeds the camera"; the C4 rulings PHASE2 relies on (500 ms, late frames shown, warm encoder,
  floor as a dev key only, P2:13-14) are only listed as D3's scope (STATUS.md table).
- **Fix**: record the C4 rulings in the operator decisions before agents start (agents read STATUS.md too).

### 19. MINOR: `cam_lipsync`'s "flash + beep" test source has no owner and no definition

- **Section**: §10.5 (P2:1531), CW10 (P2:1790-1799), CW3 synthetic source (P2:1656-1657).
- **Evidence**: the synthetic camera is C1's code strip; the audio test signals are chirp signatures, sines
  and noise (`native/audio/engine/test_signal.h:14-15`). No WP builds a flash synchronised with a beep, nor how
  the PC records the virtual mic and the camera on one clock.
- **Fix**: define it (e.g. the strip's Mac time and the audio signature's sample index, both mapped through
  the time map) and assign it to CW10 with a CW3 hook.

### 20. MINOR: human items not flagged

- **Section**: CW5 acceptance (P2:1721-1722), §10.4/§10.5.
- **Evidence**: CW5 must measure the DLL swap "after ... reboot"; after a reboot, Alex's session (needed for
  every in-session consumer, `proto/win/scripts/in-session.ps1`) exists only after he logs in. Likewise every
  §10.5 case needs Alex's session logged in. And see finding 4 (camera permission).
- **Fix**: mark the reboot step and the "Alex logged in" precondition as human items (H8).

### 21. MINOR: two interfaces left for agents to agree on

- **Section**: §0 #2 (P2:21), §3.2 `vcam` (P2:368), §4.6.5 (P2:764-769), §9.2 (P2:1383); CW8/CW9
  (P2:1765-1767, P2:1773).
- **Evidence**: the camera is created "at every start" by the helper (P2:21, P2:764) **and** by `vcam enable`
  (P2:368); after an install the running helper must notice, but who triggers it (the part's `vcam
  enable:true`, the `vcam` status query, or a re-scan) is not said. `win_app/vcam_install.py`'s API is not
  specified, yet CW8 codes against it in parallel ("mock until it lands").
- **Fix**: one rule (the helper creates at `ready` and on every `vcam` command when `install == ok`; Python
  sends `vcam {}` after an install), and a three-line API for `vcam_install` (`install|update|repair|remove(on_done)`,
  result codes incl. 1223).

## R1 resolutions re-checked

| R1 # | Result | Note |
|---|---|---|
| 1 | holds | one numbering in `on_frame` (P2:70-74, P2:235, P2:458); backpressure + visible holes (P2:462-471); clean needs f-1 clean (P2:539-542); late frames never relax `keep` (P2:582-583). Unseen-frame present time undefined: finding 10 |
| 2 | holds | flow-level `time_map` on the control channel (P2:428-430, P2:447); gated on `sr`/`video1`, and `video1` comes from the helper's ready features (`service.py:523`), so old peers never get one (they would count it in `n_rejected`, `flow.cpp:239-242`) |
| 3 | holds | network thread seals all receiver-direction packets and the time map; sender thread owns the media sealer; pacer never seals (P2:349-354, P2:437-441, P2:523). Tick latency: finding 7 |
| 4 | holds | no `data_offset`; constant geometry, read-once, range checks, private copy, `rejected` (P2:640-666, P2:701-715); `sizeof(Header)` = 448 B <= 4096 |
| 5 | partially | contract fixed (`show_placeholder`, `vcam_placeholders`, P2:273-275, P2:309), but who delivers the texts is circular (finding 3) |
| 6 | partially | one start rule and the class-1 duplicate test exist (P2:476-479, P2:594-596), but the deferred request makes a second IDR whenever the camera starts slower than 100 ms (finding 1) |
| 7 | holds | `busy` and `multi_app` gone (only P2:59, P2:1340 mention it as removed); static help line (P2:1336-1340) |
| 8 | holds | Mac-local `sharing`, accepted only from its own list (P2:1003-1007, P2:984-985, P2:1171); the planner never opens while off (P2:1055); prompt only from `set_camera(own)` / `ask_permission` (P2:1129-1134, P2:1353-1360). Backend-side guard: finding 13 |
| 9 | holds | `part.dev_command`/`dev_status` (P2:1126-1127, P2:1181-1191), CW6 acceptance (P2:1734-1735), CW11 depends on CW6 + CW1 (P2:1805) |
| 10 | partially | cluster size always, crop rule and worked examples correct (P2:896-913, checked below); the added "refuse SPS above kMaxW x kMaxH" rejects 1080p as written (finding 6) |
| 11 | partially | offset estimator and link-state feedback present (P2:432-435, P2:557-568, P2:878-880); the provisional seed added for the link-state start has the wrong sign (finding 5) |
| 12 | holds | backlog D + hold + 150 ms (P2:587), pool 12 (P2:612-615) |
| 13 | holds | `cam_stop` <= 4.0 s (P2:1527), budget P2:815-819 |
| 14 | holds | beat from the presenting thread (P2:661, P2:728-731); decode on that thread: finding 14 |
| 15 | holds | accept only from the route's S, close the old recv first (P2:1067-1071); `open_sink` fails while a sink exists (P2:311) |
| 16 | holds | 28 B RR with `clean_since`/`acked` (P2:448); verified LTRs only inside reported clean runs (P2:485-492); C4 used the same rule (c4.md Changes 3). Verified set never cleared by an IDR: finding 12 |
| 17 | holds | frame-tagged `link_out` (P2:384, P2:875-877), whole-frame drop + `video_link dropped` (P2:885-889) |
| 18 | holds | `loaded_version` from `Header.dll_version` (P2:281, P2:657, P2:1387-1390) |
| 19 | holds | inert outside LocalService (P2:688-691); ProgramData ACL by `vcam-install` (P2:1380) |
| 20 | holds | ownership checks on both sides (P2:693-699) |
| 21 | holds | `vcam-remove` + `[UninstallRun]` before the existing maintenance entry (`Beamer-Setup.iss:79-81`) + `Remove-BeamerCamera` (P2:1399-1404). Stray claim about `Install-Beamer.ps1`: finding 17 |
| 22 | holds | one lookup rule (P2:1370-1374); portable placement still unverified, in CW9's acceptance (P2:1784-1785) |
| 23 | holds | merge step owns `locales/` (P2:170, P2:1601-1603); `core/camera` added to both i18n `SOURCES` (P2:169) |
| 24 | holds | all five items fixed (P2:159-160, P2:165, P2:496-497, P2:326-328) |
| 25 | partially | H1-H7 listed and WPs list human items (P2:1534-1538, P2:1605-1606); new gaps: E2E needs camera permission (finding 4), CW5's reboot and Alex's login (finding 20) |

## Checked and found correct

- **Sizes and layouts**: video header 4+2+2+1+1+2 = 12 B; 24 B header + 12 + 1148 + 16 B tag = 1200 B UDP payload
  (`packet.h:18-21`); RR 4+2+4+4+2+4+4+2+1+1 = 28 B; NACK max 1 + 32 x 10 = 321 B; time map 1 + 32 x 16 B; PLI 6 B;
  1080p IDR 256 KB / 1148 = 228 fragments (< 1024); `ReplayWindowN<1024>` = 16 x u64. `kSlotBytes` 3,110,400
  rounded to 4096 = 3,112,960; `Slot` packs to 64 B; `Header` 448 B; `kMagic` bytes "BCAM". Ptypes 7/8/9 and
  codec 2 are free (`packet.h:14-16`).
- **Existing code the design relies on**: pong carries `t0` / peer receive / peer send (`flow.cpp:284-307`), so
  the offset formula and its sign convention are right (only the provisional seed is inverted); ping period 1 s
  (`flow.h:35`); pings and pongs UDP-only (`flow.cpp:164-171, 400-404`); `peer_link_ns` 250 ms (`flow.h:39`);
  `send_media` duplicates to the link in link state (`flow.cpp:151-161`); link outbox 64 drop-oldest and 4
  packets per event (`flow.cpp:155-158, 352-356`); stream-0 packets other than hello/ping/pong/bye are rejected
  (`flow.cpp:238-242`, hence the hunk); audio RR W3 rule (`flow.cpp:406-414`); `Sealer` single-owner
  (`packet.h:42`); `OUTBOX_LIMIT` 64 and `MAX_FRAME_PACKETS` 4 (`service.py:37, :1038-1051`, `wire.py:41`);
  `helper_dev` reaches the helper (`service.py:1224-1226`); `_new_stream` excludes only audio ids
  (`service.py:496-503`, hence the hunk); `audio_hello.features` = the helper's ready features
  (`service.py:523`); `LINK_VERSION` 6 and `MAX_CAPS` 16 (`protocol.py:50, :446`); `_CAP_FOR` and `audio_`
  prefix filters (`link.py:53-57`, `receiver.py:91-93`, `sender.py:1086`); hosts already implement `post`,
  `outbox_depth` (`audio_mac.py:141-158`); `MIN=13.0`, AVFoundation already linked, `MAC_BACKEND`
  (`build-mac.sh:22,29-30,123`); `$backendSrc`, `/std:c++20 /MT` (`win/build.ps1:19,58`); `null_backend.cpp` in
  `sources.txt`; Inno per-user `{localappdata}\Beamer`, `[UninstallRun]` -> elevated maintenance
  (`Beamer-Setup.iss:27,34,79-81`, `Invoke-InstallMaintenance.ps1:25-30`). Every file and tool PHASE2 names as
  input exists (`proto/e2e/*`, `proto/win/*`, `proto/mac/src/*`, `dev/audio/e2e/{beamer,clock}.py`,
  `mac_app/tests/design_offscreen.py`).
- **Timers and thresholds are consistent everywhere**: 500 ms "nothing feeds" (§0 #5, Deviation 1, §4.6.3
  `beat_qpc` window, §4.6.4, §5.3, `cam_helper_kill`, CW4/CW5), 2 s hold before `reconnecting` (§0 #5, §4.5.4,
  §4.6.4, `cam_stall`, CW4), 500 ms hold after a sink closes (§4.6.4, CW4), `in_use` off debounce 300 ms (§2.4,
  §3.3, §4.7, CW4), 3 s demand hold (§4.7, §6.3, CW1), PLI loss repeats 300 ms, start repeats 100 ms. IDR at 4x on
  an absolute schedule in §0 #4, §3.2, §4.3, §4.4, §10.2, CW2, §11 and PHASE1; dedup windows (1 s start/size, 2 s
  recovery) match between §0 #4, §4.3 and §10.2. Placeholder states match between §2.4, §3.2, §6.4 rule 7 and
  §8.4.
- **C4 numbers quoted** in §0, §4.3, §4.4, §4.5.3, §4.7, §4.8, §4.9, §10.5 and §11 match `c4.md` (latency
  table, start path 260/308 ms, stop 24-33/51 ms, recovery 0/52, 63/137, 300/336, 1566/1603 ms, 11 refreshes,
  0 loss in 300 IDRs, drift 10.5 ppm / 18 ms, CPU and memory, lip-sync offsets). The `cam_stall` pass mark is
  derived correctly (drop + 66 / + 103 ms).
- **Crop rule examples** recomputed: 1760x1328 -> 1760x990 at y 168; 1552x1552 -> 1552x872 at y 340; 1080x1920 ->
  1080x606 at y 656; 640x480 -> 640x360 at y 60; 1920x1440 -> 1920x1080 at y 180.
- **Lip sync**: `video_present_delay` (enc_delay + D) and `audio_present_delay` (mapped with the offset) both
  carry the same offset error e (D absorbs e through `d`; when D is clamped at the floor the present instant
  moves by -e instead), so the difference is offset-independent, as §4.9 claims.
- **Clean-run / ack logic**: the receiver never keeps a frame whose predecessor is not clean, late frames never
  relax it, acks advance only over `on_decoded(ok)` frames, and the sender acknowledges only LTRs inside
  reported clean runs; lost RRs/PLIs only delay acks. Same rules C4 ran with 0 corrupt frames in 7,220.
- **Warm encoder vs privacy**: the standby session has no camera and never encodes (§2.4, §4.11 tables); `warm`
  needs sharing on and permission granted (rule 8); the camera starts only with a `send`, which needs demand.
  The kill switch cannot be overridden from a PC: the Mac's own list is never replaced by a received one (same
  rule as `service.py`'s own-machine branch of `_on_audio_devices`), and the planner reads the local `sharing`.
- **Shared memory**: constant geometry, read-once validation, seqlock + private copy, heartbeat from the
  presenting thread, squatting checks on both sides, minimum access rights; the helper validates source fields
  too.
- **Work packages**: no new-file ownership overlap; the multi-owner files (`mac_app/setup.py`, the vcam-win
  README, `dev/camera/e2e/` for CW10/CW11) are named with their split; wave order is consistent except finding 3.

## Unverified

- Whether `LocalService` reads `%ProgramFiles%\Beamer\Camera` through the inherited `Users` ACE (C2 used
  ProgramData); Frame Server's unload timing (CW5 measures); PyInstaller placing an `EXECUTABLE` entry under
  `camera\` (CW9 checks).
- How VideoToolbox treats acknowledged tokens from before an IDR or from a previous session (finding 12 assumes it
  ignores them and falls back to an IDR).
- Whether the "NACK only while that frame's present time has not passed" cut-off (P2:445) keeps C4's 30-packet
  result (4 of 5 repaired by NACK and shown late: `q3_loss720.recv.jsonl` has only 2 `deadline_unseen` losses).
  C4's receiver gave up on the same deadline but kept NACKing (`camrecv.cpp:572-593, 824-836`); CW2's 30-packet
  case in `two_helpers_video.py` should confirm it.
- Real-camera terms (start time, capture delay): finding 1 depends only on the camera start exceeding 100 ms,
  which every assumed value does.
