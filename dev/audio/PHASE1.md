# Audio across Beamer: Phase 1 conclusions (technical proof)

Date: 2026-11-07. Branch `feat/audio`. Everything below was measured with the standalone prototypes in
`dev/audio/proto/` between the Mac (MacBook Air) and the PC (`lebig`, Windows 11), both on Wi-Fi.
Key results were re-checked by independent agents that were not told the expected answer (V1-V3 in
`STATUS.md`). Details, methods and raw tables: each prototype's `README.md`.

## Verdict per question

| Question | Answer | Evidence |
|---|---|---|
| Device inventory and hot-plug | Core Audio and WASAPI/MMDevice both enumerate inputs/outputs/defaults and notify changes. | `proto/mac`, `proto/win` `list`/`watch`. Real USB/Bluetooth plug events not yet seen (simulated only). |
| Capture the microphone | Yes on both, < 0.5 % CPU, no permission dialog from the shell. | T2, T4 |
| Capture system sound, muted locally | **Mac**: process tap with `mutedWhenTapped`: capture at source level, speakers silent, sound back 0.5 s after the tap dies (even SIGKILL). **PC**: loopback keeps the full signal when the *endpoint* is muted or at volume 0 (session mute kills it); a guard + journal restores the endpoint. | T2 + V1, T4 + V2 |
| No feedback loop when both directions run | Exclude the playing process from system capture (`--exclude-pid`): each burst seen once, then 96-99 dB down. Without it, bursts loop every 147 ms. | W2 scenario 5 |
| Transport | Control over Beamer's existing encrypted link (new `audio` capability); media as Opus over a separate UDP flow, ChaCha20-Poly1305 per packet with a per-stream key sent over the link; TCP fallback in ~2.2 s. | T1, T6 |
| Old peers (asusrog, official 1.5.x) | Unaffected: wire version unchanged, unknown messages ignored, caps 14/16. | T1 (code-checked by operator) |
| Firewalls | No new rule on either machine: Mac initiates; PC is reached because it sends first on the flow. No Mac dialog even with a freshly signed interpreter. | T6, V3 |
| Latency (one way, Wi-Fi) | Transport ~30-34 ms (network 3, jitter buffer 20-24, Opus 6.5). End to end: PC mic -> Mac 30 ms (55 ms to the speaker mix); PC sound -> Mac 51-56 ms; Mac sound -> PC headset 96 ms; Mac mic -> PC app via VB-CABLE 108 ms. | W2, W3 |
| Clock drift | Mac vs PC ~17 ppm (~61 ms per hour). Buffer-level controller + resampler keeps buffers bounded (15 min and 10 min runs). | T6, V3 |
| Loss | Opus PLC + in-band FEC: 1 % loss fully recovered; FEC useless on bursts. | T6 |
| Remote mic as a real input | **PC**: VB-CABLE (Microsoft-attested, installed silently) works: Beamer plays into CABLE Input, apps record CABLE Output. **Mac**: `BeamerAudio.driver` built and tested out of process (1526 checks); install blocked on `beamer-hal`. | T5, T3, W2 s1 |
| Local sound to another machine | Works both ways (scenarios 3, 4, 5). | W2 |

## Things we learned the hard way

- **The PC's Wi-Fi adapter dozes when it only receives** (MediaTek RZ616, Power Saving Auto): the access
  point holds incoming packets 20-130 ms and releases them in bursts. A receive-only peer therefore sends a
  46-byte sealed receiver report every 10 ms; with catch-up instead of drops and a spike-aware buffer
  target, Mac mic -> PC went from 50 to 2.1 glitches/min (10 min run). Remaining glitches sit on real
  Wi-Fi events (0.66 s stall, multi-second episodes every few minutes).
- **VB-CABLE costs ~100 ms on the shared-mode path apps use**, whatever its internal latency setting
  (left at 7168, its default). ~60 ms of it is the Windows audio engine; exclusive-mode rendering into
  CABLE Input measured ~55 ms and is the next lever.
