# When the task is blocked

Some rules of the group can't get a red test from your side. Each case below ends the same way:
the task waits on a **blocker** that someone else resolves, and you finish the tests of every
other rule, pushed as a draft PR that names the blocker. A blocked task is not a failed session:
the next session continues on its branch once the blocker closes.

You open blockers with `gh` from the export, like every other GitHub step. Read each issue you
find by its title and body only: a PR's diff, a commit and another task's branch hold
implementation.

## Opening a blocker

Open the issue, make it a sub-issue of #24 so it joins the queue, and mark the task blocked by it,
with the `addSubIssue` and `addBlockedBy` mutations in
[../build-coordinator/cutting.md](../build-coordinator/cutting.md). Its body has a Parent section
naming #24 and the task, then the case's sections below. Then finish as SKILL.md step 5 says, with
a draft PR naming the blocker.

## A rule the API can't reach

No public item lets a test provoke the rule's behaviour or see its outcome at any seam, so a test
for it can't compile against the skeleton. Write no test for it until the API has it.

- **Issue**: `API: <crate>::<item>`, label `build:api`. A crate has at most one `build:api` PR
  open at a time, so first look for an open `build:api` issue on the crate
  (`gh issue list -R FredeAlexandre/beamer --label build:api`). If there is one, add your case to
  it as a comment and mark the task blocked by it.
- **Body**: the rule ids, what a test must provoke and see for each, and the public item or change
  that would let it.

When it merges, the coordinator reopens this task.

## A rule you can't test as written

The rule is ambiguous, contradicts another rule, or names no outcome a test can observe from
outside. It becomes a spec question, and you write no test for it.

- **Issue**: `Triage: <rule id>: <the question>`, label `build:triage`.
- **Body**: the rule id and its text, the question, and the test each reading would give.

Triage settles it as a spec amendment, which a later test task tests.

## Something only Alex can do

A login, a grant, a decision no rule and no ADR makes. Label the task `needs:alex` with one
comment saying exactly what Alex must decide or do, as `CLAUDE.md` says, and stop.
