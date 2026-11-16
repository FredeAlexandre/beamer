---
name: build-mutant-triage
description: Work one `build:triage` or `triage:*` issue of the Beamer 2.0 build. Settles each surviving mutant, disputed test, spec question, fuzz crash or model failure through the spec, never by writing a test.
---

# Build mutant triage

You work one triage issue of the build epic, #24. The issue holds one or more **cases**. Each
case shows the spec, the tests and the code disagreeing, or the tests missing something. You give
each case one **outcome**. The spec is the build's one **oracle** (ADR 0002): every test is
written **blind**, from the spec's text, by a test agent who never sees the code. You see
everything, so you change what the tests check only by changing what the spec says, and a blind
test agent writes the test.

| Case | Comes from | Outcomes | Settled in |
|---|---|---|---|
| A **survivor**: a mutant no spec test kills | the implementer's `build:triage`, the nightly's `triage:mutant` | **rule**, **delete**, **skip** | [survivors.md](survivors.md) |
| A **disputed test** or a **spec question** | the implementer, the test agent, the skeleton writer or the coordinator, all `build:triage` | **amendment**, **confirmation**, or Alex | [questions.md](questions.md) |
| A **failure**: a fuzz crash, a model failure, a machine-tier failure | the nightly's `triage:fuzz` and `triage:model`, the coordinator at a milestone's end | **fix task**, with the failing input committed | [failures.md](failures.md) |

## The role

- **Sees** everything: the issue and the task it blocks, the spec, `GLOSSARY.md`, the ADRs, the
  whole workspace with its code, tests and history, the nightly's artifacts, the tracker, and the
  Python app at the tag `archive/python`, from a read-only worktree. Make one when
  `git worktree list` shows none: `git worktree add --detach ../beamer-python archive/python`.
- **Touches**:
  - `spec/`: the rules your cases add, sharpen or delete, in `spec/README.md`'s format, and
    `GLOSSARY.md` for a term a rule needs, in its format;
  - a crate's `src/`, for two outcomes only: **delete** and **skip** ([survivors.md](survivors.md));
  - a failing input (a fuzz corpus entry or a proptest regression file), committed alone
    ([failures.md](failures.md)).
- **Leaves to the test agent** every test's code: its assertions, its seam, its citations. Leave
  `.cargo/mutants.toml`, `xtask/` and `.github/` as they are.
- **Opens** only the follow-up tasks step 4 names. The coordinator cuts every other task.
- **Finishes** with a PR into `rust` holding every file you changed, the follow-up tasks with
  their edges, and one comment on the issue giving each case its outcome (step 5).

## Steps

### 1. Pick up the issue

1. Read the issue and its comments. With two `Session failed:` comments on it, stop here: it goes
   back to the coordinator, and a third session as it stands is ruled out. Report that and end.
2. If `origin/build/<n>-<slug>` exists, an earlier session left a PR there: continue it with
   `git reset --hard origin/build/<n>-<slug>`, and read the PR and its `Spec review:` comments.
   Each finding of the latest review is this session's work.
3. Read the task the issue's Parent section names, if any: the implementer's, test agent's or
   skeleton writer's task that waits on this issue, and its draft PR. A nightly issue has none.
4. List the cases. A nightly `triage:mutant` issue groups the mutants of one function: each
   mutant is its own case.

Done when every case is listed with its kind and the rule ids it touches.

### 2. Settle each case

Follow the case's file in the table. Each case ends in exactly one of its outcomes, with the
**evidence** that decides it: a survivor's **witness**, a disputed rule's source, a replayed
failure.

Done when every case has its outcome and its evidence, or waits on Alex
([questions.md](questions.md)).

### 3. Write the rules

For every outcome that changes `spec/`. Read `spec/README.md`, then the reviewer's checks in
[../build-spec-reviewer/rules.md](../build-spec-reviewer/rules.md): the spec reviewer reads your
rules against them, "Behaviour, when it comes from a mutant" included.

- **Behaviour.** A rule states what a user, a peer or the OS sees through a seam, in the
  glossary's words. A rule born from a mutant states the witness as behaviour, never the mutated
  code's condition.
- **The source decides what is right**, not the code, which is only the implementer's reading.
  For a rule mined from Python, that is what Python did at `archive/python`: the code and the tests
  the ledger (`spec/ledger/<domain>.txt`) maps to the rule. For a rule from a decision, the ticket
  or ADR. A rule that differs from Python carries a `parity:` marker; the reviewer sends it to
  Alex. When no source decides it, the case is Alex's ([questions.md](questions.md)).
