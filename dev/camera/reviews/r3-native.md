# R3: native camera code review (CW2 engine, CW3 Mac backend, CW4 Windows sink, CW5 source DLL)

Reviewer: R3 (independent). Branch `feat/camera`, head `67ac12e`. Read-only review; two scratch programs were
built from a private `git archive HEAD` export under `$TMPDIR` (nothing in the shared tree, nothing on the PC).

## What was reviewed

`git log --oneline b7c5852..HEAD -- native/`:

```
434437d CW2: decoder re-creation asks for an IDR, link-state provisional offset, fine timers, two_helpers_video.py, MSVC engine_tests
dbeff7d CW9b: real install, removal and update on lebig; vcam_install log fix; dev-install README
0ff0bfd CW5: report and acceptance drivers
b8a526c CW4: Windows video sink in the helper (MF decode, display size, shared memory, vcam verbs), video_check
e8693d1 CW3: the Mac helper runs the macOS video backend beside the audio backend
c6b5938 CW3: keep the camera's chosen format across startRunning; per-frame bitrate-step metric
8444f79 CW2: engine video transport (packets, sender, receiver, pacer, time map, null video, helper commands), tests
94ac57c CW3: encoder wake until the first frame, camera_ms from capture start, synthetic picture, idle-cost probe
9001006 CW5: vcam-win README; feeder: open on demand, loadtest stop/shutdown, open-DACL squat
0082992 CW3: macOS video backend (capture, crop/scale, VideoToolbox LTR encoder, standby, synthetic source), video_check
dffd9a0 CW5: Beamer Camera source DLL, built-in image, test tools
5adb0d8 CW5: shm_protocol.h contract (PHASE2 §4.6.2, verbatim)
e0a4ef5 CW2: video_backend.h contract (PHASE2 §2.4, verbatim)
```

`git diff b7c5852..HEAD --stat -- native/` gives 77 files and +18979/-94 lines. I read these line by line:
- **Engine:** `video_backend.h`, `video_packet.{h,cpp}`, `video_recv.{h,cpp}`, `video_send.{h,cpp}`,
  `pacer.{h,cpp}`, `sync.{h,cpp}`, and the `packet.{h,cpp}`, `streams.{h,cpp}`, `flow.{h,cpp}` and
  `helper.cpp` hunks.
- **Windows helper:** `video_backend_win.{h,cpp}`, `videodec_win.cpp`, `vcam_win.cpp`, `vcam_install_win.cpp`,
  and the size and buffer handling in `placeholder_win.cpp` and `app_usage_win.cpp`.
- **Source DLL:** `vcam-win/shm_protocol.h`, `source.cpp`, `builtin_image.h`.
- **Mac backend** (scope extended by the operator): `video_backend_mac.{h,mm}`, `videoenc_mac.mm`,
  `camera_mac.mm`.

I skimmed `null_video.cpp`, the `video_tests.cpp` cases that touch the findings, and `scale_mac.mm`. I did not
review the harnesses (`video_check.*`, `two_helpers_video.py`), the vcam-win tools (`feeder`, `shmfuzz`,
`camprobe`, `*.ps1`), or `synth_mac.mm`.

Severity counts: **1 critical, 1 major, 11 minor.**

---

## Findings

### 1. CRITICAL: `%ProgramData%\Beamer\Camera` is trusted without an ownership check. The LocalService log follows reparse points, and the elevated uninstall deletes through junctions.

*Status: plausible, not exercised on the PC. It rests on the documented default ACL of `C:\ProgramData`
(`BUILTIN\Users:(CI)(WD,AD)` and `CREATOR OWNER:(OI)(CI)(IO)F`) and on the code below.*

**Where:**
- `native/audio/win/vcam_install_win.cpp:161-187` (`make_data_dir`).
- `native/audio/win/vcam_install_win.cpp:107-125, 277-278` (`delete_tree` in `vcam_uninstall`).
- `native/audio/vcam-win/source.cpp:126-140` (`Log::line`).

**Failure scenario:**
1. Any local standard user, including malware running in Alex's own unelevated session, creates
   `C:\ProgramData\Beamer` (or `...\Beamer\Camera`) before the camera is installed. ProgramData lets Users
   create folders, so the attacker becomes the folder's owner.
