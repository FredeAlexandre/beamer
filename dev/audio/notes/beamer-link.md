# How Beamer links machines, and what audio can reuse

Read-only study of `feat/audio` (core identical to upstream `v1.5.19`), 2026-11-07. Paths are
repo-relative; `file:line` refer to this commit. Spec = `WIRE.md` (wire version 6) and `PAIRING.md`.

Live setup observed: Mac `alexandres-macbook-air` (192.168.1.138) <-> PC `lebig` (192.168.1.134)
<-> Linux `asusrog` (192.168.1.135, official upstream 1.5.x). All on port 24820, plain LAN (the
Tailscale name is only used for SSH). The Mac is **not** paired with asusrog.

## 1. Link transport

| Aspect | Fact | Where |
|---|---|---|
| Transport | One TCP connection per direction per pair, IPv4 only. Listener binds `0.0.0.0:<port>`, `listen(8)` | `core/receiver.py:690-760` (bind at :750) |
| Ports | TCP **24820** links (`DEFAULT_PORT`, per-machine `port` setting); UDP **24821** beacons/discovery; TCP **24821** pairing exchange only while a code is shown. Mac SSH-tunnel fallback uses local 127.0.0.1:**24822** | `core/protocol.py:57-58`, `mac_app/bridge.py:85-88`, `WIRE.md:1028-1060` |
| Who dials | Every desktop listens; each machine keeps one **outbound** link to every peer with `send` true (+host+port), redialling every 2 s. Input only flows initiator -> responder, so two desktops that drive each other have **two links**, one each way (hence both directions on 24820 in logs) | `WIRE.md:20-24, 245-255`; `core/link.py:79` (`OutboundLink`), `core/receiver.py:487` (`LinkResponder`); Mac `bridge.py:2357-2385` |
| Preamble | 62 bytes cleartext each way: `BEAMY` + version `0x06` + 8-byte nonce prefix + 16-byte pair key id + 32-byte ephemeral X25519 share | `WIRE.md:257-287`; `protocol.py:578-617, 725-728` |
| Key exchange | Ephemeral X25519 per link + long-term pair token. `key(dir) = HKDF-SHA256(ikm = token_text || X25519 shared, salt "beamer-link-v6", info "beamer-frames"||0x06||key_id||prefix_I||prefix_R||share_I||share_R||dir)`; dir 1 = initiator seals, 2 = responder. Forward secret: ephemeral secret and `shared` are dropped after key derivation | `WIRE.md:388-418`; `protocol.py:540-565, 730-756` (`shared` is a local at :744, secret wiped :746) |
| Key id | `HKDF(token, salt "beamer-link-v6", info "beamer-key-id")[:16]`, lets the responder pick the token before decrypting | `protocol.py:540` |
| Framing | `len(4,BE) || counter(4,BE) || ChaCha20-Poly1305 ciphertext || tag(16)`; nonce = sender's 8-byte prefix || counter; no AAD; counters strictly +1 from 1 | `WIRE.md:420-444`; `protocol.py:758-798` |
| Payload | One UTF-8 **JSON** object `{"type","data"}` per frame (binary must be base64). Non-JSON plaintext after the handshake **ends the link** | `WIRE.md:46-51`; `protocol.py:636-686` |
| Sizes | First frame <= 4096 B; others <= 16 MiB, but any plaintext > 64 KiB that is not an expected clipboard is dropped unparsed (link stays) | `protocol.py:82-93`; `link.py:615-617`; `receiver.py:974-976` |
| Handshake | Responder: <= 4 pending slots, 1/address, preamble in 1 s, whole handshake 5 s; `hello` -> `welcome` (ids, name, platform, app, caps, port, hw) | `WIRE.md:289-386`; `receiver.py:781-845`; `link.py:451-503` |
| Socket opts | `TCP_NODELAY` both sides; `SO_KEEPALIVE` initiator only; **no SO_SNDBUF/SO_RCVBUF** tuning (OS defaults/autotuning) | `link.py:455-456`; `receiver.py:904` |
| Liveness | Initiator sends `ping` after 1 s idle (or a `ping_rtt` probe every 1 s); responder sends an `ack` heartbeat every 0.4 s. Initiator ends link after 2.0 s without a byte, responder after 2.5 s. Initiator also ends link if the oldest un-acked input is > 1.5 s old | `WIRE.md:592-597`; `link.py:38-46, 604, 680-712`; `receiver.py:29-35, 118` |
| Reconnect | Outbound link loop redials every 2 s (`RECONNECT_SECONDS`), 30 s after "forgotten" evidence; beacon-follow to a moved address after 10 s down | `link.py:35-37, 267-353`; `WIRE.md:1035-1044` |
| Duplicate links | Responder step 10: a new authenticated link from the same peer id **closes the earlier inbound one** (and ends its ownership) | `WIRE.md:339-341`; `receiver.py:908-915` |

