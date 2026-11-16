# The machine tier drives the real apps from the outside, never through in-app hooks

The `machine` test tier (`cargo xtask machine`) runs scenarios against the real, deployed apps on
the Mac and `lebig`. It drives them only from the outside. It never uses a test hook, a dev
control socket or a test-only build feature inside the app.

- **Setup**: before the apps start, the harness writes seeded JSON state (pairing, peers,
  settings), so nothing is clicked through the UI.
- **Stimulus**: real OS input. A small dev-only **machine agent** on each machine posts it. On the
  Mac the agent is signed "Beamer Dev", so its Accessibility grant survives rebuilds. On the PC it
  runs in the desktop session through a scheduled task, because SSH lands in session 0.
- **Observation**: the apps' JSON-lines `tracing` logs, with the two machines' timestamps tied
  together by out-and-back pairs, because the PC's clock drifts by hundreds of ms.

The events the harness relies on are spec rules in `spec/observability.md`, and `ci`-tier tests
check that the core emits them. Renaming one is a versioned spec change, never a silent break of
the harness.

The reasoning is the same as ADR 0002's. A hook is a second way into the app that only tests use,
so a green run could prove the hook works rather than the app. The machine tier exists to check
what a person at the desk would get: real input through the real capture path, on the same build
that is deployed.

## Considered options

- **A dev control socket or CLI verbs that inject actions and read state.** This would be easier
  to script and faster to set up, but it adds code that only tests reach, and it bypasses the
  capture and injection paths that most need checking.
- **Granting Accessibility to the terminal that runs the harness.** This is simpler on the Mac,
  but it would hand synthetic input to every agent in that terminal.

## Consequences

- The log becomes a contract: what the harness needs must be logged, and logged under spec ids.
- Setup is limited to what the app's persisted JSON can express. A scenario that needs a state the
  app cannot load from disk must reach it through the UI, which makes it a `human`-tier rule.
- The machine agents are dev tools in `dev/`. They never ship in the app.

Source: the wayfinder ticket "Dev loop and CI for the Rust workspace" (#22) on
FredeAlexandre/beamer.