2. `make_data_dir` calls `SHCreateDirectoryExW` on the parent and `CreateDirectoryW`. It accepts
   `ERROR_ALREADY_EXISTS`, then replaces only the **DACL** of `...\Camera` with `SetNamedSecurityInfoW`. It never
   checks or sets the **owner**, and it never touches the parent.
3. The attacker keeps control in either of two ways:
   - As owner of `Camera`, they keep `WRITE_DAC` and can grant themselves full access again.
   - As owner of `Beamer`, they have `FILE_DELETE_CHILD` through the `CREATOR OWNER` ACE, so they can rename
     `Camera` away and put a junction in its place, for example to an object-manager symlink.
4. The source DLL in Frame Server (LocalService) then:
   - opens `...\Camera\source.log` with `CreateFileW(OPEN_ALWAYS)`, with no `FILE_FLAG_OPEN_REPARSE_POINT` and
     no final-path check;
   - rotates it with `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`.

   Together these give the attacker an arbitrary-file create/append primitive and an arbitrary move/overwrite
   primitive as LocalService. LocalService holds `SeImpersonatePrivilege`, the usual first step to SYSTEM.
5. Second consequence: when the user later clicks "Remove…", the **elevated** `vcam-uninstall` runs
   `delete_tree(program_data_root)`. That function recurses into any entry with `FILE_ATTRIBUTE_DIRECTORY`,
   including junctions (`FILE_ATTRIBUTE_REPARSE_POINT` is not tested). It therefore deletes, as Administrator,
   the contents of whatever directory the attacker pointed the junction at.

**Fix:**
- In `vcam-install`:
  - Create `%ProgramData%\Beamer` and `...\Camera` with an explicit owner (`BA` or `SY`, set through
    `OWNER_SECURITY_INFORMATION`) and a protected DACL.
  - If either folder already exists with a different owner, or is a reparse point, move it aside or delete it
    (without following links) and create it again.
- In `delete_tree`: never recurse into `FILE_ATTRIBUTE_REPARSE_POINT` entries. Remove the link itself with
  `RemoveDirectoryW`.
- In the DLL:
  - Open the log with `FILE_FLAG_OPEN_REPARSE_POINT`, check that it is a plain file whose
    `GetFinalPathNameByHandleW` is under the expected folder, and rotate by truncating the open handle instead
    of `MoveFileExW`.
  - Alternatively, log under LocalService's own profile (`%SystemRoot%\ServiceProfiles\LocalService\...`),
    where no user can create anything.

### 2. MAJOR: a new decoder that already holds the backlogged IDR still triggers a second IDR, and the hardware retry is not seamless.

*Status: confirmed by code reading and by a scratch test against `VideoRecvCore` and `SenderRecovery`.*

**Where:**
- `native/audio/engine/video_recv.cpp:460-469`.
- `native/audio/win/video_backend_win.cpp:183-222, 332-344`.

**What happens:**
- The Windows sink keeps an IDR (and the AUs behind it) that arrives while its decoder is being created, and
  decodes it when the decoder is ready (`process` → `backlog_`, then `adopt_pending` → `flush_backlog(true)`).
- `adopt_pending` emits `decoder_hw`/`decoder_sw` *before* it flushes the backlog.
- `VideoRecvCore::on_sink_event` treats `started_` (meaning "an IDR was handed") as proof that the AUs went to a
  missing decoder. It then revokes every pending picture and calls `need_idr(kPliStart)`.
- The flushed IDR's `on_decoded(ok)` then finds the frame revoked, so the picture is never shown.
- The new start PLI arrives at the sender inside the coalescing window or the 1 s class-1 window. It is
  ignored, and the stream waits ~1 s for a second IDR.

**Scratch reproduction** (`r3_dec.cpp`, `r3_rec.cpp`, engine objects of `HEAD`):
- IDR handed at +120 ms, `decoder_hw` at +170 ms: 1 new PLI, `started` reset to 0, 0 pictures presented.
- `SenderRecovery` with the IDR's first packet at +31 ms and 100 ms of pacing: start PLIs at +200…+1000 ms are
  all ignored (`kNone`); the IDR is requested at **+1100 ms**.

**When it happens:**
1. **At every start** where the hardware decoder (33-100 ms, but CW4 saw 0.4-1.7 s stalls under GPU load) is
   slower than the first IDR. The first picture then comes ~1.1 s late, against the targets "exactly one IDR
   per start" and p50 ≤ 450 ms. CW4's own measurement says "the first IDR is kept meanwhile (`backlogged` 1)",
   so this ordering occurs in practice. CW4 measured with a stand-in engine, not with this one.