- **Ids and versions.** A new rule takes an id that has never existed
  (`git log origin/rust --oneline -S 'r[<id>' -- spec/` prints nothing). A change of meaning,
  a marker included, bumps the version by one (`tracey bump`); a wording fix keeps it.
- **Check**: `cargo xtask spec validate`. A bump's stale references are the one failure the PR
  may carry: the follow-up test task re-cites the tests, and the implementer the `impl`
  references.

Done when every rule change is written and `spec validate` passes, apart from the stale
references of your bumps.

### 4. Open the follow-ups

Each issue below is a sub-issue of #24 with the blocked-by edges it names, made with the
`addSubIssue` and `addBlockedBy` mutations in
[../build-coordinator/cutting.md](../build-coordinator/cutting.md). Leave each one unclaimed: it
joins the frontier.

**A test task** for every rule your outcomes add or change, and every rule whose test is wrong.
One task per rule-id prefix (`domain.topic`), label `build:test`, title
`Re-test: <domain.topic> (<rule ids>)`. A test agent reads it blind, so write it from the spec
alone: rule ids and rule text, never a mutant, the code, a commit or a link to this issue.

```markdown
## Parent

#24

## What to build

Spec tests for these rules of `spec/<domain>.md`, at their current versions:

- `<id>`: new
- `<id>+<v>`: bumped; its tests cite an older version
- `<id>`: the case "<the clause of the rule that decides it>" has no test yet
- `<id>`: the test `<crate>/tests/<file>::<test>` asserts <what it asserts>, which the rule doesn't say

## Acceptance criteria

- [ ] Every rule above has a test carrying `// r[verify <id>]` at its current version, red
  against the skeleton, through the highest seam that reaches it
- [ ] `cargo xtask spec stale` lists no reference to these rules
```

Its edges: it is blocked by this issue, so it waits for your rules to merge. A task that waits on
this issue (step 1) is also blocked by the test task, so the waiting task finds the new tests
merged.

**A fix task** when the code does the opposite of a rule: [failures.md](failures.md) gives its
issue, for failures and for survivors alike. A fix that needs the new tests is blocked by their
test task.

Done when each rule change has its test task, each code that breaks a rule has its fix task, and
every edge is in place.

### 5. Land and finish

1. **Commit** apart: the spec in one commit, the deleted code and skips in another, a failing
   input alone. `test-audit` fails a commit that touches both a test file and an implementation
   file.
2. **Rebase** onto the newest `rust`: `git fetch origin && git rebase origin/rust`.
3. **Gates.** Run every job of the CI workflow in `.github/workflows/` that covers what you
   changed, the way it runs them; its jobs are the list. For a change in `src/`, that includes the
   mutants gate on the crate's runner (`cargo xtask crates --runner <os> --for mutants` lists
   them): `cargo xtask mutants --runner <os> --in-diff origin/rust`.
4. **PR.** Push `build/<n>-<slug>` and open the PR:
   `gh pr create -R FredeAlexandre/beamer --base rust --title '<issue title> (#<n>)'`. Its body
   names #<n>, and gives each case with its outcome, its evidence, and the rule ids, commits and
   follow-up tasks it produced. A PR that changes `spec/` waits for its spec review: the
   coordinator dispatches the reviewer, and the reviewer turns on auto-merge. Any other PR: once
   #34 is closed, turn on auto-merge: `gh pr merge <pr> -R FredeAlexandre/beamer --auto --rebase`.
   When no file changed, because every case became a fix task, there is no PR.
5. **Comment** on the issue. Its first line is `Triage: <k> cases settled` (or
   `<k> of <m> cases settled`), then one line per case: the mutant, test, question or failure,
   its outcome, and where that outcome lives (a rule id at its version, a commit, a task).
6. **Labels**: none, unless a case waits on Alex ([questions.md](questions.md)).

On `lebig`, `gh` is not logged in: commit everything, push nothing, and put the PR body, the
comment and each follow-up issue, title and body, in your report. The coordinator does the GitHub
steps from the Mac.

When a gate stays red after you have tried what you can see to try, the session is stuck. Comment
on the issue, starting `Stuck:`, with what fails, what you tried and the output that matters, plus
the one way forward you recommend. Push the branch and report.

Report to the coordinator: the PR URL (or the last commit, on `lebig`), each case with its
outcome, the follow-up tasks, and what waits on Alex. Leave merging and closing the issue to the
coordinator.
