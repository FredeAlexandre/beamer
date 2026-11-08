# Audio across Beamer: status board

Operator's working memory for the audio/microphone mission (Mac <-> PC). Updated at every
significant step. Branch: `feat/audio` (from `dev/local-build`).

## En attente d'Alex

Grouped; nothing else is blocked. Ordered by what it unblocks.

1. **Mac virtual mic: done (2026-11-08)**, nothing waiting for Alex. `beamer-hal` installed
   (`/usr/local/libexec/beamer-hal` + `/etc/sudoers.d/beamer-hal`, NOPASSWD `install|uninstall|restart`;
   source in `proto/mac-driver/helper/`). The driver blocker (coreaudiod asks safety offset in both
   directions, unknown answer -> device dropped) is fixed; "Beamer Microphone" appears, the hidden feed
   loops to it bit-exact (17.5 ms at 512 frames, 4.2 ms at 128), input volume + mute added. README checks
   1-9 pass (`proto/mac-driver/README.md`, "Live test results"), except sleep/wake (not tested) and
   Zoom/Discord pickers by hand (not installed). **Note for Alex:** on this Mac coreaudiod, Brave, Hex,
   universalaccessd and ControlCenter regularly sit at 50-100 % CPU together, about a minute after
   coreaudiod starts, with or without the Beamer driver (measured 3 min each way). Not Beamer; quitting
   Brave/Hex one at a time would show which one triggers it.
2. ~~Decision: echo between machines~~ **decided by Alex 2026-11-08: (c)**, echo cancellation in Beamer
   when a forwarded mic and local speakers are both active, plus a headset warning in the UI.
3. ~~Decision: how the PC's active mic reaches apps~~ **decided by Alex 2026-11-08**: pick "CABLE Output"
   once in the apps; Beamer does not switch Windows default devices.
4. **Optional: PC Wi-Fi adapter power saving.** The MediaTek RZ616's "Power Saving" is Auto; it is the root cause of the Mac->PC jitter (W3, V3). Beamer works around it, but setting it to Off (Device Manager > adapter > Advanced) would remove most of the remaining spikes. A system setting, so yours to decide; also on DFS channel 52 at 160 MHz (radar events can stall the channel).
5. **Ear checks, 2 minutes, whenever you are at the machines** (not blocking): (a) PC: during a
   `--mute-local` run the G733 is really silent (the API says muted, no microphone can hear the headset);
   (b) plug/unplug a USB or Bluetooth audio device on each machine while `beamer-audio watch` runs (real
   hot-plug events were never seen, only simulated).

## Phase

**Phase 2: integration into Beamer — started 2026-11-08.** Alex confirmed the driver and the defaults
(echo: AEC + headset warning; PC: apps pick CABLE Output). Design and work packages: `dev/audio/PHASE2.md`.

Phase 1 (prototyping and technical proof) closed 2026-11-07 22:20, conclusions in `dev/audio/PHASE1.md`;
Mac virtual mic validated live 2026-11-08.

## Phase 2 decisions (operator)

- PC virtual mic render mode: **shared by default** (0 holes in 4x60 s + 300 s), exclusive render into CABLE
  Input (~45 ms instead of ~105 ms, measured by WP4) as an opt-in "low latency" setting, because exclusive
  takes CABLE Input from any other app and showed 1 hole in 5 min. Revisit after the E2E soak.

## Phase 2 tasks (design: `PHASE2.md` §13)