2. **After every 3-error reopen and every `device_lost`** when the IDR beats the new decoder: two IDRs and a
   ~1 s freeze.
3. **Always, 30 s after a device loss.** The hardware-retry standby decoder emits `decoder_hw` while software is
   still decoding (`video_backend_win.cpp:208-213`). The engine revokes the ready pictures and stops handing
   frames until a new IDR. The "seamless" swap is therefore a forced IDR plus a visible freeze.

**Fix (pick one):**
- In the engine, only re-ask when the sink actually **discarded** AUs. Have the sink send a distinct event
  (e.g. `decoder_needs_idr`) from `decode_one`'s discard branch, or count `on_decoded(false)` with ms == 0 as
  discards. Ignore `decoder_*` when the new decoder received the backlogged IDR.
- In the sink, emit `decoder_*` after `flush_backlog`, with a detail telling whether an IDR was in the backlog.
  Never emit `decoder_hw` for the standby decoder (or emit `decoder_hw_standby`, which the engine ignores).

A test should cover "IDR handed, then `decoder_hw`" expecting no PLI. The existing case
`video_tests.cpp:784-805` asserts the opposite.

### 3. MINOR: the Mac handle destructor races `follow_preferred` / `resolve_lost`, which can leave a dangling listener in a shared camera source.

*Status: plausible, unverified.*

**Where:** `native/audio/mac/video_backend_mac.mm:605-636` (`~MacCameraHandle`), `532-555`, `557-578`, `718-733`.

**Failure scenario:**
- `~MacCameraHandle` removes itself from its source first, but erases itself from the backend's `handles_` only
  at the end (`handle_closed`).
- If the preferred camera changes during that window, `follow_preferred` on `q_` still finds `h` in `handles_`
  and calls `h->switch_source(next)`, which runs `next->add(this)` on the object being destroyed.
- When `next` is also used by another stream (the source is shared per device, `sources_`), it keeps the freed
  `this` in `listeners_`. The next captured frame calls `on_captured` on freed memory and the helper crashes.

**Fix:** call `be_->handle_closed(this)` (erase from `handles_` under the backend `mu_`) at the **start** of the
destructor, before `src->remove(this)`.

### 4. MINOR: `CameraSource` blocks use a raw `self` after a racy `alive` check, and the destructor does not drain `ctl_q_`.

*Status: plausible, unverified.*

**Where:** `native/audio/mac/camera_mac.mm:325-337` (start block), `347-363` (`retry_start`), `435-462` (destructor).

**Failure scenario:** `open_camera` builds the source and then fails afterwards, for example
`Encoder::create` fails, or a stream is stopped right away. `~CameraSource` sets `alive=false` and returns
without waiting for `ctl_q_`. The pending start block is still inside `startRunning` (0.2-0.8 s). When it
resumes, it reads `self->key_` in the "session changed the format" log line without checking `alive`, and it
calls `self->retry_start()` after a check-then-use of `alive`. Both touch freed memory.

**Fix:** capture copies (`key`, `s`) in the blocks instead of `self`, or keep the source alive from the blocks
with a `shared_ptr`/`weak_ptr` (`weak_from_this`).

### 5. MINOR: the Mac handle API does not guard re-entrant `set_format` or destruction from inside `on_frame`.

*Status: latent; no current engine path does this.*

**Where:** `native/audio/mac/video_backend_mac.mm:44-46` (comment), `638-676`, `605-636`.

**What I checked (operator check 1):** CW2's engine never calls `set_format`, never destroys the handle, and
never makes any `CameraHandle` call from inside `EncoderSink::on_frame` or `on_camera_event`:
- `VideoSendStream::on_frame` takes only `mu_` and `pol_mu_`.
- `set_format` is reached only through the sender thread (`handle_mail` 0xF0, from `video_config`).
- PLI handling (`act`) runs on the sender thread.
- The handle is destroyed only in `fail()` (sender thread: `no_receiver`) and `stop()` (helper thread after the
  sender thread was joined).
- `camera_lost` is posted to the helper thread.
- None of these threads holds `mu_` or `pol_mu_` while it calls the handle, so the output thread blocked in
  `on_frame` can always finish. The destructor waits for it under `sink_mu_`, and `set_format` waits for it in
  `flush()`.

