# The spec

What Beamer does, as rules: one file per domain, `spec/<domain>.md`, named for the crate suffix
(`crossing.md` for `beamer-crossing`, #20). Rules use [tracey](https://github.com/bearcove/tracey)'s
syntax, and are written in `GLOSSARY.md`'s words. `cargo xtask spec validate` checks everything on
this page. [`xtask/tests/fixtures/spec-example/`](../xtask/tests/fixtures/spec-example/) shows
every part of the format, with tests citing it.

## Rules

```markdown
r[crossing.zone.push]
A push against a zone crosses to the zone's peer once the push pressure passes the threshold.
```

- The marker `r[<id>]` stands alone at the start of a line. The rule's text follows on the next
  lines, up to a blank line.
- The id is `domain.topic.rule`: the file's domain, one or more topics, then the rule. Each part
  is lowercase kebab-case. An id keeps one meaning for good: it is never renamed, and a deleted
  rule's id is never reused.
- A rule states one behaviour that a test can observe from outside.
- A rule with several paragraphs, or with markers, is written as a blockquote, every line starting
  with `>`.

## Versions

A rule without a suffix is version 1. When its meaning changes, which includes a change to its
markers, its version goes up by one: `r[crossing.zone.push+2]`, then `+3`. `tracey bump` does this
for staged spec files. A wording fix that keeps the meaning keeps the version.

A bump makes every reference to the older version **stale**. `cargo xtask spec validate` fails, and
`cargo xtask spec stale` lists the stale references, until each one is reviewed against the new text
and cites the new version. The test agent re-cites tests, and the implementer re-cites `impl`
references. Neither touches the other's (ADR 0002).

## Tests cite rules

```rust
// r[verify crossing.zone.push+2]
#[test]
fn a_push_below_the_threshold_does_not_cross() {
```

Every spec test in a crate's `tests/` has a `// r[verify <id>]` line directly above it for each
rule it verifies. Each line cites the rule's current version. Elsewhere in a crate,
`// r[impl <id>]` marks the code that serves a rule. tracey refuses `impl` references in `tests/`.

`cargo xtask test-audit` holds the spec tests to what ADR 0002 needs of them:

- **A spec test** is a function with a `#[test]` attribute (or `#[<path>::test]`) in any file under
  a crate's `tests/`, including inside a macro such as `proptest!`, and a `prop_state_machine!`
  test, which has no body.
- **It cites.** Its `r[verify]` lines sit in the comment lines directly above it or among its
  attributes, with no blank line between. Each names a rule in `spec/` at its current version.
- **It asserts.** Its body calls an `assert*!`, `prop_assert*!` or `panic!`, or calls a helper
  function in the crate's `tests/` that does, or it is `#[should_panic]`. A state-machine test, run
  by `prop_state_machine!` or by `<test>::test_sequential`, also asserts through the
  `ReferenceStateMachine` and `StateMachineTest` impls in the crate's `tests/` of the state machine
  it runs. `debug_assert*!` doesn't count.
- **A skip has a reason.** An ignored spec test is `#[ignore = "<why>"]`. A `mutants::skip`
  anywhere in a crate has a `// mutants-justified: <reason> (<rule id>)` comment directly above
  it.
- **Unit tests don't count.** An implementer's `#[cfg(test)]` unit tests in `src/` need neither,
  and an `r[verify]` line outside `tests/` fails, so a unit test never stands in for a spec test.
  The mutants runs count only the `tests/` binaries, and so does the kill matrix.
- **Test commits and implementation commits are apart.** In a crate, a test file is anything
  under `tests/`, `proptest-regressions/`, `__fuzz__/` (bolero corpora) or `snapshots/` (insta).
  Any other file of the crate is an implementation file, apart from its `Cargo.toml`, which a test
  commit may need for a dev-dependency. Files outside `crates/` are neither. No commit touches
  both a test file and an implementation file, so an implementation commit never touches a test.
  Rebase-merging keeps commits whole on `rust`, so the check holds over its whole history. On a
  task branch, `--since origin/rust` checks only the branch's own commits.

## The Python ledger

The ledger accounts for every Python test at the tag `archive/python` (#15): which domain owns it,
and what became of it. Each domain's tests are in `spec/ledger/<domain>.txt`, one per line:

```text
# Comments and blank lines are ignored.
core/tests/test_crossing.py::ZoneTests::test_push_crosses  mined: crossing.zone.push, crossing.zone.held-at-edge
core/tests/test_crossing.py::ZoneTests::test_old_menu  dropped: <the feature, as #11 dropped it>
win_app/tests/test_tray.py::test_icon_refresh  python-detail: Qt's tray icon repaint
core/tests/test_wire.py::test_v6_hello  wire-v6: the v6 hello frame, replaced by spec/link.md
core/tests/test_store.py::test_merge_order
```

- **The test** is named as pytest names it: its path at `archive/python`, any classes it sits in,
  then the function, joined by `::`. Every `def test_*` and `async def test_*` at module level or
  in a class counts, in every `.py` file. A function nested in a function doesn't.
- **The owner domain** is the file the test is in. Each test is in the ledger once.
- **The fate** follows the test, after blanks, and is one of four:
  - `mined: <rule id>, …`: mined into these rules, named without their version;
  - `dropped: <reason>`: the feature is dropped, per #11;
  - `python-detail: <reason>`: a Python or Qt implementation detail;
  - `wire-v6: <reason>`: a wire-v6 detail, superseded by the new protocol.
- **No fate yet** is a test alone on its line, as #51 first lays the ledger out.

`cargo xtask test-audit` checks the ledger on every PR and accepts a partial one: tests missing or
without a fate pass. It fails a test that is in the ledger twice, an id that is not a Python test
at `archive/python`, a rule id that is not in the spec, and a malformed fate. `--complete`, run
before the build lands, also requires every Python test exactly once, each with a fate. The tag
must be in the clone (`git fetch origin tag archive/python`); `--python-ref` names another ref.

## Markers

A rule's markers are the list that ends its blockquote, one `- key: value` per line:

```markdown
> r[crossing.zone.held-at-edge]
> While push pressure builds, the pointer is held at edge.
>
> - tier: machine (push pressure comes only from the real capture path)
> - parity: Python crossed on the first pixel of the edge (#11 keeps push pressure, against accidental crossings)
> - model: driving
```

Markers belong inside the quote because tracey versions only a rule's own text. A list written
under an unquoted rule falls outside that text, so a changed marker would leave its tests looking
current.

| Marker | Means |
|---|---|
| `- tier: machine (<reason>)` | Verified on the Mac and `lebig` by `cargo xtask machine` (ADR 0005). |
| `- tier: human (<reason>)` | Felt, heard or seen, so Alex checks it in the milestone checklist. |
| `- do: <what Alex does>` | A `human`-tier rule's check: what Alex does at the desk. |
| `- see: <what he sees, hears or feels>` | A `human`-tier rule's check: what he sees, hears or feels when the rule holds. |
| `- parity: <what Python did> (<why the rule differs>)` | The rule changes a behaviour mined from the Python app. |
| `- model: <state machine>` | A model test of that state machine covers the rule. |
| `- fuzz: <harness>` | The rule is about a parser, which gets a bolero harness with a committed corpus. |

**Tier.** Every rule is `ci` unless it says otherwise: automated, on hosted runners. A rule leaves
`ci` only with a one-line reason in parentheses, and `ci` itself is never written. Behaviour on
three or more machines stays `ci`, through `beamer-sim` and the stateright model (#15).

**Do and see.** A `human`-tier rule carries a `do:` and a `see:`, and no other rule does. They are
its box in the milestone checklist, `cargo xtask checklist` (#49): Alex does the one and ticks the
box once he has seen the other. Like every marker they are part of the rule's text, so a change to
what Alex checks bumps the rule, and its box comes back unticked.

```markdown
> r[crossing.landing.same-height]
> After a crossing, the pointer lands at the height it left, scaled to the driven display.
>
> - tier: human (where the pointer appears is judged by eye)
> - do: Move the pointer across the edge to the other machine, near the top and then the bottom
> - see: The pointer appears on the other screen at the height it left, both times
```

**Parity.** Every behaviour change from the Python app carries a `parity:` marker, which states
what Python did and, in the final parentheses, why the rule differs. `cargo xtask spec parity`
gathers them into one list for Alex (#73). A parity change, like any change to a `human`-tier rule,
needs Alex's approval (#15, ADR 0002).

**Model.** Each of the eight state machines from #15 gets one proptest-state-machine test, and its
reference model is written from the rules that carry its marker. That test cites each of those rules.
A rule may name more than one model.

| `model:` | The state machine |
|---|---|
| `driving` | Crossings and driving across N machines, including pass on |
| `link` | The link's life: connect, keep-alive, loss, reconnect, dormant repair |
| `pairing` | Code exchange, pairing by address, discovery |
| `handover` | A shared display's handover |
| `store` | Settings sync and the active profile through the store, including conflicting edits |
| `clipboard` | Clipboard transfer: chunking, reassembly, a newer copy superseding an older one |
| `audio` | The graph of audio routes |
| `camera` | Camera routes following input |

**Fuzz.** Anything that turns bytes from outside the process into a value is a parser. Each
harness has a kebab-case name, and every rule its harness checks carries that name. These are
#15's first harnesses: `link-frame`, `pairing-message`, `discovery-packet`, `store-file`,
`clipboard-payload`, `media-frame`, `ddc-reply`, `camera-shm`, `hal-control`.

## Commands

| Command | Does |
|---|---|
| `cargo xtask spec validate` | Checks this format, then runs `tracey query validate`: unknown or malformed ids, duplicates, `impl` in tests, stale references |
| `cargo xtask spec stale` | Runs `tracey query stale` and fails on any stale reference |
| `cargo xtask spec parity` | Prints every parity change as a Markdown list |
| `cargo xtask spec digest --since <rev>` | Prints every rule added, bumped or removed since `<rev>`, parity and `human`-tier changes first: the daily digest on #118 (`dev/routines/spec-digest.md`) |
| `cargo xtask test-audit` | Checks the spec tests, the skips, the commits and the ledger as above. `--since <rev>` checks only the commits after `<rev>`; `--complete` requires the whole ledger |
| `cargo xtask checklist <milestone>` | Opens the milestone's issue for Alex, labelled `needs:alex`: a box per `human`-tier rule with its `do:` and `see:`, and a link to the newest machine-tier report in `dev/.reports/`, uploaded as a secret gist (`--report` names another, or a URL). `--only <prefix>` keeps the milestone's rule groups. Run again, it updates the issue and keeps Alex's ticks on rules that kept their version. `--dry-run` prints it and writes nothing |
| `cargo xtask kill-matrix <mutants.out>…` | Prints, as Markdown, which spec test kills which mutant and which spec tests kill none, from the `mutants.out` of each shard of a run made with nextest's `--no-fail-fast`, as the nightly's is |

Each command but `kill-matrix` takes `--root <dir>` to check another tree, such as the example.
tracey's config is `.config/tracey/config.styx`. Its version is pinned in `xtask/src/tracey.rs`, and
`xtask` says how to install it when it is missing. For browsing, `tracey query untested`, `tracey
query rule <id>` and `tracey web` work at the repo root.