## 2. Versions and capability negotiation

- **Wire version** is one byte in the preamble; v6 refuses <6 and >6 outright (short reply, close).
  There is no minor version. `protocol.py:599-617`, `WIRE.md:302-311`. Bumping to v7 would cut
  off the Linux peer: never do it for audio.
- **Capabilities**: `caps` list in `hello`/`welcome`, strings of <= 32 printable ASCII, **at most 16**.
  `WIRE.md:457, 476-492`; validation `protocol.py:446, 885-910`. A hello with 17 caps is
  malformed -> the responder answers `invalid_hello` and closes (`WIRE.md:328-333`). Same limit in
  every upstream tag v1.5.0..v1.5.19.
- **Current caps (13 of 16)**: Mac `mac_app/bridge.py:102`, PC/Linux `win_app/kvm_bridge_win.py:2026`:
  `clipboard, clipboard_image, gestures, media_keys, text, settings, design_sync, layout,
  profiles, displays, ping_rtt, identify_screens, motion_path`. **Only 3 slots left** for the fork
  and upstream combined: spend exactly one (`audio`) and version inside the audio messages.
- **Unknown message types are ignored** (spec `WIRE.md:47`). Code: responder `_dispatch` has no
  `else` (`receiver.py:1044-1072`); Mac initiator logs debug and drops (`bridge.py:2592`); PC/Linux
  initiator falls through (`win_app/sender.py:1019-1077`). Unknown keys in `data` are ignored.
  A malformed message is ignored except a malformed first frame or `ack` (link ends).
- **Gating**: "a sender does not send a message its peer's caps do not cover" (`WIRE.md:491-492`).
  Pattern: `_CAP_FOR` map + checks in `OutboundLink.post/send_input` (`link.py:53-54, 766-801`) and
  `_Link.post` (`receiver.py:419-431`); receivers double-check both ends' caps (e.g. identify:
  `receiver.py:1061-1064`).
- **Safe recipe for new message types** (as `identify_screens` did, `WIRE.md:1333-1335`):
  1. add one cap `audio` to both cap lists (stay <= 16);
  2. new `type`s with a common prefix (`audio_hello`, `audio_devices`, `audio_open`, ...), JSON
     objects, each < 64 KiB plaintext, no `seq` (never in `INPUT_TYPES`, so acks/watchdog untouched);
  3. add them to `_CAP_FOR` (initiator) and to `_Link.post` (responder) so old peers never get them;
  4. receive side: new `elif` in `receiver._dispatch`, `bridge._link_message`, `sender.on_message`,
     checking `"audio" in link.caps and in own caps`; carry a `v` field inside `audio_hello` for
     future negotiation (codecs, ports, frame sizes) instead of new caps.
  Old peers: never see the messages; even if they did, they would ignore them.

## 3. Pairing, identities, authentication

- **Identity**: 16-byte random machine id per install (`settings.json` `machine_id`), never in
  clear on a link; carried sealed in `hello`/`welcome` and checked against the entry
  (`WIRE.md:34-39, 334-337, 379-381`).
- **Long-term secret per pair**: a 32-byte token (`b64`, 43 chars) from CPace pairing v3
  (6-digit code, optional QR secret), stored in each side's peer entry; one token per pair, never
  reused (`WIRE.md:40-45, 924-1026`; `PAIRING.md:47-100`; `core/pairing.py`). `easy_pair.py` is an
  experimental SAS mode (`sas_v1`) not yet released; `feature_flags.py` only has `PAIRING_QR_VISIBLE`.
- **Authentication** = possession of the token: only the holder can produce frames under the
  derived keys; the key id selects the token; machine id mismatch -> `wrong_id`. Address is not
  authenticated (`WIRE.md:494-513`).
