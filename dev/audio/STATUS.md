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
| T1 Beamer link/pairing/protocol review -> `dev/audio/notes/beamer-link.md` | Mac subagent | running (started 15:00) |
| T2 Mac CLI `beamer-audio` (inventory, mic, process tap + local mute, play, measures) -> `proto/mac/` | Mac subagent | running |
| T3 `BeamerAudio.driver` HAL plug-in, built + tested out of process (install blocked, see above) -> `proto/mac-driver/` | Mac subagent | running |
| T4 Windows CLI `beamer-audio` (WASAPI inventory, mic, loopback + local-mute experiments, play, measures) -> `proto/win/` | PC agent, branch `pc/audio-proto` (worktree `beamer-audio-proto`) | running |
| T5 VB-CABLE: verify Microsoft signature, silent install or stop | PC agent (`~\audio-driver`) | **done** 15:10: Pack46 driver 3.5.2.0 installed silently (`VBCABLE_Setup_x64.exe -i -h`), Microsoft WHCP attestation signature verified before install and re-checked independently after (Win32_PnPSignedDriver signer). No prompt, no reboot asked, defaults unchanged. |
| T6 Transport prototype (UDP vs TCP, PCM vs Opus, jitter buffer, drift, loss, latency) | - | waiting for T1 (design), T2/T4 (endpoints) |

## Decisions

- **Common stdio contract for both prototypes** (2026-11-07): `beamer-audio list|watch|capture-mic|capture-system [--mute-local]|play|measure`, raw f32le interleaved, 48 kHz, mic mono / system stereo, diagnostics as JSON lines on stderr. Why: the transport prototype pipes any capture into any player on either OS without caring which side it runs.
- **Mac virtual mic = one plug-in, two devices** (visible input "Beamer Microphone", hidden output "Beamer Mic Feed", shared ring buffer). Why: Beamer just plays into a device by fixed UID, no custom IPC into coreaudiod; the hidden feed keeps output menus clean.
- **PC virtual mic = VB-CABLE** (CABLE Input fed by Beamer, apps record CABLE Output). Only one cable is allowed, so it is reserved for the mic; muting redirected PC system sound must come from another mechanism (T4 tests endpoint mute vs process loopback).

## Measurements

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
