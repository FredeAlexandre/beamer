# Machine agent

A dev-only program that posts real OS input (pointer, buttons, keys, wheel) for the `machine` test
tier, which drives the real Beamer apps only from outside (ADR 0005). The harness talks to it
over [`PROTOCOL.md`](PROTOCOL.md). It replaces the Python app's `pcagent.py`.

It never ships. It is its own Cargo workspace, so no crate of the app depends on it, no blind
export holds it and no bundle or install contains it (`xtask/tests/layout.rs` keeps it that way).

This directory holds the Mac agent (#47). The PC agent (#48) speaks the same protocol.

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

## Tests

```sh
cargo test --manifest-path dev/machine-agent/Cargo.toml
```

They speak the protocol over a real loopback connection to an agent whose desktop is a fake.
What the real desktop does, `check.sh` checks on the Mac.