- **Fresh key for a media channel, no new user step** - two options, both fine:
  - *In-band key (simplest, recommended)*: the side opening a stream draws 32 random bytes and sends
    them inside the encrypted, authenticated link (`audio_open {key, stream_id, udp_port, ...}`).
    Inherits the link's forward secrecy; new key per stream / per link-up. No crypto spec change.
  - *Exporter*: in `LinkSession.accept_preamble` (`protocol.py:730-756`), before `shared` goes out
    of scope, also derive `media = HKDF(ikm = token || shared, salt "beamer-link-v6", info
    "beamer-media" || key_id || prefix_I || prefix_R || share_I || share_R)`. Needs a code change in
    LinkSession on both new peers; old peers unaffected. Caveat: two links per pair -> two exporters;
    the stream must name which link it came from.
  Per-datagram AEAD: ChaCha20-Poly1305 with nonce = 4-byte (stream id | direction) || 8-byte packet
  counter, AAD = header; sliding replay window (e.g. 64-128 packets) instead of strict +1.

## 4. Threads, dispatch, and how much the link can carry

**Initiator (OutboundLink, both apps)** - `core/link.py`:
- Threads: `Beamer-link` (connect/handshake/reader) and `Beamer-link-writer` (`link.py:506-551`).
- Reader thread opens frames, handles `ack`/`ping` itself, then calls the app's `message`
  callback **synchronously on that thread** (`link.py:660-667`). Mac: `bridge._link_message`
  (`mac_app/bridge.py:2540-2594`); PC/Linux: `LinkSender.on_message` (`win_app/sender.py:1019`).
  A slow handler delays `ack` processing -> watchdog risk. Audio handlers must only enqueue.
- Outbox: one FIFO deque, **no priority** (`link.py:803-831`); limits 2048 messages / 32 MiB
  (`link.py:41-42`); a full queue makes `send_input` fail = link treated as failed. Writer stalls the
  head while 32 inputs are un-acked (`INPUT_WINDOW`, `link.py:836-840`): anything behind waits too.
  Mouse moves are coalesced in the queue (`link.py:813`). Writes in 64 KiB chunks.

**Responder (LinkResponder, both apps)** - `core/receiver.py`:
- Per link: `Beamer-v6-reader` (read-ahead), `Beamer-v6-writer`, and the serve thread that
  dispatches (`receiver.py:901-967`). The reader **blocks after every non-mousemove message until
  it has been dispatched** (`receiver.py:455-461`), so an audio frame handler is serialised with
  input injection (and vice versa: a 250 ms slow injection delays audio).
- Writer sends `outbox` first and generates `ack`s **only when the outbox is empty**
  (`receiver.py:1003-1020`): a steady responder->initiator audio stream can starve acks under
  congestion, and the initiator kills the link after 1.5 s of un-acked input. `OUTBOX_LIMIT = 512`
  messages closes the link (`receiver.py:126, 425-428`) - about 10 s of 20 ms frames.
- Callbacks reach the UI thread via `AppHelper.callAfter` (Mac, e.g. `kvm_bridge_app.py:5109`) and Qt
  `Signal`s on `StatusBridge` (PC, `win_app/kvm_bridge_win.py:314-330, 627-650`).
- Construction: Mac responder `mac_app/windows_input.py:53-78`, Mac links `bridge.py:2372`;
  PC responder `win_app/kvm_bridge_win.py:627`, PC links `win_app/sender.py:1791-1968`.

**Where audio hooks in**: a new `core/audio_link.py` (pure, like `settings_sync`) owning the audio
control state machine and UDP socket; fed by new `elif` branches in the three dispatchers above,
given `post(peer, msg)` through `bridge`/`LinkSender.send_to` + `responder.send`
(`receiver.py:1735`), and started on link-up via the existing `announce`/`up` hooks
(`bridge._announce_to` `bridge.py:2508`, `sender._up` `sender.py:1936`). Peer capability
discovery should copy `bridge.profiles_capable()` (`bridge.py:889-897`), which unions outbound and
inbound link caps.

