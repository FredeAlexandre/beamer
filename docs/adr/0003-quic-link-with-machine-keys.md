# The link is QUIC with a key pair per machine, and input rides datagrams with its own resends

The Rust app's link between two paired machines is one QUIC connection (quinn), on one UDP port,
carrying everything both ways: input, control, media and bulk. It replaces the Python app's two TCP
links per pair, the separate UDP media flow and the shared per-pair token. Each machine has its own
Ed25519 key pair. Pairing (CPace with the six-digit code, run inside a `beamer-pair/1` QUIC
connection and bound to both keys and the TLS exporter) pins the other's public key, and links
authenticate by those pinned keys through self-signed certificates and a custom verifier. Input does
not use QUIC streams: it travels as datagrams that carry every input event not yet acknowledged,
with our own acknowledgements and a resend after max(2 × RTT, 5 ms).

The baseline shows Beamer's own work under 1 ms, so the network is the budget, and the one place to
win is the tail: a 409 ms input stall on a 6 ms link. TCP's minimum retransmission timeout (about
210 ms on the Mac, 300 ms on the PC) can explain it. QUIC's probe timeout is about 43 ms on this
path, and redundant input datagrams repair a loss with the next event.

## Considered options

- **Our own protocol over UDP with Noise (`snow`).** It gives full control, but we would own
  reliability, congestion control for 8 MiB images and 5 Mbit/s video, and MTU handling, and every
  part of that needs spec rules, mutation tests and fuzzing. No maintained crate combines Noise with
  reliability.
- **TCP plus UDP, as in the Python app.** It keeps TCP's retransmission floor and two crypto layers.
- **A shared token per pair as a QUIC pre-shared key.** rustls has no external PSK support (rustls
  issue #174, not before 0.24).
- **Input on a top-priority QUIC stream.** It is simpler, but a loss waits for QUIC's probe
  timeout.

## Consequences

- If UDP is blocked, nothing links. There is no TCP fallback. The LAN and Tailscale both pass UDP.
- Anyone who can reach the port can learn a machine's stable public key during the TLS handshake. We
  accept that on a home LAN.
- Datagrams count against QUIC's congestion window. Bulk transfers therefore get a capped
  flow-control window, so they can never fill the window and hold input up.
- The private key is the identity: it is kept in the Keychain on macOS and under DPAPI on Windows,
  and losing it means pairing again.
- Don't "simplify" by moving input onto a stream, or by bringing back a TCP fallback.

Source: the wayfinder ticket "Design the new link protocol" (#14) on FredeAlexandre/beamer.
