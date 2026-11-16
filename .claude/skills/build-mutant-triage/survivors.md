# Survivors

A **survivor** is a mutant that no spec test kills at any seam that reaches it: `cargo xtask
mutants` runs, against each mutant, the `tests/` of every crate that depends on the mutated one
(#135), on the OS that mutates it, and cargo-mutants reports the mutant missed or timed out. A
mutant no spec test reaches at all is missed too. Nobody writes a test against its diff (ADR 0002).
Each survivor ends in exactly one of three outcomes:

- **rule**: the mutant changes what someone outside can see, so the spec gains or sharpens the
  rule that decides it, and a blind test agent tests it;
- **delete**: nothing outside can see the change, so the code goes;
- **skip**: only another tier can see it, so the code carries a `mutants-justified:` skip naming
  the rule that verifies it there.

## Replay it

Get the mutant's diff:

- **Nightly** (`triage:mutant`): the issue names the run. `gh run download <run> -R
  FredeAlexandre/beamer -p 'mutants-*'` gives every shard's `mutants.out`; each mutant's diff is in
  its `diff/` folder.
- **In-diff** (the implementer's `build:triage`): the issue gives the mutant as cargo-mutants
  printed it, such as `src/zone.rs:41:9: replace > with >= in Zone::crossed`. Make that change by
  hand. While the code is only on the implementer's branch, replay it there
  (`git switch --detach origin/build/<task>-<slug>`), and a **delete** or **skip** is a commit
  you push to that branch, named in your comment on the issue.

Run the spec tests the gate runs on the clean tree, then with the mutant applied (`git apply
<diff>`), then take it out (`git apply -R <diff>`). `<os>` is the runner that mutates the crate
(`cargo xtask crates --runner <os> --for mutants` lists its crates): `linux` for the OS-independent
crates, whose mutants meet only the OS-independent crates' tests, so only the sans-I/O seams
(`beamer-sim`, `beamer-core`) count for them:

```sh
cargo nextest run $(cargo xtask crates --runner <os> --for test) -E 'kind(test)' --no-fail-fast
```

Replay a platform crate's mutant on its OS, as the gate does: `lebig` for the Windows crates
(`dev/README.md`). The tests that fail only with the mutant applied are its killers. Then:

- **None fails**: a survivor: no spec test at any seam that reaches the crate tells the mutant
  from the code. Find its witness, below.
- **A spec test fails**: the mutant is caught now, by a test merged since the run or one still red
  at a `todo!()` then. The case is settled by that test: name it in your comment on the issue.
- **The diff no longer applies, or the code is gone**: the code changed after the run. Make the
  same mutation by hand in the current code and replay it; when the mutated code is gone, the
  case is settled by the commit that removed it. Name that commit.

A **timed-out** mutant usually makes a loop endless. When a spec test hangs on it, that test sees
it: replay with that test alone to be sure, then skip it, naming that test's rule. Otherwise,
triage it like a missed one.

## Find the witness

A **witness** is what tells the mutant from the code through a seam: a state and a sequence of
events or calls, and the different outcome each gives, read through `beamer-sim`, `Core`
(`handle`, `poll`, `next_deadline`) or the crate's public API. Read every caller of the mutated
code up to a public item to find one. Write it down in the seam's terms: "Given <state>, after
<events>, the code gives <outcome>; the mutant gives <other outcome>."

## Choose the outcome

**rule**, when the witness shows at the `ci` tier. Find the rules the code serves: its
`r[impl …]` references, and `tracey query rule <id>`. Then:

- **No rule covers the behaviour**: write a new rule that decides the witness.
- **A rule covers it but leaves the witness open**, or decides it in a way the text hides: sharpen
  it so its text decides the witness. A meaning change is bumped (SKILL.md step 3).
- **A rule already decides it plainly**: the rule stands, and the test task quotes the clause that
  decides it.

Each case of this outcome gets the test task of SKILL.md step 4. When the code itself is on the
wrong side of the rule, as its source decides it, it also gets a fix task blocked by that test
task.

**skip**, when the witness shows only where mutation never runs: a `machine`-tier rule through the
real OS, a `human`-tier rule, a `perf.*` budget. Name the rule that verifies it there; when none
does, write it first (SKILL.md step 3), with its `tier:` marker. Then mark the function:

```rust
// mutants-justified: <why no ci test can see it> (<rule id>)
#[cfg_attr(test, mutants::skip)]
fn send_to_display(…) {
```

A skip takes every mutant of the function out of the gate, including the ones tests kill. When the
rest of the function is tested, move the code the skip is for into a function of its own first.
`cargo xtask test-audit` fails a skip without its `mutants-justified:` line.

**delete**, when no witness exists at any tier: the mutated code is dead, redundant or
equivalent. Delete or simplify it so the mutant can't be made, then run the replay command again:
every spec test that passed still passes.

## When the code change waits

The mutants gate on your PR mutates the lines your **delete** or **skip** changes. A survivor of
the same lines whose outcome is **rule** stays alive until its test merges, and fails that gate.
Then keep the code change out of the PR, and say in your comment on the issue that it lands after
the test task: the issue stays open for it.