**The latent problem:** the header comment says "the engine may call any of them from inside on_frame". That
is false for two calls, and nothing enforces the rule:
- `set_format` calls `VTCompressionSessionCompleteFrames` while the output callback is on the stack, which
  deadlocks.
- The destructor passes the recursive `sink_mu_` and then invalidates the session from inside its own callback.

**Fix:**
- Correct the comment.
- Add a thread-local "inside sink callback" flag set in `on_encoded`/`emit`. While it is set, `set_format` and
  the destructor should defer the work to `timer_q_` (or log and fail).

### 6. MINOR: the encoder wake can loop forever when its buffer cannot be created, and it holds the handle lock during the slow first encode.

*Status: confirmed by code reading. The rest of the wake checks out (operator check 2, below).*

**Where:** `native/audio/mac/video_backend_mac.mm:194-213`.

**Failure scenarios:**
- `woken_` is incremented only when `wake_buf_` exists. If `CVPixelBufferCreate` fails, every wake reschedules
  another one 200 ms later. That continues for the handle's whole life (until a real frame is submitted or the
  handle dies), so a stuck camera with a live receiver keeps a 5 Hz timer forever.
- `wake()` holds `mu_` (recursive) across `enc_->encode`, which takes ~40 ms on an idle encoder. During that time
  `on_captured` blocks, and so does every engine call that needs `mu_` (`set_input_paused`, `request_idr`, …).
  The engine's sender thread holds its `cam_mu_` while making those calls.

**Fix:** count every attempt (`woken_++` unconditionally), and drop `mu_` around the encode (take what is needed
under the lock, then encode outside it with a "wake in flight" flag).

**Operator check 2, the rest of the wake:**
- **No leak.** Each wake frame carries `FrameCtx.discard=true` and `output_cb` returns before `d->out`
  (`videoenc_mac.mm:75`). The flag travels per frame, so a concurrent real frame cannot be dropped and a wake
  frame cannot leak. The first real frame is `ForceKeyFrame` (`want_idr_` from `start_encoding`), so nothing
  references the discarded frames, and LTR tokens of wake frames are never seen by the engine.
- **It stops in every case:**
  - on the first submitted real frame;
  - after 20 wakes (4 s);
  - when the handle is destroyed: `alive=false`, and pending `dispatch_after` blocks check the captured `alive`
    without touching `self`;
  - on a permission failure: no handle is ever created;
  - on a camera error or interruption: no real frame, so the 20-wake bound applies, and the engine's 3 s / 5 s
    guard closes the handle anyway.
- **Timestamps** of the wake frames lie 2 s before `open_ns_`, so they stay below any later capture time.

### 7. MINOR: the helper maps `Global\…Shm` without checking who created it.

**Where:** `native/audio/win/vcam_win.cpp:325-349`.

**Failure scenario:** a process with `SeCreateGlobalPrivilege` (another service, an admin tool) creates the
section first. The source refuses it ("squatted section", built-in image), but the helper opens it by name and
writes every live picture into it. The squatter receives the user's video. The `.Frame` event is opened the same
way, which is harmless.

**Fix:** after `OpenFileMappingW`, require `GetSecurityInfo` owner == LocalService or SYSTEM, mirroring the
source's `owner_is_service`, and report `vcam error "squatted"` otherwise.

### 8. MINOR: the elevated `vcam-install` copies a DLL from a possibly user-writable place, and the test seams are live in release.

*Status: plausible.*

**Where:** `native/audio/win/vcam_install_win.cpp:74-80, 195-214, 82-88, 327-330`.

**Failure scenarios:**
- In portable and dev trees, the install source `<exe dir>\camera\BeamerCameraSource.dll` is user-writable.
  Another process of the user can swap the file between the user's UAC consent and `CopyFileW` (the
  `files_equal` check followed by the copy is a TOCTOU). The swapped DLL is then loaded by Frame Server as
  LocalService.
- `--test-dir` (and `--test-reg`) are accepted by the elevated verb in every build. With `--test-dir` alone, the
  real HKLM COM class points into a caller-chosen, possibly user-writable folder.
- The `%ProgramFiles%\Beamer\Camera\<v>` folder "inherits Program Files' ACL". If Beamer's own installer created
  `%ProgramFiles%\Beamer` with a looser ACL, the DLL is user-replaceable. Unverified.