| WP | Where | State |
|---|---|---|
| Design `PHASE2.md` | Mac subagent | **done** 2026-11-08 (`03c8066`) |
| WP1 Python control plane (`core/audio`) | Mac, worktree `beamer-wp1`, branch `audio/wp1` | **done**, merged (`6321e5a`, `76734fe`): 81 tests pass Mac + PC, cluster convergence ≤ 5 rounds over 3,200 seeds; offer carries `host` (no LAN bind on the Mac) |
| WP2 Portable C++ media engine (`native/audio/engine`) | Mac `beamer-wp2` / `audio/wp2` (+ PC build) | running |
| WP3 Mac backend + helper, driver moved to `native/audio/mac-driver` | Mac `beamer-wp3` / `audio/wp3` | running |
| WP4 Windows backend + helper | PC agent, `beamer-wp4` / `pc/wp4` | **done**, merged (`1f7ea66`): Elgato virtual endpoints blocklisted (physical Wave:3 kept); loopback with endpoint muted -13.98 dBFS exact; self-exclusion digital zero; journal restore exact (14 endpoints); demand on CABLE Output 20-26 ms; exclusive render into CABLE Input 43-45 ms vs 103-111 ms shared (opt-in). Full helper not linked yet (needs WP2 engine); firewall prompt in the user session still to test via Beamer |
| WP8 Packaging and permissions | Mac `beamer-wp8` / `audio/wp8` | **done**, merged (`c45a500`): plist usage strings, audio-input entitlement, helper/driver bundling + signing (skipped until build scripts exist), driver install via beamer-hal (dev) or admin prompt (designed, mock-tested), Windows helper install + `restore`. Deployed both. Beamer had been found stopped on the Mac since 13:53 (no crash report) |
| WP5 Link integration | Mac `beamer-wp5` / `audio/wp5` (+ PC via deploy) | running |
| WP6 Page model + Mac Audio page | Mac `beamer-wp6` / `audio/wp6` | running; milestone 1 (`view.py`) committed `8102be9` |
| WP7 Qt Audio page | PC agent, worktree `beamer-wp7`, branch `pc/wp7` | running |
| WP9 Echo cancellation | Mac + PC | waits for WP2, WP3, WP4 |
| WP10 Two-machine E2E + soak | Mac + ssh | waits for WP1-5, WP8 (WP9) |

Coordination: deploys serialized with the lock dir `~/Developer/.beamer-deploy.lock`; each WP commits its
report as `dev/audio/reports/<wp>.md`; the operator merges into `feat/audio`.

## Start checks (2026-11-07)

| # | Check | Result |
|---|---|---|
| 1 | `sudo -n -l /usr/local/libexec/beamer-hal install` | FAIL: helper absent, no sudo rule |
| 2 | `ssh -o BatchMode=yes lebig whoami` | OK `lebig\alex` |
| 3 | PC clone clean on `dev/local-build`, `git fetch pc` | OK |
| 4 | Claude agent on the PC (`claude -p`) | OK (answers in ~4 s) |
| 5 | `dev/deploy.sh` both, Beamer running | OK (57 s; Mac pid up, PC session 1 via task; links Mac<->PC and PC<->asusrog up) |
| 6 | Windows Update paused, caffeinate | OK (paused until 2026-11-21T13:48Z; `caffeinate -dimsu` running) |

## Tasks

