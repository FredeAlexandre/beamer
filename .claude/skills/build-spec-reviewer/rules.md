# Reviewing rules

Every rule the PR adds or changes is **observable**, **traceable** and in the glossary's words
(SKILL.md step 4). Each check below says what passes; a rule that fails one gets a finding naming
the check. The format itself is `spec/README.md`'s: read it first.

## Observable

- **One behaviour.** A rule joining two behaviours that a test could see apart is two rules.
- **Seen from outside.** It names what the user, a peer or the OS sees: an output of a public
  item, an event or command through `Core`, a message on the link, a log line, or for `human`
  tier, something Alex sees, hears or feels. The seams are listed in #24's Testing Decisions.
  Internals (a struct, a thread, a lock, a queue, a function, a retry loop) are how the code
  works, so the rule states the behaviour they produce instead.
- **Decidable.** A test can tell pass from fail. "Quickly", "smoothly", "robustly", "eventually"
  and "as needed" give way to the number, the event or the order that decides them. A budget names
  its percentile and its value.
- **At its tier.** `ci` unless the rule's `tier:` marker says why not, in one line that names what
  hosted runners cannot reach: the real capture path, a cable, a sound. Behaviour across three or
  more machines stays `ci`, through `beamer-sim`. `human` is for what is felt, heard or seen.
- **Behaviour, when it comes from a mutant.** A triage amendment born from a surviving mutant
  states what a user or peer sees, never the mutated code's condition. "Returns early when the
  count is zero" is code; "A clipboard copy of zero bytes sends nothing to the peer" is a rule.

## Traceable

- **Valid.** `cargo xtask spec validate` passes in the review worktree. When the PR bumps a
  version, the stale references the bump creates are the one failure it may carry: the test agent
  and the implementer re-cite them (`spec/README.md`, Versions).
- **Ids keep their meaning.** Each id sits in its domain's file. No id is renamed. A new id has
  never existed: `git log origin/rust --oneline -S 'r[<id>' -- spec/` prints nothing.
- **Versions.** Compare each changed rule's old text and markers with its new ones. A change of
  meaning, a marker included, raises the version by exactly one; a wording fix that keeps the
  meaning keeps the version.
- **A source you can follow,** which says what the rule says:
  - a mined rule: the Python tests the ledger maps to it, and the code at `archive/python` they
    exercise;
  - a rule from a decision: the ticket or ADR, such as #11, #14, #16 or #20, named in the PR or
    the task;
  - an amendment: the triage issue that asked for it, named in the PR.

  A rule that differs from what Python did carries a `parity:` marker stating what Python did and,
  in its final parentheses, why the rule differs. A rule with no source, or one that differs from
  its source without saying so, is a finding.
- **Markers.** A rule about one of the eight state machines names its `model:`. A rule about a
  parser names its `fuzz:` harness. Both tables are in `spec/README.md`.

## A whole domain, at stage 0

A `build:spec` domain PR also meets its task's criteria, checked across the whole file:

- every Python test the ledger gives the domain has exactly one fate;
- Later and Dropped features follow #11: a Later feature has a rule naming the room the design
  leaves for it, and a Dropped one has none;
- every state machine and parser in the domain's scope is marked, as #15 lists them.

## Deleted rules

A deleted rule's id never returns, and the PR names the triage issue or decision that removed
it. Deleting a rule mined from Python, or a `human`-tier rule, goes to Alex.

## What goes to Alex

These hold the verdict at "waits for Alex" (SKILL.md step 5), each one listed with its reason:

- **A parity change**: a rule the PR adds, changes or deletes that contradicts behaviour mined from
  Python. That is a rule with a `parity:` marker, a change to the meaning of a mined rule, or the
  deletion of one. A rule whose source is a decision ticket was not mined.
- **A `human`-tier rule**: a rule the PR adds, changes or deletes that is `human` tier before or
  after the change, including one moving into or out of that tier.

**At stage 0**, while #73 is open, the parity changes of a `build:spec` domain PR reach Alex
through #73's one list, which `cargo xtask spec parity` builds from the markers. For those, the
check is the marker itself: present, saying what Python did and why. They don't hold the verdict.
