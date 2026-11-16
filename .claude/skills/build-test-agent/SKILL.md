---
name: build-test-agent
description: Work one `build:test` task of the Beamer 2.0 build. Writes one rule group's spec tests blind, inside a `cargo xtask blind` export, each red against the `todo!()` skeleton.
---

# Build test agent

You work one `build:test` task of the build epic, #24. You write the spec tests for one rule
group: each rule of the group gets tests that verify it through the highest **seam** that reaches
it, and every test is **red** against the skeleton, panicking at a `todo!()`. The implementer then
makes them pass and kills every mutant with them, never touching them.

You work **blind** (ADR 0002). Your tests come from the spec's text, the one **oracle**, so they
assert what the rules require rather than what some code happens to do. A test written after
seeing the implementation certifies its bugs. Blindness is the point of this role: every file you
read or write is in the export.

## The role

- **Sees** the **export** that `cargo xtask blind` writes: the spec, `GLOSSARY.md`, `CLAUDE.md`
  and the role skills, and every crate with its public API, doc comments and existing tests, each
  function body replaced by `todo!()`. On GitHub, it sees the task issue and its comments, and the
  issues and the PR this skill has you open or look up.
- **The worktree** from the brief is where your tests land. Run there only the commands steps 1
  and 5 give, and open no file in it but a test file a rebase conflict names: it holds the
  implementation and its history.
- **Touches**, in the export: any crate's `tests/`, including bolero corpora under
  `tests/__fuzz__/`, the `[dev-dependencies]` of those crates' `Cargo.toml`, and `Cargo.lock`.
  `cargo xtask copy-tests` brings back exactly these.
- **Leaves untouched**: `spec/`, every crate's `src/` and public API, and the tests of other rule
  groups, unless a rule they cite is in your group or they are a model test or a harness you
  extend ([model-and-fuzz.md](model-and-fuzz.md)). A rule you can't test as written, or can't
  reach through the API, is a blocker ([blocked.md](blocked.md)).
- **Opens** only the issues [blocked.md](blocked.md) names. The coordinator cuts every other task.
- **Finishes** with a PR into `rust`, a comment on the task, and the labels and edges of any
  blocker it opened (step 5).

## Steps

### 1. Pick up the task, and go blind

In the worktree:

1. Read the issue's comments. With two `Session failed:` comments on it, stop here: the task goes
   back to the coordinator to be split, triaged or labelled `needs:alex`, and a third session as
   it stands is ruled out. Report that and end.
2. If `origin/build/<n>-<slug>` exists, an earlier session left a draft PR there: continue it with
   `git reset --hard origin/build/<n>-<slug>`, and read the blockers its PR names.
3. Write the export beside the worktree: `cargo xtask blind ../beamer-<n>-blind`.

Done when the export exists. From here to step 5, every command runs in the export (start each
with `cd <export> &&` when your shell returns to the worktree), and every path you open is in it.

### 2. Read the rule group

The issue names the crate and the rule-id prefix; the rules are in `spec/<domain>.md`. Read them
with the glossary. For each rule:

- `tracey query rule <id>` names the skeleton items that serve it (its `impl` references) and the
  tests that already verify it.
- Its **markers** decide what it gets. A `tier: machine` or `tier: human` rule gets no test here:
  the machine tier and the milestone checklist verify those, and your PR lists them. A
  `model: <state machine>` or `fuzz: <harness>` rule also gets its place in a model test or a
  bolero harness: [model-and-fuzz.md](model-and-fuzz.md).
- Its **seam** is the highest one through whose public items you can provoke the rule's behaviour
  and see its outcome. Walk down this list and take the first that reaches it:
  1. **`beamer-sim`**, for anything that involves two or more machines: N `Core`s over fake
     platforms, an in-memory network and a virtual clock.
  2. **`Core`**, in `beamer-core`, for one machine's behaviour: `handle(Event, now)`,
     `poll() -> Option<Command>`, `next_deadline()`.
  3. **The crate's own public API**, for behaviour the rule states in that crate's own terms,
     below `Event` and `Command`: a parser's bytes, a domain state machine's `handle` and `poll`, a
     view-model's state, or a platform crate's translation code (keycodes, coordinates, OS
     structs to domain events), whose tests run on a hosted runner of that crate's OS.

  The tests go in the seam crate's `tests/`, which may not be the task's crate.

Tests that already cite a rule of the group (`grep -rn 'r\[verify <prefix>' crates`) are part of
the task. On a task reopened by a `build:api` merge, its comment names the changed items, and
each test that uses one no longer compiles in the export until you update it. Each test that
`cargo xtask spec stale` lists cites an older version of its rule: review it against the new
text, then re-cite it, change it, or replace it.

Done when every rule id of the group has its tier, its seam and the public items its tests will
go through, or a blocker from [blocked.md](blocked.md).

### 3. Write the tests

