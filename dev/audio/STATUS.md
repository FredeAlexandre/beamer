# Audio across Beamer: status board

Operator's working memory for the audio/microphone mission (Mac <-> PC). Updated at every
significant step. Branch: `feat/audio` (from `dev/local-build`).

## En attente d'Alex

1. **Mac: the `beamer-hal` root helper is missing** (start check 1 failed, 2026-11-07 14:51).
   `/usr/local/libexec/beamer-hal` does not exist and `sudo -n -l /usr/local/libexec/beamer-hal install`
   asks for a password, so there is no sudoers rule either. Needed: the helper script plus a
   NOPASSWD rule for `beamer-hal install|uninstall|restart`. Unblocks: installing
   `BeamerAudio.driver` (the remote mic as a real input on the Mac). Everything else continues;
   the driver is built and tested as far as possible without installing it.

## Phase

**Phase 1: prototyping and technical proof** (`dev/audio/proto/`, standalone programs, no change
to the Beamer app).

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
| T4 Windows CLI `beamer-audio` (WASAPI inventory, mic, loopback + local-mute experiments, play, measures) -> `proto/win/` | PC agent, branch `pc/audio-proto` (worktree `beamer-audio-proto`) | running |
| T5 VB-CABLE: verify Microsoft signature, silent install or stop | PC agent (`~\audio-driver`) | **done** 15:10: Pack46 driver 3.5.2.0 installed silently (`VBCABLE_Setup_x64.exe -i -h`), Microsoft WHCP attestation signature verified before install and re-checked independently after (Win32_PnPSignedDriver signer). No prompt, no reboot asked, defaults unchanged. |
| T6 Transport prototype (UDP media + AEAD, Opus vs PCM, jitter buffer, drift, loss, latency) -> `proto/transport/` | Mac subagent (+ssh) | running (started 15:20; synthetic signals until T2/T4 land) |

## Decisions

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

- Mac feedback loop: a tap excluding only its own pid re-captures what Beamer plays (e.g. PC sound played on the Mac would be sent back). In Beamer the tap must exclude Beamer's own process (and any helper that plays).
- `play` (both OSes) needs drift compensation against a remote clock: done in the transport receiver (T6), not in the device tool.

- Mac virtual mic: add an input volume + mute control to BeamerAudio (Zoom auto-level, macOS input slider) — after the first real install.
