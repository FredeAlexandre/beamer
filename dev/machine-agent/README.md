# Machine agent

A dev-only program that posts real OS input (pointer, buttons, keys, wheel) for the `machine` test
tier, which drives the real Beamer apps only from outside (ADR 0005). The harness talks to it
over [`PROTOCOL.md`](PROTOCOL.md). It replaces the Python app's `pcagent.py`.

It never ships. It is its own Cargo workspace, so no crate of the app depends on it, no blind
export holds it and no bundle or install contains it (`xtask/tests/layout.rs` keeps it that way).

This directory holds both: the Mac agent (#47, `src/mac.rs`) and the PC agent (#48,
`src/pc/`), one program that serves the same protocol through each OS's own `Desktop`.

## On the Mac

```sh
dev/machine-agent/check.sh     # rebuild, sign, start, move the pointer, type a key: PASS or why not
dev/machine-agent/bundle.sh    # rebuild, sign "Beamer Dev" and install only
open -g ~/Library/Developer/Beamer/Beamer\ Machine\ Agent.app   # start it (the harness does this)
```

`bundle.sh` installs **Beamer Machine Agent.app** (`uk.co.kalkman.beamer.dev.machine-agent`) in
`~/Library/Developer/Beamer/`, away from `/Applications` and Beamer's own bundle. It signs with the
"Beamer Dev" identity and refuses to install an ad-hoc build. Its log is
`~/Library/Logs/Beamer/machine-agent.log`.

Start it only with `open`. launchd then starts it, so macOS holds the agent itself responsible for
the input it posts, and checks the agent's own Accessibility grant. Its binary run from a
terminal would borrow the terminal's grant instead, the very thing ADR 0005 rules out.

Send it requests by hand with the binary's client mode, which needs no grant:

```sh
A=~/Library/Developer/Beamer/Beamer\ Machine\ Agent.app/Contents/MacOS/machine-agent
"$A" send '{"op":"hello"}' '{"op":"move","by":[40,0]}' '{"op":"key","key":"KeyA","action":"press"}'
```

### One-time grant

macOS lets the agent post input only once Alex switches it on under Accessibility. The grant is
tied to the bundle id and the "Beamer Dev" certificate (its designated requirement), not to one
binary, so it survives every rebuild.

1. Run `dev/machine-agent/check.sh`. On the first run it stops at "macOS does not trust the agent
   yet", and macOS shows "Beamer Machine Agent would like to control this computer".
2. Click **Open System Settings**, or go to System Settings > Privacy & Security > Accessibility.
   Switch **Beamer Machine Agent** on. If it is not listed, click **+**, press ⌘⇧G, enter
   `~/Library/Developer/Beamer/`, and choose `Beamer Machine Agent.app`.
3. Run `dev/machine-agent/check.sh` again. It rebuilds and re-signs the agent first, so its PASS
   also shows that the grant survived a rebuild.

If the "Beamer Dev" certificate is ever replaced, the grant no longer matches: remove the old
entry with **−** and grant it again.

## On the PC

```sh
dev/machine-agent/check-pc.sh   # from the Mac: ship HEAD, build, install, start, move the pointer, type a key
```

`check-pc.sh` copies this commit's `dev/machine-agent` to the PC with `git archive` and `scp`,
into `%LOCALAPPDATA%\BeamerDev\machine-agent-src`, so the PC's clone is left alone. There it runs
`install.ps1`, which builds the agent and installs it into `%LOCALAPPDATA%\BeamerDev\machine-agent\`.
That is away from Beamer's own install in `%LOCALAPPDATA%\Beamer\`. `install.ps1` also registers
the scheduled task **BeamerDev-MachineAgent**, which `deploy.sh` never mistakes for Beamer's, and
prints the install directory. It can be run on the PC from any checkout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File dev\machine-agent\install.ps1
Start-ScheduledTask -TaskName BeamerDev-MachineAgent      # start it (the harness does this)
```

Start it only through its task. Anything started over SSH runs in session 0, which has no
desktop, so Windows takes none of its input: its `hello` says `"trusted": false` and every input
request is refused with `not-trusted`. The task is an Interactive one for the user who installed
it, at the normal (not elevated) level, with no trigger. It runs only when started, and only
while that user is signed in at the PC. It runs `machine-agent-windowless.exe`, the same program
built without a console, so no window opens on the desktop. Its log is
`%LOCALAPPDATA%\BeamerDev\machine-agent\machine-agent.log`.

`trusted` is also `false` while the PC is locked. Windows drops input posted to an elevated
window (UIPI) without a word, so a test keeps elevated windows out of the way.

The harness reaches the agent through the tunnel in `PROTOCOL.md`. With a shared `ControlMaster`
in `~/.ssh/config`, as `lebig` has, give the tunnel its own connection (`ssh -N -S none -L ...`).
Otherwise the forward is handed to the master and outlives the harness.

Send it requests by hand from the Mac, through that tunnel:

```sh
ssh -N -S none -L 24871:127.0.0.1:24870 lebig &
dev/machine-agent/target/release/machine-agent send --port 24871 '{"op":"hello"}' '{"op":"move","by":[40,0]}'
```

The agent posts each move as one SendInput event. When the pointer lands where the move asks, the
event is absolute, which puts it on the exact physical pixel. When an edge holds the pointer, the
event is relative and carries the whole push: Beamer's Windows capture measures a push from Raw
Input's relative counts, which an absolute move does not carry. Windows applies its pointer speed
and acceleration to a relative move, so put the pointer at the edge with `to` before pushing.
Keys go by scan code, so they land by position on the PC's AZERTY layout. `key-state` reads the
key back through the virtual key that layout gives the scan code.

## Tests

```sh
cargo test --manifest-path dev/machine-agent/Cargo.toml
```

They speak the protocol over a real loopback connection to an agent whose desktop is a fake, and
they pass on both machines. What the real desktops do, `check.sh` checks on the Mac and
`check-pc.sh` checks on the PC.