**Budget if audio rides the link**: Opus 20 ms @ 64 kb/s ~ 160 B -> ~300 B JSON+base64, 50 msg/s,
~15 KB/s: negligible next to the link and small vs mouse traffic. Raw PCM 48 kHz stereo s16 =
192 KB/s (5 KB frames base64) is feasible on wired LAN but adds kernel send-buffer queueing ahead of
input (NODELAY prevents Nagle, but not bufferbloat: no SO_SNDBUF cap). Any TCP loss stalls input
and audio together for an RTO (~200-300 ms).

## 5. Cluster model

- **No hub, no global membership.** Each machine has its own `peers` list (<= 32) with one token
  per pair; links are strictly pairwise and direct (`WIRE.md:77-158`). The "hub" in
  `dev/README.md` is dev tooling only.
- Each peer entry: `id, name, label, platform, token, host (IPv4), host_pinned, port, hw, send,
  allow_drive, side, paired_with, linked, ...` (`WIRE.md:104-132`). Host learned from pairing,
  inbound link source, or beacons (UDP 24821, every 2 s, name+platform+port).
- **Third parties**: `paired` message lists the sender's other desktop peer ids
  (`WIRE.md:605-608`) -> stored as `paired_with`. Names of third-party machines only arrive via the
  shared `layout` tiles (flooded newer-wins to `settings`+`layout` peers, `WIRE.md:1522-1550`;
  `core/settings_sync.py:345-410`).
- Chains require a full mesh: B never relays A's input to C (`WIRE.md:856-866`).
- Today: Mac knows PC; PC knows Mac and asusrog; Mac sees asusrog only as an id in PC's
  `paired_with` (and as a named tile if layout sharing carries it).
- **For every machine to see every machine's audio devices**: each audio-capable machine announces
  its own `audio_devices {set_at, by, devices:[...]}` to direct peers with `audio`; a receiver may
  forward newer states to its other `audio` peers (same newer-wins flood as `layout`) for
  *display*. Media itself should flow only over a direct pairing (same rule as input), or be
  explicitly relayed by an intermediate machine (a hub mode, extra latency, decrypt/re-encrypt).
  asusrog (old) will never advertise `audio`, so it shows as "no audio" and must be updated or
  paired directly to take part.

## 6. Firewalls

- **Windows** (`win_app/firewall_win.py:23-34, 338-344, 378-397`; installer
  `win_app/Install-Maintenance.ps1:66-87`): three inbound Allow rules, profile **Private** only
  (Domain optional, never Public), each scoped to **both** `-Program Beamer.exe` and a port:
  `Beamer Receiver (TCP-In)` TCP 24820 (the configured port), `Beamer Pairing (UDP-In)` UDP 24821,
  `Beamer Pairing (TCP-In)` TCP 24821. A new port means a new rule (+ repair for existing
  installs, elevation needed). Outbound is open; WFP is stateful, so UDP replies to datagrams the
  PC sent first are admitted without any rule.
- **Mac**: application firewall on, stealth mode on, "allow signed (built-in and downloaded)" on,
  and `/Applications/Beamer.app/Contents/MacOS/Beamer` is listed **Allow incoming connections**
  (`socketfilterfw --listapps`). The rule is per-app (code identity "Beamer Dev", self-signed, no
  Team ID), not per-port, so any extra TCP/UDP listener inside the same signed Beamer process is
  already allowed; an ad-hoc rebuild changes identity and may prompt again. Local Network privacy
  (`NSLocalNetworkUsageDescription`) is per app too and already granted.
- macOS 27 tunnel fallback (`Beamer Tunnel.command`, SSH -L to 24822) carries **TCP only**: when a
  link runs through it, UDP media cannot - a reason to keep an in-link fallback.

## 7. Mac Info.plist and entitlements

- **Info.plist** is generated by py2app from the `plist` dict in `mac_app/setup.py:89-104`
  (already has `NSLocalNetworkUsageDescription` at :101). Add `NSMicrophoneUsageDescription` and
  `NSAudioCaptureUsageDescription` there. `build_dmg.sh:111-121` post-edits it with PlistBuddy
  (deletes `PythonExecutable`) - another place to inject keys if needed.