| Task | Owner | State |
|---|---|---|
| T1 Beamer link/pairing/protocol review -> `dev/audio/notes/beamer-link.md` | Mac subagent | **done** 15:06 (caps limit and caps count re-checked by operator) |
| T2 Mac CLI `beamer-audio` (inventory, mic, process tap + local mute, play, measures) -> `proto/mac/` | Mac subagent | **done** 15:45; local-mute result under independent re-check (V1) |
| V1 Independent check of Mac `capture-system --mute-local` (signal kept, speakers silent, restored on kill) | Mac subagent (blind) | **confirmed** 15:55: 5 runs each; tap = source level (-44.1 dBFS) muted or not; mic hears tone +30 dB unmuted, nothing above room noise muted; back 0.5 s after SIGTERM and SIGKILL |
| T3 `BeamerAudio.driver` HAL plug-in, built + tested out of process -> `proto/mac-driver/` | Mac subagent | **done** 15:28: universal bundle signed Beamer Dev, staged in `~/Developer/beamer-hal-staging/`; `test.sh` (plain + ASan/UBSan + TSan) 1526 checks pass, re-run by operator. **Install blocked** (beamer-hal). After install: 9 checks listed in its README (coreaudiod accepts the self-signed identity, hidden feed found by UID, latency/safety offsets, coreaudiod restart). Gaps: no volume/mute control yet, x86_64 slice untested. |
| T4 Windows CLI `beamer-audio` (WASAPI inventory, mic, loopback + local-mute experiments, play, measures) -> `proto/win/` | PC agent, branch `pc/audio-proto` (worktree `beamer-audio-proto`) | **done** 16:05, merged (`cb1029a`). Static C++ exe 470 KB. Local mute result under blind re-check (V2) |
| V2 Independent check: PC loopback captured while endpoint muted / volume 0; state restored after kill | PC agent (blind, `~\audio-verify`) | **confirmed** 16:25: -40/-50 dBFS in -> exactly -40.00/-50.00 out, unmuted, muted (tool and own script) and volume 0; capture killed -> unmuted within 0.2 s; capture + guard both killed -> stays muted until `restore-endpoint` (journal in %TEMP% correct); volume 0 also zeroes channel volumes (restore those too). All 14 active endpoints restored exactly. |
| T5 VB-CABLE: verify Microsoft signature, silent install or stop | PC agent (`~\audio-driver`) | **done** 15:10: Pack46 driver 3.5.2.0 installed silently (`VBCABLE_Setup_x64.exe -i -h`), Microsoft WHCP attestation signature verified before install and re-checked independently after (Win32_PnPSignedDriver signer). No prompt, no reboot asked, defaults unchanged. |
| T6 Transport prototype (UDP media + AEAD, Opus vs PCM, jitter buffer, drift, loss, latency) -> `proto/transport/` | Mac subagent (+ssh) | **done** 16:40 (`7493a56`). Temporary PC firewall rules removed (0 `Beamer-dev-*` left, checked). Numbers to be cross-checked by W2. |
| W1/W1b VB-CABLE internal latency (`VBAudioCableWDM_Latency`) | PC agents | **done** 19:10: **restored to 7168** (vendor default). One-process measurement (shared WASAPI duplex, sample-exact, 8 sessions/value, balanced order): shared-mode delay ~100 ms at 7168, 2048 and 512 alike (differences within ~20 ms session noise); the setting only matters to kernel-streaming clients (KS floor 40 ms); exclusive mode ~55 ms at every value. 512 has a reproducible hole under load. W1's flat numbers were real, plus 15-40 ms of its two-process harness. ~60 ms of the cable path is the Windows engine/endpoint buffering, not the driver. Report: PC `~\briefs\pc-cable-analyse.report.md`. |
| W2 Four scenarios end to end with the real tools + `--exclude-pid` against feedback loops; independent latency numbers -> `proto/scenarios/` | Mac subagent (+ssh) | **done** 19:20: all four directions + both at once work. Scenario 1 works but glitchy (62/min). |
| W3 Mac->PC jitter: root cause and fix | Mac subagent (+ssh) | **done** 20:50 (`60f6b4d`): cause = **PC Wi-Fi power save while it only receives** (MediaTek RZ616, Power Saving Auto: AP buffers the Mac's packets 20-130 ms then bursts). Raw probe: Mac->PC spikes/min 20-60 when the PC sends nothing, 2-7 when it sends every 10 ms, 1 when it sends 16 B/10 ms to the router only. Not AWDL, not the Mac sender (capture->send 0.6/0.85 ms p50/p99), DSCP EF no effect. Fix: 46-byte sealed receiver report every 10 ms from a receive-only side, catch-up by +0.2 % playback instead of drops, spike-aware JB target hold, decoder kept in step. Scenario 1, 10 min: **2.1 glitches/min** (was 50), one-way 108/116 ms (was 119/130). Confirmed by V3. |
| V3 Blind check: does Mac->PC jitter depend on the PC transmitting? | Mac subagent (blind) | **confirmed** 22:10: 15 x 65 s interleaved. Mac->PC spikes/min ~39 (PC silent), ~24 (PC every 100 ms), ~13 (PC every 10 ms); PC->Mac 0-2 whatever the Mac does; held-then-released bursts of 20-30 ms on the PC receive side -> PC adapter power save. Receiver reports reduce but don't remove the spikes; the spike-aware JB covers the rest. |

## Decisions

- **Media config** (from T6): mic = Opus mono VOIP 32 kb/s, 10 ms frames, in-band FEC on, DTX off; system sound = Opus stereo AUDIO 96-128 kb/s, 10 ms, FEC off (FEC forces speech mode). Jitter buffer adaptive: target = playback period + half a frame + p97 jitter + 2 ms, clamp 10-120 ms. Drift: buffer-level controller steering a resampler, driven from the playback device callback (in process), not from a pipe.
- **Latency budget**: Beamer's hop <= 50 ms one way (G.114: 150 ms mouth-to-ear comfortable; the call app itself adds 50-100 ms). Transport meets it (~33 ms); VB-CABLE at 7168 does not -> W1.
- **VB-CABLE latency setting left at its default 7168** (19:10): tuning it was tried (operator decision 16:45, reversible) and brings no gain on the shared-mode path apps use. To cut the PC virtual-mic latency, work on the ~60 ms outside the driver (e.g. Beamer rendering into CABLE Input in exclusive mode / smaller periods), not on this setting.
- **Receive-only peers send receiver reports every 10 ms** (W3): keeps a power-saving Wi-Fi radio awake (PC: MediaTek RZ616), costs 46 B/10 ms, no latency; doubles as the RTCP-like feedback channel (loss, jitter, buffer level) for the sender. We do not change the PC's adapter power setting (system setting, and users won't).
- **PC system-sound redirect = process loopback excluding Beamer's process tree + endpoint mute** (2026-11-07, confirmed by V2). In Beamer: keep the mute journal and replay `restore-endpoint` at every Beamer start, so a crash of both the helper and its guard never leaves the PC muted. Why: endpoint mute keeps loopback intact (session mute does not); excluding Beamer avoids sending remote audio back. Cost: endpoint mute also silences anything Beamer plays to that same endpoint (see open questions); a crash guard must restore the mute.

