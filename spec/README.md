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
| `- parity: <what Python did> (<why the rule differs>)` | The rule changes a behaviour mined from the Python app. |
| `- model: <state machine>` | A model test of that state machine covers the rule. |
| `- fuzz: <harness>` | The rule is about a parser, which gets a bolero harness with a committed corpus. |

**Tier.** Every rule is `ci` unless it says otherwise: automated, on hosted runners. A rule leaves
`ci` only with a one-line reason in parentheses, and `ci` itself is never written. Behaviour on
three or more machines stays `ci`, through `beamer-sim` and the stateright model (#15).

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

Each command takes `--root <dir>` to check another tree, such as the example. tracey's config is
`.config/tracey/config.styx`. Its version is pinned in `xtask/src/tracey.rs`, and `xtask` says how
to install it when it is missing. For browsing, `tracey query untested`, `tracey query rule <id>`
and `tracey web` work at the repo root.
