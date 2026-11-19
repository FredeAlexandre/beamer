# Runtime

What runs the core: the engine that hands `Core` its time, its datagrams and everything the other
threads produce, and carries out the commands it gives back; the data folder where the store and
this machine's own settings are kept; the log and the crash log; and the heartbeat the watchdog
reads. Decided in #16 and ADR 0004 (one process, one engine thread, no async runtime), #21 (the
watchdog), #11 (F35 and F19) and ADR 0005 (the log the machine tier reads). Mined from the Python
app at `archive/python`: `core/crash_log.py`, the logging and settings-file code of both apps
(`mac_app/kvm_bridge_app.py`, `mac_app/settings_store.py`, `win_app/kvm_bridge_win.py`,
`win_app/app_config.py`), and their tests.

The other domains decide what the runtime carries out, and this file does not repeat them:

- **Deadlines** are theirs, such as the link's keep-alive and idle timeout (`link.alive.keep-alive`,
  `link.alive.idle`), the beacon (`discovery.beacon.every-two-seconds`) and a take's answer
  (`driving.take.no-answer`).
- **The socket**: one UDP port, 24820, for links, pairing and the beacon
  (`link.transport.port`, `pairing.connection.alpn`, `discovery.beacon.bytes`), and what happens
  when it is taken (`link.transport.port-taken`, `discovery.beacon.port-failure`).
- **The capture word**: what it holds, and that capture decides from it alone, never waiting on the
  rest of Beamer (`input.capture.word`).
- **Platform calls**: that none makes the rest of Beamer wait on the OS
  (`platform.capability.never-waits`), and the priority capture and the engine run at
  (`platform.tuning.top-priority`).
- **What the data files hold**: the store and this machine's own settings (`spec/store.md`,
  `spec/settings.md`), and what links do while the peer list cannot be read
  (`pairing.peers.unreadable`).
- **Log lines**: each domain's own (`link.log.*`, `driving.log.*`, `crossing.log.*`), and the events
  the machine tier reads (`spec/observability.md`).
- **Quit and crashes**: what Quit sends before Beamer ends (`driving.home.quit`, `link.send.quit`),
  the pointer shown again after a crash (`crossing.pointer.crash-mac`,
  `crossing.pointer.crash-windows`), and what the watchdog does with the heartbeat and with "bye"
  (`spec/app.md`).

## Engine

r[runtime.engine.deadline-not-early]
`Core` is told a deadline it asked for has come no earlier than that deadline.

r[runtime.engine.deadline-earlier]
A deadline `Core` asks for while the engine waits for a later one is kept: `Core` is told of it at
its time, not at the later one.

r[runtime.engine.time-only-forward]
Each time the engine gives `Core` is no earlier than the one before, whatever happens to the
machine's clock.

r[runtime.engine.wake]
An event from the UI, a capture thread, a platform worker or a media thread reaches `Core` while
no deadline is due and no datagram arrives.

r[runtime.engine.order-per-origin]
The events that come from one place (the socket, a capture thread, the UI, a platform worker, a
media thread) reach `Core` in the order that place produced them.

r[runtime.engine.commands-in-order]
Commands `Core` gives for one destination (the socket, injection, the UI, one platform worker, one
data file) are carried out in the order `Core` gave them.

r[runtime.engine.injection-inline]
An injection `Core` asks for is posted to the OS before `Core` is given its next event.

r[runtime.engine.call-outcome]
The outcome of a platform call that blocks (a DDC/CI exchange, a permission probe, a file write)
reaches `Core` as an event when the call ends.

r[runtime.engine.socket]
The engine sends every datagram `Core` gives it from the one UDP socket bound on the link's port,
and hands `Core` every datagram that socket receives, with the address it came from.

## Capture word

r[runtime.capture-word.published]
When a step of `Core` changes the capture word, the new word is published before the engine gives
`Core` its next event: every pass-or-swallow decision from then on reads it.

## UI

r[runtime.ui.newest-snapshot]
The UI always reads the newest snapshot the engine has published. Snapshots published between two
of its reads are skipped, never queued.

r[runtime.ui.woken-on-change]
The UI is woken when the engine publishes a snapshot, and is not woken while none is published.

r[runtime.ui.frame-values]
A value the UI draws every frame, such as push pressure or the pointer overlay's position, is
readable at its newest without waiting for a snapshot.

r[runtime.ui.commands]
Every command the UI sends reaches `Core`, in the order the UI sent it.

## Media rings

r[runtime.ring.never-waits]
A media thread never waits on the engine, nor the engine on a media thread: a datagram that finds
its ring full is dropped, and the drops are counted.

r[runtime.ring.in-order]
Datagrams that pass through a ring arrive unchanged and in the order they were put in.

## Heartbeat

r[runtime.heartbeat.every-second]
While the engine runs, it writes the line `beat` to the watchdog's pipe once a second.

r[runtime.heartbeat.from-the-engine]
The heartbeat comes from the engine itself: while one step of `Core`, or anything else on the
engine, runs longer than a second, no `beat` is written.