- **Common stdio contract for both prototypes** (2026-11-07): `beamer-audio list|watch|capture-mic|capture-system [--mute-local]|play|measure`, raw f32le interleaved, 48 kHz, mic mono / system stereo, diagnostics as JSON lines on stderr. Why: the transport prototype pipes any capture into any player on either OS without caring which side it runs.
- **Mac virtual mic = one plug-in, two devices** (visible input "Beamer Microphone", hidden output "Beamer Mic Feed", shared ring buffer). Why: Beamer just plays into a device by fixed UID, no custom IPC into coreaudiod; the hidden feed keeps output menus clean.
- **PC virtual mic = VB-CABLE** (CABLE Input fed by Beamer, apps record CABLE Output). Only one cable is allowed, so it is reserved for the mic; muting redirected PC system sound must come from another mechanism (T4 tests endpoint mute vs process loopback).

- **Transport = option B** (2026-11-07): control over the existing encrypted link behind one new capability `audio` (new `audio_*` JSON types, < 64 KiB, sent only to peers advertising it); media as Opus over a separate UDP socket (port 24823 proposed), per-packet ChaCha20-Poly1305 with a per-stream random key handed over the link; fallback to Opus frames over the link (<= 32 kb/s) if UDP fails. Why: the link has one FIFO per direction with no priority, and the accepting side processes one message at a time, so audio on it would delay input (and a lost TCP segment would stall both). Not C: a second TCP link from the same peer is closed by the receiver. Compat: wire version byte must never change (would cut asusrog); unknown message types are ignored by all 1.5.x; caps max 16, Mac already sends 13 (`mac_app/bridge.py:102`, `core/protocol.py:446`), so `audio` is the 14th.
- **Cluster view**: no hub, links are pairwise. Each machine announces its own devices to direct peers and forwards newer lists (like the shared screen layout). Audio streams only over a direct pairing.

## Measurements

### Mac (T2, MacBook Air built-in devices, 48 kHz)

| What | Value |
|---|---|
| IO buffer sizes granted | 128 / 256 / 480 / 512 frames, exactly |
| Hardware mic->speaker latency (measured) | 44.2 ms constant; reported device+safety+stream props sum to 70.4 ms (overstated by ~26 ms) |
| System sound: app render -> tap delivery | 8.0 / 13.3 / 21.3 ms at 128 / 256 / 512 frames |
| Local mic -> speaker through the tool | 62 ms (128 fr) / 77 ms (512 fr), incl. 10 ms play buffer |
| Clock drift mic vs speakers | 0.000 ppm over 330 s (same clock); both +2.07 ppm vs host clock |
| CPU | tool 0.3-0.5 %; tap aggregate without the speakers as clock: coreaudiod 0.1-0.4 % (with them: ~11 %) |
| Local mute with process tap (`.mutedWhenTapped`) | tap level unchanged (-24.2 dBFS); mic hears -43 dBFS unmuted vs -91..-96 dBFS (floor) muted; unmutes within 1 s of kill -9 |

