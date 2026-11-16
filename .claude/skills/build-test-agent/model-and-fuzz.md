# Model tests and fuzz harnesses

A rule with a `model:` or `fuzz:` marker (`spec/README.md`, "Markers") gets its place in a model
test or a bolero harness, besides any example tests the contract in SKILL.md asks for. Both meet
that contract too: cited, asserting, red, deterministic.

## A `model:` rule

Each state machine the marker names gets **one** proptest-state-machine test for the whole build,
in the `tests/model_<state machine>.rs` of its seam crate. Look for it first: when an earlier task
wrote it, extend it, and add your rules' `r[verify]` lines to the ones it carries.

- **The reference model** (`ReferenceStateMachine`) is the spec in code: its state, its
  transitions and their preconditions come from the text of the rules that carry the marker, read
  with the glossary. It is the oracle, so it stays as plain as the rules are.
- **The system under test** (`StateMachineTest`) drives the seam's public items with each
  transition. After each one, `apply` asserts that what the seam shows matches the reference
  state; a rule that holds in every state is an assertion in `check_invariants`.
- **The test** is one `prop_state_machine!` with a `// r[verify <id>]` line inside the macro,
  above its `#[test]`, for each rule with the marker. Leave its config at the default: the
  mutants runs fix the seed and the case count through `PROPTEST_RNG_SEED` and `PROPTEST_CASES`,
  and a config in the test would override them.

`test-audit` counts only test functions with a body, so it skips this test and checks neither its
citations nor its assertions: check them yourself. tracey still sees its `r[verify]` lines, and
`red-first` runs it.

## A `fuzz:` rule

Each harness name gets **one** bolero harness: a test function named for it, with `-` as `_`
(`link-frame` is `link_frame`), in the seam crate's `tests/fuzz_<harness>.rs`. Its seam is the
public item that turns bytes from outside the process into a value: the parser itself.

- **The harness** takes raw bytes, `bolero::check!().for_each(|bytes: &[u8]| { … })`, hands them
  to the parser, and asserts, for every input, what the harness's rules say: a frame that cannot
  be decoded is refused rather than half-read, a decoded value encodes back to the same bytes. It
  carries a `// r[verify <id>]` line for each rule that names the harness.
- **The corpus** is committed. Write one seed per message shape the rules describe, each a file of
  raw bytes in `tests/__fuzz__/<test function>/corpus/`, built from the layout the spec gives
  (a test vector the spec copies is a seed as it is). `cargo test` replays the corpus before its
  random inputs, and the nightly fuzz job starts from it.