- **Entitlements**: `mac_app/entitlements.plist` (only `cs.allow-unsigned-executable-memory`),
  applied by `sign_app()` in `mac_app/build_dmg.sh:169-193` to the inner `python` and the bundle;
  non-Team-ID builds copy it and add `disable-library-validation` (:176-180). Signing uses the
  **hardened runtime** (`--options runtime`), so `com.apple.security.device.audio-input` must be
  added to `entitlements.plist` or mic capture is silently denied. Not sandboxed.
- `Install-Beamer-Mac.command:28-38` resets TCC (`Accessibility ListenEvent LocalNetwork`) for
  ad-hoc builds; add `Microphone` (and audio capture service) to that list later.

## 8. Where an audio page plugs into the UI

- Mac: page registry `mac_app/pages.py:18-34` (key, name, SF Symbol, purpose); builders map
  `mac_app/kvm_bridge_app.py:1357-1375` (add `"audio": self._audio_page`); View menu auto-fills
  from `pages.PAGES` (`kvm_bridge_app.py:940`). Machine list/rows: `mac_app/machines_panel.py`;
  the screens map: `mac_app/diagram.py` (Crossing page layers) - device badges per tile could go there.
- Windows/Linux: `win_app/pages_win.py:18-34`; builders `win_app/kvm_bridge_win.py:880-890`;
  per-peer row words `win_app/peers_view.py`; map `win_app/diagram.py`.

## 9. Recommendation

| | (A) frames inside the TCP link | (B) separate UDP media, keyed + negotiated over the link | (C) separate TCP |
|---|---|---|---|
| Input latency | Shares one FIFO and one TCP stream: audio bytes queue ahead of keystrokes; TCP loss stalls both | Input link unchanged | Separate stream, but same NIC queues |
| Audio quality | RTO stalls (200-300 ms) need a deep jitter buffer | Loss -> Opus PLC/FEC, low fixed jitter buffer (20-60 ms) | Same RTO stalls as A |
| Link safety | Interacts with ack starvation (`receiver.py:1003-1020`), 512 outbox cap, 1.5 s ack watchdog, serialised dispatch | None | Must not reuse 24820 handshake as-is: step 10 would close the input link |
| Old peers | Safe if cap-gated | Safe if cap-gated | Safe if cap-gated, new port |
| Firewall | Nothing new | PC: no rule if the PC sends first (stateful), else new `Beamer Media (UDP-In)` rule; Mac: already allowed per-app | New TCP rule on PC, or PC always dials |
| Tunnel/Tailscale | Works everywhere the link works | Fails over the SSH tunnel; Tailscale OK (keep datagrams < 1200 B) | Tunnel needs a second forward |
| Complexity | Lowest | Medium (packet AEAD, replay window, punch/keepalive, path pick) | Medium-high, worst of both |

**Recommend B, with A as a low-bitrate fallback.**
- Control plane on the existing link: one new cap `audio`; messages `audio_hello {v, codecs,
  udp_port}`, `audio_devices`, `audio_open/close {stream, key, dir, codec, frame_ms}`; sent on the
  link this machine opened when up, else the other (existing "which link" rule, `WIRE.md:612-614`).
- Media: one UDP socket per app on a fixed port (suggest 24823; 24822 is the Mac tunnel, 24821 is
  pairing), destination = the peer's link address (`host`, or the inbound link's source).
  Both ends send authenticated probes at stream start (every 200 ms until one arrives): the PC's own
  outbound probe opens its stateful firewall, the Mac listener is already allowed, so **no new
  firewall rule** is strictly needed; add `Beamer Media (UDP-In)` to `firewall_win.py`/installer
  anyway for robustness. Optionally let the Mac always originate (it's the side whose user cares).
- Opus 48 kHz, 10-20 ms frames, inband FEC; per-packet ChaCha20-Poly1305 with key from
  `audio_open` (in-band random key) and 64-bit counter nonce; replay window; keepalive 1/s;
  declare the stream dead after ~3 s and fall back.
- Fallback A: if no UDP packet arrives in ~2 s (tunnel, strict firewall), carry the same Opus
  frames as `audio_frame {stream, seq, b64}` on the link at <= 32 kb/s, handlers only enqueue.
- Keep all audio callbacks off the link threads (enqueue only) and do encode/decode in C (ctypes
  calls release the GIL) so the GIL does not add input jitter.
- Leave the Linux peer alone: never bump the wire version, never exceed 16 caps, never send audio
  types to a peer without `audio`.
