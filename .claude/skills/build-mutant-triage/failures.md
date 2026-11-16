# Failures

A **failure** is a check of the rules that fails on merged code: a bolero harness crashes, a model
finds a counterexample, a machine-tier run fails a rule. Unlike a survivor, a test saw something.
The question is whose it is. Each failure ends as a **fix task** whose branch carries the failing
input, committed, so the fix is checked against it and keeps it fixed.

## Replay it

Find the run the issue names (`gh run list -R FredeAlexandre/beamer -w nightly.yml`), then:

- **A fuzz crash** (`triage:fuzz`): `gh run download <run> -R FredeAlexandre/beamer -n
  fuzz-crashes` gives each crashing input under `<crate>/tests/__fuzz__/<harness>/crashes/`. The
  **input** is that file, copied into the `corpus/` folder beside its `crashes/` folder. Run the
  harness with `cargo test -p <crate> --test <its file>`: `cargo test` replays the corpus, so it
  fails on the input.
- **A model failure** (`triage:model`): `-n model-failures` gives the model's output and its
  counterexample, a sequence of actions.
  - A stateright check is exhaustive, so rerunning it gives the same counterexample:
    `cargo test --release -p <crate> -- --include-ignored <test>`. It has no seed: the
    counterexample goes in the fix task's body, and nothing is committed.
  - A proptest model test fails on a seed: rerun it with the seed and case count the issue gives
    (`PROPTEST_RNG_SEED=<seed> PROPTEST_CASES=<n> cargo nextest run -p <crate> <test>`). proptest
    saves the shrunk case beside the test, in a `.proptest-regressions` file: that file is the
    input.
- **A machine-tier failure** (the coordinator's `build:triage`, at a milestone's end): the issue
  holds the failing log lines and names the report in `dev/.reports/`. That run is the evidence:
  `cargo xtask machine` deploys to both machines, so leave rerunning it to the fix. Nothing is
  committed.

When the input no longer fails on the newest `rust`, a later commit fixed it. Commit the input in
your own PR all the same, so it stays fixed, and name that commit as the case's outcome.

Done when the failure reproduces on the newest `rust` and you hold its input, or it is fixed and
the input is in your PR.

## Whose it is

Read the rules the harness or model cites (its `r[verify …]` lines), or the rule the machine tier
checked, and judge the failing input against them:

- **The code breaks a rule**: a fix task, below.
- **The harness or the model asserts what its rules don't say, or misreads one**: the test is
  wrong. Settle it as a disputed test ([questions.md](questions.md)). Its test task is the fix
  task: its branch carries the input, as below.
- **The rules don't say what happens to this input**: settle it as a spec question
  ([questions.md](questions.md)), then judge it again against the rule you wrote. The fix task is
  blocked by this issue, so it waits for that rule.

## The fix task

Open it as SKILL.md step 4 says: a sub-issue of #24, unclaimed. Label `build:impl`, title
`Fix: <crate> <harness or model>: <what breaks>`. A fix for a rule some test doesn't check yet is
blocked by that rule's test task.

```markdown
## Parent

#24, from #<this issue>

## What to build

The fix in `<crate>` for `<rule id>`, which <harness or model> shows broken by <the input's path
on this task's branch, or the counterexample>. The failure: <the output that matters>.

## Acceptance criteria

- [ ] <the harness or model> passes with the input on this branch
- [ ] Every in-diff mutant is killed by `tests/`
```

Then lay the input on its branch, so the implementer starts from it (the implementer's step 1
continues an existing branch):

```sh
git switch -c build/<fix#>-<slug> origin/rust
git add <the input> && git commit -m '<harness or model>: the failing input from #<this issue>'
git push -u origin build/<fix#>-<slug>
gh pr create -R FredeAlexandre/beamer --base rust --draft --title '<fix title> (#<fix#>)' \
  --body '#<fix#>: the failing input, committed first. The fix follows.'
```

The commit holds the input alone. Its PR stays a draft, red on the input, until the fix lands.
Switch back to your own branch for the rest of the issue.