**Fix:**
- Open the source with `FILE_SHARE_READ` only, copy from that handle, and check the copy's hash or signature
  against a value compiled into the helper.
- Compile the test seams out of release builds, or refuse them when not `--dev`.
- Set an explicit protected DACL on the version folder.

### 9. MINOR: the pacer thread can end up joining itself.

*Status: plausible, unverified.*

**Where:** `native/audio/engine/pacer.cpp:139-148`, `flow.cpp:68-72`.

**Failure scenario:** `client = c->second.lock()` can make the pacer thread the last owner of a
`VideoSendStream`. This happens when the helper's `stop_stream` and the flow's `detach` drop their references
while a packet is being sent. When `client` goes out of scope, `~VideoSendStream` runs on the pacer thread.
If that releases the last `shared_ptr<Flow>` (the flow was closed meanwhile), `~Flow` resets `pacer_`, and
`~Pacer` joins its own thread. That throws `resource_deadlock_would_occur`, which leads to `std::terminate`.

**Fix:** in `~Pacer`, detach when `std::this_thread::get_id() == thread_.get_id()`. Alternatively, never let the
pacer thread drop the last reference: hand `client` to a deferred release, or keep the flow's `vsend_` reference
until `remove_stream` has returned.

### 10. MINOR: the video thread can miss a packet wake-up.

**Where:** `native/audio/engine/video_recv.h:228-232`, `video_recv.cpp:839-849`.

**Failure scenario:** `ingest` calls `cv_.notify_all()` without changing any state guarded by `mu_`. If a packet
completes a frame while the video thread is between `pump()` and `cv_.wait_until`, the notification is lost.
The frame is then handed only at the next deadline (≤ 33 ms display tick, or a present time). This adds up to
one tick to the start IDR's decode on some starts. The heartbeat is unaffected.

**Fix:** increment a counter under `mu_` in `ingest` (or push a "packets" mail) and wait with a predicate that
includes it.

### 11. MINOR: decoder output in a linear buffer uses the display width as stride, and the 2D-buffer path is not length-checked.

**Where:** `native/audio/win/videodec_win.cpp:256-269`.

**Failure scenario:** the sender accepts any even `codec.w` from 2 to 4096 (`helper.cpp parse_video_codec`).
Take a stream whose display width is smaller than its coded width (right cropping, e.g. 1272 inside 1280). On
the software path, the MFT writes rows `coded_w` bytes apart, but `copy_nv12_display(bp, w_, …)` reads them
`w_` apart, so the picture comes out sheared. On the `Lock2DSize` path, `blen` is never compared with
`pitch * coded_h * 3 / 2` before `copy_nv12_display` reads up to that offset. The decoder normally allocates
enough, but nothing checks it. The offered sizes (1280, 1920, 640 wide) are not affected today.

**Fix:**
- Use the output type's coded width (`MF_MT_FRAME_SIZE` width, or `MF_MT_DEFAULT_STRIDE`) as the pitch of linear
  buffers.
- Check `blen >= pitch * coded_h_ * 3 / 2` before copying.
- Alternatively, make the sender refuse widths that are not multiples of 16.

### 12. MINOR: DLL teardown edges.

*Status: plausible, unverified.*

**Where:** `native/audio/vcam-win/source.cpp:781-784`, `706-711`.

**Failure scenarios:**
- `Stream::Shutdown` waits 5 s for the delivery thread and then carries on. If the thread is still running (for
  example blocked in `QueueEventParamUnk`), the `Stream` can be freed under it.
- `Stream::GetMediaSource` calls `parent_->QueryInterface` on a raw `Source*`. If Frame Server releases the source
  without calling `Shutdown` while another thread calls `GetMediaSource`, the QueryInterface resurrects an object
  whose destructor is already running.

Both are crash-only (Frame Server restarts the camera); no hostile input reaches them.

**Fix:** wait without a timeout (the loop wakes at least every 1 s), or hold a COM reference to the `Stream` in
the thread. Give `Stream` a `ComPtr`/weak reference to its parent, or clear `parent_` in `~Source` before the
stream lock is released.

### 13. MINOR: `camera_lost` identifies the stream by a raw pointer that may already be reused.

**Where:** `native/audio/engine/helper.cpp:232-241`.