- **Two machines on one desk hear each other** (Wave:3 hears the MacBook speakers at -11 dB): forwarding a
  mic towards speakers howls. Needs echo cancellation or a headset rule (decision pending, STATUS).
- **Windows process exclusion does not follow launcher processes** (venv `python.exe` shim): exclude the
  PID of the process that actually renders. Only one process tree can be excluded.
- **Endpoint mute on the PC silences the whole device**, including remote sound Beamer would play on it.
- **Mac tap aggregate**: don't include the output device as clock (coreaudiod 11 % CPU) and don't use
  auto-start (no data while the system is silent).
- Reported device latencies are unreliable on both OSes (Mac overstates by ~26 ms; WASAPI
  `GetStreamLatency` returns 0): measure, don't trust properties.

## Recommended architecture for Phase 2 (integration into Beamer)

**Per machine, one native audio helper** (the prototypes' device tools grown up: Swift on the Mac, C++ on
Windows; Linux later) owning capture, playback, mute, exclusion and device notifications, with the jitter
buffer and resampler driven **from the playback device's callback** (not through a pipe). Beamer's Python
process keeps the control plane: link messages, settings, UI, keys. Encode/decode stays native (Opus,
~1 % CPU). The helper runs as a child of Beamer so Beamer's tree is excluded from system capture.

**Wire (control, on the existing link, only to peers advertising `audio`):**
- `audio_hello` (version, codecs, sample rates), `audio_devices` (this machine's inputs/outputs, defaults,
  levels capability; re-sent on change and forwarded with a version stamp so every machine in the cluster
  sees every machine's devices, like the shared screen layout), `audio_route` (desired routing state, last
  writer wins with a timestamp), `audio_open`/`audio_close` (stream id, direction, device, codec params,
  random 32-byte key, UDP port), `audio_stats` (optional, for the UI vu-meters/levels).
- Media: UDP 24823, packet = header (version, stream id, seq, timestamp) as AEAD associated data + Opus
  payload; receiver reports every 10 ms; TCP fallback carries the same frames on the link at <= 32 kb/s.

**Media settings:** mic = Opus mono VOIP 32 kb/s, 10 ms, FEC on, DTX off; system sound = Opus stereo AUDIO
96-128 kb/s, 10 ms, FEC off. Adaptive jitter buffer 10-120 ms (target = playback period + half a frame +
p97 jitter + 2 ms, spike hold), drift +-500 ppm max via resampling, catch-up at +0.2 % speed.

**Virtual devices:** Mac `BeamerAudio.driver` ("Beamer Microphone" visible input, hidden "Beamer Mic
Feed" fed by the helper by UID); PC VB-CABLE (helper plays into CABLE Input, apps pick CABLE Output).

**UI:** a new "Audio" page beside the screens page (`mac_app/pages.py`, `win_app/pages_win.py`), showing
the machines as on the screens page with their mics and outputs; pick the active mic (any machine) and,
per machine, where its sound plays; levels when available. Settings persist and re-apply on reconnect.

**Packaging:** Mac Info.plist gets `NSMicrophoneUsageDescription` + `NSAudioCaptureUsageDescription`
(`mac_app/setup.py`), entitlements get `com.apple.security.device.audio-input`
(`mac_app/entitlements.plist`); first use will show the macOS permission prompts once (needs Alex).
Windows ships the helper exe next to `Beamer.exe`.

## Open before or during Phase 2

1. `beamer-hal` (Alex) to install and validate `BeamerAudio.driver` for real (9 checks in its README),
   then add input volume/mute to it.
2. Echo policy (AEC vs headset rule) and how the PC's active mic reaches apps (default-device switch vs
   picking CABLE Output once): decisions listed in STATUS.
3. Cut the PC virtual-mic path: exclusive-mode render into CABLE Input; re-run scenarios 2-5 with the W3
   transport fixes.
4. Real hot-plug and an ear check of the PC endpoint mute (Alex, 2 minutes).
5. Linux (asusrog) as an audio peer: out of scope for Phase 1; the protocol keeps it possible.
