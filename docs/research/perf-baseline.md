# Performance baseline of the Python app on the Mac and the PC

Ticket: [Performance baseline of the Python app on the Mac and the PC](https://github.com/FredeAlexandre/beamer/issues/4),
part of the map [Wayfinder: port Beamer to Rust](https://github.com/FredeAlexandre/beamer/issues/1).
Measured 2026-11-13 and 2026-11-16. Tools, raw data and outputs are in [`perf-baseline/`](perf-baseline/).

## Setup

| | Mac | PC |
|---|---|---|
| Machine | MacBook Air `Mac17,4`, Apple M5, macOS 27.2.1 | Ryzen 5 7600X, Windows 11 build 26200 |
| Network | Wi-Fi (`en0`) | Wi-Fi, MediaTek RZ616 Wi-Fi 6E, 160 MHz, link 2.4 Gbps, power saving Auto |
| Build | Beamer 1.5.19 + fork, commit `545bcac`, installed 2026-11-12 21:53 | same commit, installed 2026-11-12 21:54 |

Both machines are on the same home Wi-Fi (5 GHz, channel 52). Nothing was measured wired. `main`
has had only docs commits since `545bcac`, so this is today's app.

## Headline numbers

| What | p50 | p95 | p99 / max | n | Method |
|---|---|---|---|---|---|
| Link ping RTT (app level, both links) | 6 ms | 9–10 ms | p99 24–32 ms, max 41 ms (PC) / 880 ms (Mac, once) | ~21,000 pings | Beamer's own `ping_rtt` (log) |
| Input, PC drives Mac: send → injected → ACK back, minus ACK hold | 6.3 ms | 10.5 ms in a typical minute, 40 ms in the worst 5% of minutes | worst minute's max 409 ms | 138,954 events, 181 minutes | Beamer's input ACK round trip (log) |
| Input, Mac drives PC: same | 6.1 ms | 9.1 ms | max 14.6 ms | 102 events, 1 minute | same |
| Crossing, breakthrough → pointer placed on the other machine | ~4.5 ms | ~18 ms | max ~20 ms | 52 out-and-back pairs | both logs, clock offset cancelled |
| Clipboard text, cross → ready to paste | 8–36 ms | — | — | 5 (3 PC→Mac, 2 Mac→PC) | clipboard change counters + live clock link |
| Clipboard image, cross → ready to paste | 113–345 ms | — | — | 3 (2 PC→Mac, 1 Mac→PC); 1 image skipped | same |
| Audio one way (system sound, PC → Mac speakers) | 65.1 ms | 84.9 ms | — | 31 min soak | fork's WP16 harness, not the shipped build |
| Audio one way (Mac mic → PC app via CABLE) | 172.4 ms (120.3 ms low latency) | 185.0 ms (166.7) | — | WP16 | same; ran in a fallback mode, fixed later, never re-measured |
| Camera glass to glass | **not measured** | | | | prototype only: 57.5 / 60.3 / 65.3 ms p50/p95/p99 synthetic source → Chrome, 720p |
| CPU, Beamer, normal use (5 s samples) | Mac 2.2–2.4 %, PC 1.9–2.5 % of a core | Mac 3–7 %, PC 4 % | crossing + clipboard bursts: Mac 11 %, PC 49 % | ~75 samples per machine | `ps` / `Get-Process` sampler |
| CPU, Mac Beamer, 3-day average | 11.8 % of a core | | | 71 h | cumulative CPU time, same process |
| Memory, Beamer | Mac RSS **678 MB and growing**; PC private 88 MB (211 MB peak during an image copy) | | | | sampler |
| Memory, audio helpers | Mac `beamer-audio` 13 MB, PC 14 MB | | | | sampler |

## Input end to end

**What is measured.** Each input message carries a sequence number. The receiver acknowledges the
highest sequence it has *dealt with*: decoded, dispatched and injected into the OS. The ack carries
`held_us`, how long the receiver waited after injecting before it sent the ack (acks are coalesced).
The sender logs, once a minute, the median, p95 and largest of `now − sent − held` (`core/link.py:925`,
`core/receiver.py:1044`). That is the full Beamer path from writing the input to the socket, through
injection on the other machine, plus the return leg of the network. It excludes the OS before Beamer's
hook sees the event and after injection (HID → hook, inject → screen). The Rust app uses the same OS
APIs, so those parts are not Beamer's to budget.

- PC drives Mac (Alex's usual direction): 181 one-minute windows, 138,954 events on 2026-11-16.
  Sample-weighted median **6.3 ms**. Within a minute, the p95 is 10.5 ms in a typical minute, and
  39.8 ms in the worst 5 % of minutes (worst 1 %: 77 ms). The per-minute largest has a median of
  38 ms and a maximum of 409 ms.
- Mac drives PC: one minute (102 events) on 2026-11-16, median 6.1 ms, p95 9.1 ms, largest 14.6 ms.
  The 2.5 days of logs held no other samples of this direction, because Alex drives from the PC.
- The one-way trip from injection to screen is roughly half of that RTT plus the sender's own stages,
  so about **3–4 ms at p50**. The sender's stages are tiny: PC `input_queue` mean 0.03 ms and
  `write` 0.07 ms. The receiver's stages are too: Mac `decode` 0.09 ms, `dispatch` 0.22 ms and
  `inject` 0.18 ms (mean). The network dominates everything.
- p99 is not available across all events: the app logs only the median, p95 and largest per minute.
  To get a true p99, log the samples themselves (see "For the protocol ticket").

**Wi-Fi tail.** The tails (p95 of bad minutes, 40–77 ms; largest, up to 409 ms) come from the radio.
The audio work traced Mac → PC spikes to the PC adapter's power saving (`dev/audio/STATUS.md`,
item 4). The receiver's `ack_due_lag` and `writer_wait_lag` maxima (139 ms, 358 ms) are Python
threads waking late, and they show up in the same minutes.

## Crossing

**What is measured.** The time from the moment Beamer decides the pointer has pushed through an edge
to the moment the other machine has placed the cursor. The edge resistance dwell, which the user
controls, comes before this window. Log points: PC "input is on …" (`win_app/sender.py:1521`) → Mac
"… pointer arrived" (`core/receiver.py:1952`), and Mac "pushed through the zone …"
(`core/receiver.py:1533`) → PC "input returned to this PC" (`win_app/sender.py:1534`).

**Clock problem.** The PC's wall clock wanders against the Mac's: −225 ms on 2026-11-13 and +35 ms on
2026-11-16 (Windows time sync). A single offset gives impossible negative crossings. Each crossing
out is therefore summed with the crossing back that follows it, a median of 11 s later, which cancels
the offset: `(mac − pc) + (pc − mac) = go + return`.

**Result.** 52 pairs on 2026-11-16: go + return p50 9 ms, p95 37 ms, max 40 ms. So **one crossing
takes about 4.5 ms at p50 and 18 ms at p95**, if the two directions are about equal. n = 52 is too
small for a p99.

## Clipboard

Beamer does not send the clipboard when you copy. It sends it when input crosses: the new owner
gets the clipboard if it changed, and the machine that let go may still send it for up to 10 s
(`core/owner.py`, `CLIPBOARD_AFTER`). So the latency that matters to a user is **cross, then ready to
paste**. Measured on 2026-11-16 13:51–13:54 with Alex copying and crossing. Change counters were
polled every 1 ms on both sides (Mac `NSPasteboard.changeCount`; PC `GetClipboardSequenceNumber`,
run in the desktop session through a scheduled task). The sender's crossing time was mapped onto
the receiver's clock with the offset measured every second over SSH (error ≤ RTT/2 ≈ 3 ms).

| Direction | Content | Cross → ready (ms) |
|---|---|---|
| PC → Mac | text | 8, 12, 36 |
| PC → Mac | image (copied from a browser) | 283, 345 |
| PC → Mac | image | **not sent**: the PC logged "Local clipboard image is invalid or too large; skipping the image" (13:53:12), 3.5 s after the crossing |
| Mac → PC | text | 16, 20 |
| Mac → PC | image (screen region) | 113 |

These samples are too few for percentiles. They give the order of magnitude: tens of ms for text
and hundreds for images, so image cost is encoding and size, not the network. Image sizes were not
recorded.

## Audio and camera

The fork's own measurements are collected, with sources, in
[`perf-baseline/audio-camera-existing.md`](perf-baseline/audio-camera-existing.md). In short:

- **Audio one way**, last full run (WP16, commit `44e7291`, Wi-Fi):
  - PC mic → Mac app: 41–56 ms p50.
  - PC sound → Mac speakers: 65.1 / 84.9 ms (p50/p95, 31 min soak).
  - Mac sound → PC headset: 69.5 / 80.2 ms.
  - Mac mic → PC app via CABLE: 172.4 / 185.0 ms, or 120.3 / 166.7 ms with low latency.

  The shipped build (`7b83e71` and later) includes fixes that were never re-measured end to end. The
  Mac mic path ran in a fallback mode in WP16, and ~110 ms is expected but unmeasured. Every number
  runs from a test source to a digital tap, so there is no true mouth-to-ear figure and no p99.
- **Audio soak**: 0 packets lost over 31 min. Glitches were 22.1/min Mac → PC, inflated by echo
  cancellation on the test source (5.0/min without it), and 1.9/min PC → Mac.
- **Camera**: no glass-to-glass number on the shipped app; that run got 0 frames because of a
  test-source bug. Prototype: 57.5 / 60.3 / 65.3 ms p50/p95/p99 at 720p, and 56.8 / 63.3 / 106.6 ms
  at 1080p, synthetic source → Chrome. Real camera through an agent shell: first frame in 0.72–1.22 s,
  15–16 fps in a dark room.
- **Helper CPU while streaming**: Mac `beamer-audio` 8.0 % of a core during the audio soak (2.1 %
  with the synthetic camera, ~9 % with the real one). PC helper 2.5 %. PC Frame Server 1.4 % at 1080p.

## CPU and memory

Sampled every 5 s on 2026-11-16 13:50–13:56 (`raw/cpumem.out`). Each sample is labelled with who held
the input, read from both logs. "Mixed" means a crossing happened inside the 5 s window, which
covers the clipboard rounds. CPU is % of one core.

| Machine | Process | Local use | PC drives Mac | Mac drives PC | Window with a crossing |
|---|---|---|---|---|---|
| Mac | Beamer | 2.0 % | 2.2 % (p95 7.3) | 2.4 % (p95 3.0) | 6.1 % (p95 11.3) |
| Mac | beamer-audio | 3.0 % | 3.0 % | 3.0 % | 2.8 % |
| Mac | coreaudiod | 1.6 % | 1.4 % | | 1.4 % |
| PC | Beamer | 2.5 % | 1.9 % (p95 4.1) | 2.2 % (p95 3.7) | 3.4 % (**p95 49.3**) |
| PC | beamer-audio | 0.6 % | 0.6 % (p95 2.8) | 0.6 % | 1.2 % |

- **Mac Beamer memory grows without bound**: RSS was 147 MB on 2026-11-13 at 14:44, 247 MB 25 minutes
  later, and **678 MB** on 2026-11-16 at 13:56. That is the same process, pid 96569, started 2026-11-12
  21:54, so it averaged +7.5 MB/h with bursts. The audio soak saw 401 → 411 MB. Upstream's closed issue
  57 traced a leak through PyObjC's event-tap trampoline; something still leaks.
- **Mac Beamer CPU over 3 days**: 30,205 CPU-seconds over 71 h, or **11.8 % of a core on average**.
  That is five times the quiet-window figure, so heavier phases (audio, camera, long crossings)
  dominate real use. This average is the fairer baseline for "what Beamer costs".
- The PC peak of 49 % CPU and 211 MB fell in the window of the PC image copy that was then skipped.
- PC Beamer private memory is a steady 88 MB.
- No sample covers the camera streaming through the app, and none covers idle with audio fully off.

## Gaps, and why they don't block the protocol design

- **Camera glass to glass** and **audio after the final fixes** were never measured on the shipped
  build. Both need a physical loop (a screen with a clock in front of the camera, a mic in front of
  a speaker) and Alex at the machines. The prototype and WP16 numbers bound them well enough for
  budgets, which the Rust app will have to meet on its own measurements anyway.
- **True p99 for input**: the logs keep only per-minute summaries.
- **Wired LAN**: everything here is Wi-Fi, which sets the tail. The LAN-first target should state
  whether its budgets assume wired or Wi-Fi.
- **Clipboard** samples are few; enough for order of magnitude, not for percentiles.

## For the protocol ticket

- On Wi-Fi the network RTT (~6 ms p50, ~10 ms p95) is the whole input budget. Beamer's own work
  on each side is well under 1 ms at the mean. Its tail comes from Python threads waking late
  (`ack_due_lag` / `writer_wait_lag` maxima of 76–358 ms) and from the radio. A Rust app can only win
  on that tail and on the clipboard image path, not at the p50.
- Budgets can be set relative to the link: input at no more than the link RTT p50 + 1 ms, and
  crossing at no more than one RTT.
- The measurement points used here (ACK minus hold, crossing pairs, clipboard change counters) are
  cheap to keep. Logging raw samples instead of per-minute summaries would give the `perf.*` rules
  true p99s.