**Failure scenario:** the backend posts `camera_lost(h)`. Before the helper thread runs the lambda, the stream
stops, its handle is freed, and a new stream's handle is allocated at the same address. `owns_camera(h)` then
matches the wrong stream, which is stopped with `no_camera`.

**Fix:** pass a handle generation or ID with the call, or have `MacCameraHandle` carry the stream ID given at
`open_camera`.

---

## Checked and found sound

**Source DLL security (§4.6.3, all five rules):**
- Geometry comes only from constants; `magic/version/header_bytes/slot_count` are never read back.
- Every helper field is read once into a local. `mode`, `latest` and `beat` are validated, including a future
  beat.
- The seqlock copy validates `w`, `h` (even, within range), `w ≤ stride ≤ 4096`, and `stride*h*3/2 ≤ kSlotBytes`
  in 64-bit arithmetic on **locals**. It `memcpy`s into the private `staging_`, re-checks `seq`, and only then
  swaps the buffers, so a torn copy never reaches `good_`.
- Scaling and copying read only `good_`. I checked `scale_nv12`, `copy_nv12` and `draw_dot` for bounds at every
  offered size and at every valid slot geometry.
- The sample's `pitch`/`len` are checked against the app's type.
- The frame event can wake the delivery thread at most once per 2 ms, and work happens only per pending
  `RequestSample`, so a hostile writer cannot spin it.
- The process-token check makes the in-process validation copy inert.
- `OwnerSid` and `InstallGuid` are validated before they reach SDDL or object names. The squat check
  (owner LocalService/SYSTEM) is done on the section and on the frame event.
- The log is rate-limited, and `vsnprintf` truncation is safe (apart from finding 1).

**Helper side of the shared memory:**
- `want_*` is accepted only from `kOffered`, `streaming` only as 0/1, and `dll_version` within range.
- Live slots are written only with display sizes that passed `display_size_ok` (≤ 1920x1080, even), so
  `memcpy ≤ kSlotBytes`. The seqlock writer sequence (odd, fence, data, even) is correct.
- The Demand event is created with the DACL and an owner check, and the helper falls back to polling.
- The heartbeat comes only from the presenting thread (`sink_*` on the video thread, the idle loop otherwise).
- Timers: 500 ms close-hold and then `idle`; `starting` deferred behind the hold; `reconnecting` after 2 s of
  hold (`video_recv.cpp:648`); the DLL shows its built-in image when the beat is older than 500 ms.

**Network parsing (everything sits behind AEAD with the stream or flow key):**
- `read_header` and the AEAD run before any video parsing.
- `read_video_header` rejects `nfrag` of 0 or above 1024, `frag ≥ nfrag`, and reserved flags. Frames whose
  `nfrag` differs from the first packet's are ignored.
- `read_nack`, `read_pli`, `read_time_map` and `read_video_rr` all check their lengths.
- NACK seqs are bounded by the history's front and back before indexing (the history is contiguous: every seal
  is pushed).
- Feedback `ptype` is restricted to RR/NACK/PLI by `Flow::handle_packet`, so the internal mail types
  0xF0/0xF1 cannot be injected.
- Frame maps are capped (160 / 64), gap NACKs at 64, and give-up evidence at 256 frames.
- `ReplayWindowN<N>::shift` is equivalent to the old 128-bit code.
- Remaining exposure, if the key leaks: an authenticated peer can still make the receiver hold ~160 × 1.1 MB of
  fragments. That is noted only, not a finding.

**Nonces:**
- Sender-direction video packets are sealed only on the sender thread (`send_au`).
- Receiver-direction NACK/PLI/RR are sealed only on the network thread: `ingest` and `net_tick` under the core
  lock, from `handle_packet` and `video_tick`.
- The time map uses the existing `ctrl_sealer_` on the network thread.
- A retransmission re-sends identical stored bytes (same nonce, same plaintext).

**Protocol and state machines:**
- **Hand-off in frame order.** Loss is declared only on evidence: NACK give-up, a present time passed behind a
  higher seq, or a hole after the next seen frame's present time. There is no in-order shortcut in link state.
- **Clean chain.** `chain_clean_` is decided at hand-off. A loss or decoder error clears it, and only a complete
  IDR or refresh sets it. `keep = clean`. Revocation covers every later handed frame. `acked_` advances only
  over consecutive frames decoded ok while the stream is not broken. A frame whose reference chain is
  incomplete is never presented: I followed revoke, `run_start_` and the `on_decoded` order.
