# Mutants are killed through the spec, by black-box tests only

In the Rust rewrite, the `cargo-mutants` gate counts only the black-box spec tests in each crate's
`tests/` directory. Those tests are written from the spec by an agent that has never seen the
implementation. The implementer may write `#[cfg(test)]` unit tests as scaffolding, but those tests
never count toward the gate. When a mutant survives, nobody writes a test against the mutant's diff.
A triage agent reads the diff and takes one of three outcomes:

- **Spec gap.** It writes or sharpens a rule in `spec/`. A blind test agent then tests that rule.
- **Unobservable code.** The code is deleted.
- **Justified skip.** The code gets a `mutants-justified:` comment.

The aim is tests with teeth instead of AI slop. A test written after seeing the code, or after
seeing a mutant's diff, tends to assert what the code does rather than what the spec requires. Such
a test still kills mutants, so the gate goes green while it certifies the bugs. Tests that never see
the code, plus a surviving mutant that can only become a spec change, keep the spec as the one
oracle.

## Considered options

- **Implementer tests count toward the gate.** This is the cheapest option, but the implementer
  can then satisfy the gate with tests that mirror the code.
- **Separation relaxes after the red-first gate**, so any agent may write a killing test that
  cites an existing rule id. This saves a hop per survivor, but brings back tests written from the
  code.

## Consequences

- Triage costs one extra agent hop for every surviving mutant. We accept that cost.
- A spec amendment from triage is reviewed like any other spec change. A change that contradicts
  behaviour mined from the Python app goes to Alex, because every parity drop is an explicit
  decision.
- Don't "simplify" this by letting the implementer's unit tests count, or by letting the triage
  agent write the test itself.

Source: the wayfinder ticket "Test strategy for the Rust app" (#15) on FredeAlexandre/beamer; the
tool findings on `research/rust-testing-toolkit`.