Write each test to its rule's text and the contract below, in `tests/<topic>.rs` of the seam
crate, where `<topic>` is the rule id's topic. A dev-dependency goes in with
`cargo add --dev -p <crate> <dependency>`.

**The contract.** Every test meets these. `cargo xtask test-audit` checks that each test cites and
asserts, and `cargo xtask red-first` that it is red:

- **Cited.** A `// r[verify <id>]` line directly above `#[test]`, one per rule the test verifies,
  each at the rule's current version.
- **Asserts the rule.** Each clause of the rule is an assertion. A rule with a boundary ("once
  the pressure passes the threshold, and not before") gets a test on each side of it. Every
  outcome is read through the seam's public items: what a call returns, the `Command`s `poll`
  gives, what the simulation shows. The assertion message names the rule id.
- **Red.** It panics at a `todo!()` of the skeleton. A test that passes against `todo!()` bodies
  tests nothing, and one that doesn't compile needs an API the skeleton lacks.
- **Deterministic.** Time comes in through `now` and the simulation's virtual clock, randomness
  through a seed you pass. Nextest fails a test that passes only on its retry.
- **In the glossary's words.** The test's name says the behaviour, like
  `a_push_below_the_threshold_does_not_cross`.

A body that is not `todo!()` in the export (`blind` keeps a `const fn`'s and an `impl Trait`
function's) may be implementation: test that item from its rule like any other.

Done when every rule of the group that step 2 gave a seam has its tests, written to the contract.

### 4. Check the tests in the export

```sh
cargo nextest run -p <crate> --no-fail-fast  # each new test fails with "not yet implemented"
cargo xtask spec validate
cargo xtask test-audit       # no .git here, so `git log HEAD failed` is always its last problem
cargo fmt -p <crate>
cargo clippy -p <crate> --tests -- -D warnings
find crates -name '*.proptest-regressions' -delete
```

Run each `-p` for every crate whose `tests/` you changed. Of clippy's findings, yours are those in
`tests/`. The last line deletes the seeds proptest saved when its tests failed on `todo!()`: a
seed is committed only when it fails on the implementation.

Done when every command passes but the two that must fail as their comments say, and every new
test's failure is the `todo!()` panic.

### 5. Land the tests

In the worktree:

1. **Copy back** with `cargo xtask copy-tests ../beamer-<n>-blind`. It prints each file it copied:
   only `tests/`, `Cargo.toml` and `Cargo.lock`.
2. **Commit** them, alone: a test commit touches no implementation file.
3. **Rebase** onto the newest `rust`: `git fetch origin && git rebase origin/rust`. A conflict in
   `Cargo.lock` resolves to `rust`'s lock plus your dev-dependencies (in a rebase, `--ours` is
   `rust`): `git checkout --ours Cargo.lock && cargo metadata --format-version 1 > /dev/null`. A
   conflict in a test file is another task's test: keep both.
4. **Gates.** `cargo xtask red-first --base origin/rust` prints `red` for every new or changed
   test, and `cargo xtask test-audit --since origin/rust` prints no problem. If test-audit asks
   for the tag `archive/python`, fetch it as it says. A failure sends you back to a fresh
   export: `copy-tests` refuses a file copied once and changed again, so delete the old export,
   write a new one from the worktree as step 1 does, fix the tests there, and repeat from step 4.
5. **PR.** Push `build/<n>-<slug>` and open the PR:
   `gh pr create -R FredeAlexandre/beamer --base rust --title '<issue title> (#<n>)'`.
   Its body names #<n>, lists each rule id with the seam and the tests that verify it, the rules
   left to the machine tier or the checklist, and the red-first result. Once #34 is closed, turn
   on auto-merge: `gh pr merge <pr> -R FredeAlexandre/beamer --auto --rebase`. When a blocker from
   [blocked.md](blocked.md) is still open, open it with `--draft` instead, naming each blocker,
   and leave auto-merge off.
6. **Comment** on the task: the PR, any blocker, and anything a later task must know.
7. **Labels**: on success, none. Blockers carry the labels [blocked.md](blocked.md) gives them.
8. **Clean up**: run `tracey kill` in the export, then delete it. The next session exports afresh.

When a gate stays red after you have tried what you can see to try, with no blocker from
[blocked.md](blocked.md) that explains it, the session is stuck. Comment on the task, starting
`Stuck:`, with what fails, what you tried and the output that matters, plus the one way forward
you recommend: a split, a spec question for triage, or a decision for Alex. When the task already
has one `Session failed:` comment, the recommendation is required. Push the branch and report.

On `lebig`, `gh` is not logged in: commit everything, push nothing, and put the PR body and each
issue you would have opened, title and body, in your report. The coordinator does the GitHub steps
from the Mac.

Report to the coordinator: the PR URL (or the last commit, on `lebig`), the rule ids now tested,
the red-first result, and each blocker. Leave merging and closing the issue to the coordinator.