- **`clean_since`/`acked` on the sender.** Only sent LTRs inside `[clean_since, acked]` and not older than the
  newest numbered IDR are verified. An IDR erases the older sent and verified LTRs. Until an LTR is verified
  after an IDR, class-2 requests produce IDRs (rate-limited) or are deferred, never refreshes.
- **Start-PLI coalescing.** A request is pending from `start_encoding`/`request_idr` until the first packet has
  left, then for RTT + pace + 50 ms. After that, one per 1 s, with requests in between dropped (not queued).
  The scratch test of `SenderRecovery` behaved as specified.
- **IDR pacing.** Absolute `t0 + bytes_before / rate`, so a late wake sends the due packets back to back. The
  behind-IDR flag comes from `idr_queued`.
- **Clock offset.** The sign matches §4.5.3: the pong offset is `((t1-t0)+(t2-t3))/2`, and mapping uses
  `x - offset`. The offset is re-read for every frame. The provisional seed is
  `sender + enc + 20 ms - completion`, keeping the largest of 30 non-IDR samples. The time map uses the newest
  entry and modulo-2^32 ts.
- **Playout.** p99 + 8 ms within [floor, 150]: rises at once, falls at 30 ms/s, starts at 70 ms until 20
  samples. IDR and behind-IDR frames are excluded from the samples.
- **Late frames and backlog.** Late frames are shown on decode, never backwards. Backlog older than D + hold +
  150 ms is decoded but not kept.
- **Privacy guards.** No PLI within 3 s, or no authenticated feedback for 5 s, closes the camera at once in
  `fail()`, on the sender thread, without engine locks held. The helper then ends the stream. The Mac source
  stops its capture asynchronously when its last stream goes.
- **TCP fallback.**
  - Video goes to frame-tagged `link_out` events of at most 4 packets of one frame.
  - Feedback and the time map go by UDP plus the link outbox, only in link state.
  - In link state the sender switches to 1 Mbit/s and 15 fps; `video_link dropped` blocks NACK repair of those
    frames and triggers a refresh.

**Concurrency:**
- Engine lock order: `streams_mu_ → core.mu_ → OffsetEstimator`. Sender: `mu_ → pol_mu_`. `cam_mu_` is taken
  only on its own. The pacer `mu_` is released around `send_` and `on_paced`. I found no cycle.
- `on_fail_` and `camera_lost` are posted to the helper thread.
- Windows sink: decoding runs on the sink's own thread, and `on_decoded` goes through a mailbox. The pool is
  under `pmu_`, and pictures being written are marked `busy`. Teardown order: decoder thread joined, then
  `dead` set for the worker, then `sink_gone`.
- `Vcam::kick_create` never joins from inside a listener callback (the helper posts).

**Audio regression:**
- Peers without video see the same `handle_packet` paths: an RR for an audio stream is matched before
  `vsend_`.
- `ReplayWindow` behaves the same. The `sr` time map is off by default, and `render_ts_` publication is a
  lock-free pair.
- `Flow::next_deadline` gives the same values when no video stream exists.
- Two changes for audio-only flows:
  - The `NetLoop` wait now rounds down to whole ms and uses `select()` for the sub-ms rest, which adds a few
    wake-ups.
  - On POSIX, a socket fd ≥ `FD_SETSIZE` would turn the last < 1 ms before a deadline into `poll(0)` spins.
    That does not happen at this helper's fd counts.
- The always-open loopback `Waker` is connected to itself, so other local senders are filtered out.

## Not checked or not confirmed

- **No runs.** Nothing was executed on the PC or with real hardware. The `ProgramData` default ACL (finding 1)
  and the hardware decoder's creation time relative to the first IDR at start (finding 2.1) are taken from
  documentation and the CW4 report.
- **Mac backend parts.** `synth_mac.mm`, `scale_mac.mm` (beyond a skim) and the AVFoundation format-locking
  details were not reviewed for correctness.
- **Harnesses and tools.** `video_check.*`, `two_helpers_video.py`, and the vcam-win tools and scripts were not
  reviewed.
- **Stream keys.** Whether Beamer always issues a fresh key when a stream ID is reused is out of scope: it is the
  pre-existing audio rule. A reused (key, ID) pair would repeat sender nonces from seq 1.