r[runtime.heartbeat.pipe-gone]
When the watchdog's pipe is closed or full, writing the heartbeat neither stops nor delays the
engine.

## Data files

> r[runtime.files.folder]
> Beamer keeps two data files, `store.json` and `local.json`, in its data folder:
> `~/Library/Application Support/Beamer/` on the Mac and `%LOCALAPPDATA%\Beamer\` on the PC.
> `store.json` is this machine's copy of the store, and `local.json` holds the settings that never
> leave this machine.
>
> - parity: Python kept one file, `settings.json`, in the same folder (#16: the store's copy and this machine's own settings are two files)

> r[runtime.files.fresh-start]
> Beamer reads, writes, moves and deletes no file in the data folder but its two data files and
> their temporary files: the Python app's `settings.json` and `config.json` are left as they are.
>
> - parity: Python read and migrated 1.4.x's `config.json` and kept a backup before the profiles upgrade (#11: F19 without the 1.4.x migration; #21: a fresh start that never reads the Python app's files)

r[runtime.files.format]
Each data file is one JSON object in UTF-8, indented by two spaces, ending with a newline.

r[runtime.files.mac-private]
On the Mac the data folder is readable only by its user (mode `0700`), and so is each data file
(mode `0600`).

r[runtime.files.unicode-path]
The data files are written and loaded back intact when the data folder's path holds characters
outside ASCII, such as a user folder named `José`.

r[runtime.files.round-trip]
What Beamer writes to a data file, it loads back unchanged at its next start.

> r[runtime.files.first-run]
> When neither data file is on disk at startup, Beamer starts from the defaults, logs one line saying
> it is a first run, and creates the data folder and both data files, holding the defaults, before
> `Core` is given its first event.
>
> - parity: Python also wrote its settings file at once on a first run, but logged no line for it: the Mac's "first run: no settings yet" came only when loading failed and no file was there (a first run is named in the log, for whoever reads a report)

> r[runtime.files.one-missing]
> When one data file is on disk at startup and the other is not, it is not a first run: no first-run
> line is logged, the missing file starts from its defaults and is created holding them before
> `Core` is given its first event, and the file on disk is loaded, or refused as
> `runtime.files.refused-defaults` says.
>
> - parity: Python kept one file, so it could not be missing beside another (#16: the store's copy and this machine's own settings are two files)

> r[runtime.files.any-bytes]
> Whatever bytes a data file holds, Beamer either loads it or refuses it each time it reads it, and
> never crashes on it.
>
> - fuzz: store-file

> r[runtime.files.refused-defaults]
> A data file that is on disk but is refused at startup (damaged, or not a version this build reads)
> is not a first run: Beamer starts from the defaults for that file alone, logs a line naming the
> file and why it was refused, and leaves the file as `runtime.files.refused-kept` says. No
> first-run line is logged, and the file is not created again.
>
> - parity: Python's PC started with no settings at all, its window showing none, and only the Mac started from the defaults (one rule for both OSes, from the Mac's)

r[runtime.files.refused-kept]
A data file refused when it is read, at startup or at Reload configuration
(`viewmodel.notice.reload`), is not written over until a later read of it succeeds, so it stays as
it was for the user to mend or move aside. No change, Quit or retry writes it meanwhile.

> r[runtime.files.debounced]
> A change to the store or to `local.json` is written to its file 300 ms after the last change to
> that file, unless `runtime.files.max-wait` writes it sooner: a burst of changes, each within
> 300 ms of the one before, that ends within 1.7 s of its first change is written once.
>
> - parity: Python's PC wrote a switched toggle at once, without the 300 ms wait its other changes had (#16: `Core` debounces its writes, every change alike)

> r[runtime.files.max-wait]
> No change to a data file waits more than 2 s to be written. While changes keep coming, the file is
> written 2 s after its first change not yet written, and the next change starts a new wait: a
> burst of changes 0.1 s apart that lasts 3 s is written twice, at 2 s and 300 ms after its last
> change.
>
> - parity: Python had no maximum: a change waited as long as further changes kept coming within 300 ms (2 s is a value no decision sets; #16 decides only that writes are debounced)

> r[runtime.files.quit]
> A change not yet written when Beamer quits is written before the process ends, unless its file is
> refused (`runtime.files.refused-kept`).
>
> - parity: Python dropped a change still waiting out its 300 ms when it quit: the Mac's Quit never wrote it, and the PC's stopped its pending timer (a change made just before Quit is kept)

r[runtime.files.newest-wins]
When several writes of one data file are pending, the file ends holding the newest content.

r[runtime.files.whole]
A data file is replaced whole: a process killed at any moment of a write leaves the previous file or
the new one, never a partial or empty file.

> r[runtime.files.leftovers]
> A temporary file that an interrupted write left in the data folder is never loaded, and is gone
> after the next write of its data file.
>
> - parity: Python removed a temporary file only when its own write failed, so one left by a killed process stayed for good (a leftover is cleared at the next write)

> r[runtime.files.write-failed]
> When a data file cannot be written (the disk is full, the folder is not writable), the file on
> disk stays as it was, a line naming the file and the OS's error is logged, and `Core` is told of
> the failure.
>
> - parity: Python's PC logged the failure ("Setting could not be saved") and showed a "Save failed" dialog only for the Connection page's port, and the Mac showed the error as a note in its window (#16: errors that aren't bugs are values, logged and passed on, and what a screen says of them is `spec/viewmodel.md`'s)

r[runtime.files.write-retried]
After a failed write, the newest content of that data file is tried again with its next change and
at Quit.

## Log

r[runtime.log.path]
Beamer logs to `~/Library/Logs/Beamer/Beamer.log` on the Mac and to
`%LOCALAPPDATA%\Beamer\Beamer.log` on the PC.

> r[runtime.log.fallback]
> When the log's folder cannot be created or written, Beamer logs to `Beamer\Beamer.log` in the
> OS's temporary folder, and when that fails too, it runs on without a log file.
>
> - parity: on the Mac, Python failed to start when it could not create its log folder (one rule for both OSes, from the PC's)

r[runtime.log.mac-private]
On the Mac the log's folder is readable only by its user (mode `0700`).

r[runtime.log.appends]
Each start of Beamer adds to the log already there, so the lines of the run before a restart are
kept.

> r[runtime.log.rotate]
> When a line would take the log past 1 MiB (1,048,576 bytes), the log becomes `Beamer.log.1`, the
> older ones move up to `.2` and `.3`, the one past `.3` is deleted, and a new `Beamer.log` starts.
>
> - parity: Python rotated the PC's log at 1,000,000 bytes and the Mac's at 1 MiB (one size on both OSes)

> r[runtime.log.json-lines]
> Each line of the log is one JSON object holding at least `timestamp` (RFC 3339, in UTC), `level`,
> `thread` and `message`, with the line's other fields as further keys.
>
> - parity: Python wrote plain text lines, "time LEVEL thread message" (ADR 0005: the machine tier reads JSON lines)

r[runtime.log.utf-8]
The log is UTF-8: text outside ASCII, such as a path holding `Élodie`, reads back from the file as
it was logged.

r[runtime.log.thread-names]
Every thread Beamer starts has a name, and each line it logs carries that name as its `thread`.

> r[runtime.log.level]
> Lines at info level and above are logged. With `BEAMER_DEBUG` set in its environment, Beamer
> logs debug lines too.
>
> - parity: Python honoured `BEAMER_DEBUG` on the PC only (one rule for both OSes)

> r[runtime.log.start-line]
> The first line of each run names Beamer's version, the OS and its version, and the log's path.
>
> - parity: Python's PC logged "Starting Beamer Windows receiver" and the log's path, and the Mac logged neither (one start line on both OSes, naming the version the machine tier's reports need, ADR 0005)

r[runtime.log.written-within-a-second]
A line logged and not dropped under `runtime.log.never-waits` is in the log file within 1 s of
being logged.

> r[runtime.log.never-waits]
> A thread that logs never waits for the log file, except a panicking thread writing its own line
> (`runtime.crash.logged`). When lines come faster than the file takes them, the lines that find no
> room are dropped, and once there is room again one line says how many were dropped. No other line
> is ever dropped.
>
> - parity: Python wrote each line on the thread that logged it, which waited for the file, and dropped none (#16: real-time threads never take locks, and their lines reach the logging worker through a ring)

## Crash

> r[runtime.crash.ends-the-process]
> A panic on any thread, including inside a callback the OS runs, ends the whole process.
>
> - parity: Python logged a worker thread's uncaught exception and ran on without that thread (ADR 0004: a panic aborts the process, and the watchdog restarts it)

r[runtime.crash.logged]
Before the process ends, a panic logs an error line naming the thread, the panic's message and the
place in the source it came from. This line is never dropped.

r[runtime.crash.log-kept]
When a panic ends the process, the log file holds every line logged before it and not dropped
under `runtime.log.never-waits`, and the panic's own line (`runtime.crash.logged`).

> r[runtime.crash.crash-log]
> A panic adds an entry to `crash.log`, in the log's folder: the time, Beamer's version, the
> thread's name, the panic's message, its place in the source, and the panicking thread's
> backtrace. Earlier entries are kept.
>
> - parity: Python wrote `crash.log` on the Mac only, and only on a fatal signal, with every thread's Python stack (ADR 0004: a panic hook writes the crash log on both OSes)

> r[runtime.crash.fatal-signal]
> A crash that is not a panic (a fatal signal on the Mac, an unhandled exception on the PC) adds an
> entry to `crash.log` naming the signal or exception and the thread it hit, then ends the process
> as the OS would, so its own crash report is still made.
>
> - parity: Python wrote every thread's Python stack on a fatal signal on the Mac, and nothing on the PC: it left faulthandler off there because it also reports access violations that are then handled (a Rust app has no interpreter stack, and an unhandled-exception filter on the PC sees only the crashes no handler caught)
