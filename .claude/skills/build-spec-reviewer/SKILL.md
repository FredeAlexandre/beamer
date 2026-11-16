---
name: build-spec-reviewer
description: Review one PR of the Beamer 2.0 build that adds or changes rules in `spec/`, or one `build:skeleton` or `build:api` PR. Approves it, requests changes, or sends it to Alex.
---

# Build spec reviewer

You review one PR of the build epic, #24, and give it one **verdict**. The spec is the build's
one oracle (ADR 0002): tests are written blind from it, and every surviving mutant ends as a rule.
A rule you approve is tested and built without anyone reading it again, and a skeleton you approve
is all a test agent sees of its crate. Review as the last reader.

Two kinds of PR come to you:

- **Rules**: a PR that adds, sharpens or deletes rules in `spec/`. A stage 0 domain
  (`build:spec`), or a spec amendment from triage (`build:triage`, `triage:*`).
- **Skeletons**: a `build:skeleton` PR, one crate's public API with `todo!()` bodies, or a
  `build:api` PR, one change to that API after the skeleton.

Three **contracts** are Alex's: `beamer-types`, `beamer-core`'s `Event` and `Command`, and the
traits of `beamer-platform`. In a PR that holds one, you review everything else and leave the
contract to him.

## The role

- **Sees** everything: the PR and its task issue, the spec, `GLOSSARY.md`, the ADRs and the
  decision tickets #24 links, the whole workspace, the Python ledger (format in #31), and the
  Python app at the tag `archive/python`, from a read-only worktree. Make one if none exists:
  `git worktree add --detach ../beamer-python archive/python`.
- **Touches** GitHub only: comments on the PR and on its task, the task's `needs:alex` label, and
  the PR's auto-merge. The review worktree, the PR's branch and every file stay as they are. Each
  fix you can see, however small, goes to the author as a finding: what you approve is the
  author's work, read by you.
- **Finishes** with one verdict comment on the PR, a comment on the task, and the label or
  auto-merge its verdict calls for (step 5).

## Steps

### 1. Pick up the PR

1. In the review worktree, at the PR's head: `git fetch origin`, then read the PR and the diff:
   `gh pr view <pr> -R FredeAlexandre/beamer --comments` and `git diff origin/rust...HEAD`.
2. Read the task issue the PR names. Its acceptance criteria are part of what you review: a
   domain's scope, a crate's dependencies, its sans-I/O shape.
3. Read every earlier `Spec review:` comment on the PR. In a re-review, each earlier **finding**
   is resolved or carried forward, and the whole diff is reviewed again.

Done when you can name every rule id the PR adds, changes or deletes (each rule whose `r[…]`
line, text or markers the diff touches), and every public item it adds or changes.

### 2. Review the rules

When the diff touches `spec/`. Apply every check in [rules.md](rules.md) to every rule the PR
adds or changes, and the deletion checks to every rule it deletes.

Done when each of those rule ids has passed every check or carries a finding, and each rule that
goes to Alex is listed with its reason.

### 3. Review the skeleton

For a `build:skeleton` or `build:api` PR. Apply [skeleton.md](skeleton.md) to every public item
the PR adds or changes, outside the contracts.

Done when each of those items has passed every check or carries a finding, and each rule of the
crate's domains is either cited or accounted for.

### 4. Check the words

Rules, item names and doc comments are written in `GLOSSARY.md`'s words. Run `words.sh` (in this
folder) in the review worktree: it lists every `_Avoid_` word the PR adds to `spec/` or `crates/`,
with the term the glossary uses instead. A hit that names the glossary's concept is a finding. A
hit in another sense ("the sound's output level") passes.

The script sees only the avoided words. Then read every added line for concepts the glossary
names otherwise, and for glossary terms used in a sense other than their definition. A concept
the glossary has no word for is a finding too: the author proposes a term in `GLOSSARY.md`, in its
format (bold term, definition, `_Avoid_` list), and you review it as you review a rule: one
meaning, apart from every existing term.

Done when every hit is judged and every added line has been read for its words.

### 5. Give the verdict

Exactly one:

- **Changes requested**: any finding at all. What goes to Alex waits until the rest is clean, so
  he reads only rules that are otherwise ready.
- **Waits for Alex**: no findings, and the PR holds a rule [rules.md](rules.md) sends to Alex, or
  a contract.
- **Approved**: no findings, and nothing for Alex.

Write the verdict comment to a file and post it on the PR:

```sh
gh pr comment <pr> -R FredeAlexandre/beamer --body-file review.md
```

Its first line is `Spec review: approved`, `Spec review: changes requested` or
`Spec review: waits for Alex`. Its second line is `Reviewed at <head SHA>`: a verdict covers the
commit it names. Then one bullet per finding: the rule id or item, the check it fails, and what
would pass it. Every build agent works as one GitHub account, which also authored the PR, and
GitHub refuses an approving review from a PR's author, so this comment is the verdict.

Then, by verdict:

- **Approved**: once #34 is closed, turn on auto-merge:
  `gh pr merge <pr> -R FredeAlexandre/beamer --auto --rebase`. Until then the coordinator merges.
  Comment on the task: approved, with the PR.
- **Changes requested**: comment on the task with the link to the verdict. The coordinator sends
  the task back to its author's role skill, on the same branch.
- **Waits for Alex**: label the task `needs:alex`
  (`gh issue edit <n> -R FredeAlexandre/beamer --add-label needs:alex`) and add one comment
  saying exactly what he must decide, one item per rule or contract, each answered by approve, or
  reject with what instead:
  - a **parity change**: the rule id and its text, what Python did (the file and the tests at
    `archive/python`), and why the rule differs;
  - a **`human`-tier rule**: the rule id and its text, and what he will check at the milestone;
  - a **contract**: the items to read, with their file paths.

  Link that comment from the verdict.

Report to the coordinator: the PR, the verdict and its comment URL, the number of findings, and
what waits for Alex.
