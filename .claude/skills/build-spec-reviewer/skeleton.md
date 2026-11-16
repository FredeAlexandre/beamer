# Reviewing a skeleton

A skeleton is everything a test agent sees of its crate. `cargo xtask blind` exports it with
every body as `todo!()`, and the agent writes the tests for the crate's rules against that export
alone. Review it as that agent: from these items, can you test every rule they cite?

## Every skeleton and API PR

- **Public API only.** Public items with `todo!()` bodies, and private items only where the
  public ones need them to compile. No logic.
- **Each item cites its rules.** Every public item carries `// r[impl <id>]` for each rule it
  serves, and serves each rule it cites. An item that serves no rule is a finding: either a rule
  is missing, or the item is.
- **Testable.** For each cited rule, a test through the highest seam that reaches it (#24's
  Testing Decisions) can drive the behaviour and observe the outcome through public items. A rule
  whose outcome no public item exposes is a finding.
- **Covered.** `tracey query uncovered` lists the rules no `impl` reference cites. Each rule of
  the crate's domains is cited here, or the PR names the crate that serves it.
- **The task's criteria hold**: its dependencies and sans-I/O shape (`handle(event, now)`,
  `poll()`, `next_deadline()`, time and randomness injected) as the issue gives them,
  `cargo xtask deps` passes, and the crate builds in a blind export:
  `cargo xtask blind <dir>`, then `cargo build -p <crate>` in `<dir>`. A body `blind` keeps
  (it warns, naming each) is code the test agent sees, so it is a finding unless it holds no
  logic.

## A `build:api` PR

- **One change, on its own.** The PR changes one item's signature and its rule citations: no
  bodies, no tests, no spec. It is the only open `build:api` PR on its crate:
  `gh pr list -R FredeAlexandre/beamer --base rust --search 'API: <crate>'`.
- **Needed.** It names the rule ids that need the change, and each is one the old API could not
  meet, as its issue explains.

## The contracts

`beamer-types`, `beamer-core`'s `Event` and `Command`, and the traits of `beamer-platform` are
Alex's to approve, and so is a `build:api` change to one of them. Their task carries `needs:alex`
from the skeleton writer. Review the rest of the PR (`Core`, the fakes, other items) and leave the
contract's items unjudged. When the rest passes, the verdict is "waits for Alex".
