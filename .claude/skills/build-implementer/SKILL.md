---
name: build-implementer
description: Work one `build:impl` task of the Beamer 2.0 build. Makes one rule group's merged, red spec tests pass and kills every in-diff mutant, without touching the tests.
---

# Build implementer

You work one `build:impl` task of the build epic, #24. Its rule group's tests are already merged
on `rust` and **red**: each panics at a `todo!()` in the skeleton. You write the bodies that turn
them green, until every mutant `cargo mutants --in-diff` makes of your diff is killed by
`tests/`. The spec is the **oracle**. The tests are the spec's, written blind by the test agent,
and they are how the gates hear it (ADR 0002).

## The role

- **Sees** everything: the spec, `GLOSSARY.md`, the ADRs, the whole workspace with every crate's
  tests, the tracker, and the Python app at the tag `archive/python`, read as prior art for
  how a rule was done. Where the Python app and a rule differ, the rule wins.
- **Touches** the task's crate: its function bodies, its private items and modules, its
  dependencies within what `xtask deps` allows, and `#[cfg(test)]` **scaffolding** in `src/`.
  Scaffolding is yours to write and delete. It never counts: the mutants gate counts only
  `tests/`, so a mutant that only scaffolding kills is still a **survivor**.
- **Leaves untouched**: `tests/`, `proptest-regressions/`, fuzz corpora, `insta` snapshots,
  `spec/` and `.cargo/mutants.toml`, in every crate, and every `mutants::skip` and
  `mutants-justified:` decision. Tests, the spec and skips change only through triage. This is the
  separation the build rests on, and `xtask test-audit` fails any implementation commit that
  touches a test file.
- **The public API is fixed.** Each public item keeps the signature and the rule-id citations the
  skeleton gave it. A change to it is a `build:api` task ([blocked.md](blocked.md)).
- **Opens** only the issues [blocked.md](blocked.md) names. The coordinator cuts every other task.
- **Finishes** with a PR into `rust`, a comment on the task, and the labels and edges of any
  blocker it opened (step 6).

## Steps

### 1. Pick up the task

1. Read the issue's comments. With two `Session failed:` comments on it, stop here: the task goes
   back to the coordinator to be split, triaged or labelled `needs:alex`, and a third session as
   it stands is ruled out. Report that and end.
2. If `origin/build/<n>-<slug>` exists, an earlier session left a draft PR there: continue it with
   `git reset --hard origin/build/<n>-<slug>`, and read its PR and the blockers it names.
3. Read the rule group: the issue names the crate and the rule-id prefix; the rules are in
   `spec/<domain>.md`. Read them with the glossary.
4. Find the group's tests by rule id across the workspace: they sit behind the highest seam that
   reaches each rule, which may be `beamer-sim`'s or `beamer-core`'s `tests/` rather than your
   crate's:
   `grep -rn 'r\[verify <prefix>' --include=*.rs .`
5. Run them with `cargo nextest run` and see them red.

Done when you can name every rule id in the group, each test that verifies it, and the `todo!()`
each one panics at. A test that panics at a `todo!()` in another crate's group is blocked: see
[blocked.md](blocked.md).

### 2. Implement

Write each body to its rule's text, and let the tests confirm it. Code written to the assertions
passes the tests and leaves the rule's other cases to the bugs. Name things with the glossary's
words. Use scaffolding wherever a quick unit test helps you build.

When a test seems wrong (it contradicts the rule it cites, asserts more than the rule says, or
cannot pass together with another test), it is a **disputed test**: follow
[blocked.md](blocked.md) and implement the rest of the group.

Done when every test of the group passes under `nextest`, apart from disputed ones, and every
test that passed on `origin/rust` still passes.

### 3. Kill the in-diff mutants

Run the CI workflow's mutants gate locally, against your diff from `origin/rust`. Its test filter
lives in `.cargo/mutants.toml`, so the gate and your run count the same tests:

```sh
git diff origin/rust... > ../mutants.diff
cargo mutants --in-diff ../mutants.diff --test-tool=nextest
```

Each survivor (missed or timed out) is one of two things:

- **Unobservable**: nothing a test can see changes. Simplify or delete that code, then run again.
- **Observable, but no test pins it**: the code serves a rule the tests don't reach, or no rule
  at all. That is a spec gap: follow [blocked.md](blocked.md).

Done when the run reports no survivors, or each remaining one has its `build:triage` issue.

### 4. Pass the merge gates

Rebase onto the newest `rust` (`git fetch origin && git rebase origin/rust`), then run every job
of the CI workflow in `.github/workflows/` that covers your crate, the way it runs them: `fmt`,
`clippy -D warnings`, the `xtask` checks, `tracey`, `nextest`. Its jobs are the list; this skill
keeps no copy of it.

When the task's crate is a platform or media crate and its group has `machine`-tier rules, also
run `cargo xtask machine <prefix>` (#104). It deploys to both machines, so run it once, at the
end.

Done when every gate passes locally on your crate's OS.

### 5. When you can't get there

A gate that stays red after you have tried what you can see to try, with no blocker from
[blocked.md](blocked.md) that explains it, is a stuck session. Comment on the task, starting
`Stuck:`, with what fails, what you tried and the output that matters. Add the one way forward
you recommend: a split (which rule ids go where), a spec question for triage, or a decision for
Alex. When the task already has one `Session failed:` comment, this is its last session as it
stands, so the recommendation is required. Push the branch, and end with the report. The
coordinator records the failed session.

### 6. Finish

1. **Commit** only implementation and scaffolding, rebased onto `origin/rust`.
2. **PR.** Push `build/<n>-<slug>` and open the PR:
   `gh pr create -R FredeAlexandre/beamer --base rust --title '<issue title> (#<n>)'`.
   Its body names #<n>, lists the rule ids now green, and gives the mutants result. Once #34 is
   closed, turn on auto-merge: `gh pr merge <pr> -R FredeAlexandre/beamer --auto --rebase`.
   When a blocker from [blocked.md](blocked.md) is still open, open it with `--draft` instead,
   naming each blocker, and leave auto-merge off.
3. **Comment** on the task: the PR, any blocker, and anything a later task must know.
4. **Labels**: on success, none. Blockers carry the labels [blocked.md](blocked.md) gives them.

On `lebig`, `gh` is not logged in: commit everything, push nothing, and put each issue you would
have opened, title and body, in your report. The coordinator does the GitHub steps from the Mac.

Report to the coordinator: the PR URL (or the last commit, on `lebig`), the rule ids now green,
the mutants result, and each blocker. Leave merging and closing the issue to the coordinator.