### Transport (T6, Mac<->PC over Wi-Fi, Opus 10 ms, adaptive JB)

| What | Value |
|---|---|
| Network RTT | p50 5.1-5.7 ms, p95 6.6-8.9 ms; Tailscale = LAN; Wi-Fi stalls up to 1.46 s once, 48-119 ms a few times per 15 min |
| Transport one way (no devices) | 30-34 ms = network 2.7 + JB 20-24 + Opus 6.5 (PCM 27 ms, Opus RESTRICTED_LOWDELAY 26 ms; 20 ms frames 44 ms) |
| Full chain via VB-CABLE (Mac->PC CABLE->Mac round trip) | 152-164 ms (60 s), 176 ms p50 over 15 min; VB-CABLE alone 90-110 ms at its 7168-sample setting |
| Opus CPU | < 1 % of a core per stream (prototype Python total 10-17 % Mac, 4-7 % PC) |
| Loss, FEC on / off (glitch 10 ms windows per 45 s) | 1 %: 4 / 13; 5 %: 48 / 60; 10 %: 73 / 87; FEC useless on bursts; PCM+repeat ~2x worse than Opus |
| Clock drift Mac vs PC | host clocks +17.9 ppm (~64 ms/h); sample-clock estimates -18.1 / +15.7 ppm; buffers bounded over 15 min and 10 min |
| UDP blocked -> TCP fallback | switch after 2 s + 207 ms; same 65 ms loop with no loss |
| Firewall | PC: no rule needed if the PC sends first to the Mac's port (hole punch), LAN and Tailscale. Mac: no dialog, no log entry, even with a freshly re-signed interpreter |

### End-to-end scenarios (W2, Wi-Fi, 75 s runs, latency from per-machine timestamps joined by ping clock offset, error <= 2.3 ms)

| Scenario | One-way p50 / p95 | Glitches/min | CPU (one core) |
|---|---|---|---|
| 1 Mac mic -> PC virtual mic (CABLE Output) | before W3: 125.6 / 137.4 ms, 61.7 glitches/min; **after W3 (10 min): 107.7 / 115.8 ms, 2.1 glitches/min** (all at real Wi-Fi events); real mic: sound -> PC app 186 / 194 ms before W3 (Mac speaker->mic 71 ms) | 2.1 | Mac 7.1 %, PC 4.5 % |
| 2 PC mic (Wave:3) -> Mac | 29.5 / 37.1 ms to receiver output; 55.4 / 63.2 ms to the Mac speaker mix | 10.2 | transport Mac 11 %, PC 2.4 %; tools < 0.5 % |
| 3 Mac sound -> PC G733, Mac muted | 95.6 / 103.8 ms (loopback-observed on the PC) | 1.9 | Mac 4.1 %, PC 5.0 % |
| 4 PC sound -> Mac speakers, PC muted | 55.6 / 64.1 ms (2nd run 51.1 / 71.4) | 4.0 | Mac 14.9 %, PC 1.7 % |
| 5 = 3 + 4 at once | Mac->PC 100.1 / 107.0; PC->Mac 62.0 / 71.5 | PC 3.1, Mac 8.0 | Mac 18.8 %, PC 6.1 % |

Cross-check without clocks: a burst round trip Mac->PC->Mac took 146.7 ms = 96.4 + 50.3 (matches 3 and 4). Glitches line up with the receiver running dry during Wi-Fi delay spikes; 0 packets lost. Mac->PC shows 10-15 ms network jitter vs ~2 ms PC->Mac (+~15 ms buffer).
Loop test: with `--exclude-pid` each burst is seen once, then 96-99 dB down; without it bursts came back every 146.7 ms, 73-88 times, losing only 0.15 dB per lap.

### PC (T4, WASAPI shared mode, 48 kHz)

