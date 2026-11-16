# One process, one engine thread, no async runtime

The Rust app is a single process. It holds the UI, the core, the link, the platform code and the
media engine. Steps that need elevation (firewall repair, the admin-mode relaunch, and later the
GameInput recovery task) run as `--elevated` subcommands of the same binary. The only code outside
that process is what the OS loads itself: the Mac HAL driver inside coreaudiod and the Windows
camera source inside Frame Server.

None of our crates uses an async runtime. Every piece of logic is a sans-I/O state machine
(`handle(event, now)`, `poll() -> Option<command>`, `next_deadline()`), driven by a few dedicated
threads that each block on their own source:

- **The engine thread** owns the UDP socket, quinn-proto and one `Core` value that composes every
  domain state machine. It blocks in `polling` on the socket, a waker and the next deadline.
  Injection runs inline on this thread, because `SendInput` and `CGEventPost` take microseconds.
- **The capture threads** are the Mac event tap's CFRunLoop thread and the Windows hook thread with
  its message loop. They only forward events to the engine. Their one synchronous decision, pass or
  swallow, reads a lock-free **capture word** that the engine republishes whenever crossing state
  changes.
- **The main thread** runs Rune. The engine hands it coalesced snapshots through a latest-value
  slot, plus per-frame values through atomics, and takes commands back over a channel.
- **The media threads** (OS audio callbacks and one encoder per send stream) exchange datagrams with
  the engine through lock-free SPSC rings.
- **The platform workers** take the blocking OS calls (DDC/CI, file writes, permission probes).

Release builds use `panic = "abort"`. A panic hook writes the crash log, and the OS restarts the
app. When the process dies, its hooks die with it, so local input is never stranded, and a peer
that held our input gets it back through the link's idle timeout.

## Considered options

- **tokio with quinn's high-level API.** It means less driver code for us, but executor wake-ups sit
  on the hook-to-wire path, whose budget is 0.5 ms p99, and tests need turmoil and shuttle where a
  virtual clock is enough. The model tests from the test strategy need sans-I/O machines anyway.
- **App plus media helper, as the Python app has it.** It isolates a crash in a C codec, but media
  datagrams would cross a process boundary through shared memory (one more decoder to fuzz), and
  there would be two signed binaries and a subtler TCC attribution. The Python app needed the helper
  for the GIL. The Rust app doesn't.
- **A headless agent plus a UI process.** A crash in Rune could not drop the link, and the core would
  idle at about 10 MB instead of about 50 MB. But the tray must always be there, so two processes
  would always run, joined by an IPC protocol. Because the UI already talks to the core only through
  snapshots and commands, this split stays possible later if Rune's crashes ever bite.
- **One actor per feature, each on its own thread.** It adds channels and nondeterminism and wins
  nothing: the domain work is far under a millisecond.

## Consequences

- Don't add tokio "to simplify networking". An async executor may live inside one platform adapter
  (a later Linux portal backend needs `zbus`/`ashpd`), and it never crosses the platform seam.
- Don't give a domain crate a clock, a thread or a socket: time and randomness come in as arguments.
  This is what lets `beamer-sim` run N machines on a virtual clock.
- No platform adapter may block the engine thread. A call that can take more than microseconds
  goes to the adapter's own worker, and its outcome comes back as an event.
- A UI or codec crash takes the whole app down, and the OS restart brings it back. Surviving a panic
  in process is not attempted.

Source: the wayfinder ticket "Architecture and crate layout of the Rust app" (#16) on
FredeAlexandre/beamer.
