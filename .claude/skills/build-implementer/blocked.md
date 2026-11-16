# When the task is blocked

Some of the group can't go green from your side. Each case below ends the same way: the task
waits on a **blocker** that someone else resolves, and you finish everything that doesn't depend
on it, pushed as a draft PR that names the blocker. A blocked task is not a failed session: the
next session continues on its branch once the blocker closes.

## Opening a blocker

Open the issue, make it a sub-issue of #24 so it joins the queue, and mark the task blocked by it,
with the `addSubIssue` and `addBlockedBy` mutations in
[../build-coordinator/cutting.md](../build-coordinator/cutting.md). Its body has a Parent section
naming #24 and the task, then the case's sections below. Then finish as SKILL.md step 6 says,
with a draft PR naming the blocker.

## A disputed test

A test you believe is wrong becomes a spec question, and the test stays exactly as it is.

- **Issue**: `Triage: disputed test <crate>/tests/<file>::<test> (<rule id>)`, label
  `build:triage`.
- **Body**: the rule id and its text, the test and the assertion in question, what you believe
  the rule requires instead, and why: the contradiction, the overreach, or the other test it
  can't pass together with.

Triage settles it as a spec amendment, then a new blind test, or as a confirmation that the test
is right.

## A survivor that is a spec gap

A mutant that changes behaviour a test could see, but none does.

- **Issue**: `Triage: survivor in <crate>::<function> (<rule id or "no rule">)`, label
  `build:triage`.
- **Body**: the mutant as `cargo mutants` printed it, the code it mutates, and what that code does
  that no rule says, or which rule it serves that its tests don't reach.

Triage settles it as a new or sharpened rule and a blind test for it, or a `mutants-justified:`
skip.

## A public API change

A rule can't be met through the public items the skeleton gave you.

- **Issue**: `API: <crate>::<item>`, label `build:api`. A crate has at most one `build:api` PR
  open at a time, so first look for an open `build:api` issue on the crate. If there is one, add
  your case to it as a comment and mark the task blocked by it.
- **Body**: the item, the change, and the rule ids that need it.

When it merges, the coordinator reopens the test tasks it affects.

## A lower crate's `todo!()`

A test of your group panics at a `todo!()` in another crate. That crate's group has its own
`build:impl` task, and the edge between the two is missing. Open nothing. Comment on your task
naming the item, its crate, and that crate's impl task if you can find it. The coordinator adds
the edge.

## Something only Alex can do

A privacy grant, a login, a decision no rule and no ADR makes. Label the task `needs:alex` with
one comment saying exactly what he must decide or do, as `CLAUDE.md` says, and stop.