| What | Value |
|---|---|
| Period granted | 10 ms on mic and render (IAudioClient3 offers only 480 frames); buffer 1056 frames |
| `GetStreamLatency` | reports 0, unusable |
| Render write -> loopback | 40 ms (2-period queue), 30 ms (1 period) |
| `play` stdin -> `capture-system` stdout | 53 ms (2 periods), 36-42 ms (1 period) |
| Clock drift vs QPC over 360 s | mic -25.98 ppm, render -26.24 ppm (difference within error) -> expect tens of ppm Mac<->PC (26 ppm = 94 ms/h) |
| CPU (% of one core) | mic 0.09, system 0.15, play 0.10; no glitch in 60 s idle or with 12 cores busy |
| Local mute | endpoint mute or endpoint volume 0: loopback (endpoint and process) keeps the full signal (-13.98 dBFS in = out), on G733 and DualSense. Session mute/volume 0: loopback silent (unusable). Mute guard restores within 1 s after kill of the tool. |
| Fixes | own 7.1->stereo fold-down (WASAPI downmix lost 13.5 dB); `play` uses WASAPI conversion (unity gain) |

## PC audio endpoints (2026-11-07)

| Role | Endpoint | Notes |
|---|---|---|
| Default playback (console, multimedia) | Logitech G733 | |
| Default playback (communications) | DualSense | Zoom/Discord may follow the communications role |
| Default recording (console, multimedia) | Elgato Wave:3 Mic In | |
| Default recording (communications) | DualSense | |
| Virtual mic sink (Beamer plays into it) | CABLE Input "Haut-parleurs (VB-Audio Virtual Cable)" `{0.0.0.00000000}.{53EE99D1-271D-45C8-B798-D304F02687BA}` | VB-CABLE |
| Virtual mic (apps record it) | CABLE Output `{0.0.1.00000000}.{DDC7E301-3260-4ED3-8389-0938254820E0}` | VB-CABLE |
| (unused) | CABLE In 16 Ch | VB-CABLE |

## Open questions

- Mac awdl0 is active but W3 found it is not the cause of the Mac->PC jitter.
- Remaining Wi-Fi events: one 0.66 s stall and 2-3.4 s episodes of 45-130 ms delays once per 5 min, cause unknown. Scenarios 2-5 not re-run with the W3 fixes. Opus decoder after a lost frame gives false glitch detections on loud chirps: earlier mic-path glitch counts are slightly pessimistic.

- **Acoustic coupling between the two machines** (W2): the Wave:3 hears the MacBook speakers at about -11 dB; a first mic-forwarding trial howled for ~10 s. Forwarding a mic while the other machine plays through speakers needs echo cancellation (e.g. macOS voice processing IO / Windows AEC on the capture side) or a headset. Product decision for Phase 2.
- Windows `--exclude-pid` does not follow a venv launcher (`python.exe` shim): exclude the PID of the process that actually opens the player; in Beamer `os.getpid()` of the real process; check the PyInstaller bootloader parent/child split. Only one process tree can be excluded; Windows loopback covers every render device of the endpoint mix.
- System sound Mac -> PC at ~96 ms one way exceeds the 50 ms hop budget (mic budget); fine for media but worth trimming: where are the extra ~40 ms vs PC -> Mac (Mac->PC Wi-Fi jitter, PC G733 render path)? Phase 2 tuning item.

- PC: while PC sound is redirected away (endpoint muted), sound coming from another machine cannot play on that same endpoint. Options: play it on another endpoint, or per-app routing (undocumented IAudioPolicyConfig), or accept the limitation. To decide in Phase 2 with Alex's typical setup in mind.
- PC: the API reports muted, but nobody has heard the G733 during `--mute-local` (the Elgato mic can't hear the headset). Human ear check goes into the grouped list of human checks.
- PC communications default is the DualSense (in and out): picking "the active mic" on the PC may mean setting CABLE Output as the communications default (IPolicyConfig is undocumented) or telling Alex to select CABLE Output in each app.

- Mac feedback loop: a tap excluding only its own pid re-captures what Beamer plays (e.g. PC sound played on the Mac would be sent back). In Beamer the tap must exclude Beamer's own process (and any helper that plays).
- `play` (both OSes) needs drift compensation against a remote clock: done in the transport receiver (T6), not in the device tool.

- Mac virtual mic: add an input volume + mute control to BeamerAudio (Zoom auto-level, macOS input slider) — after the first real install.
