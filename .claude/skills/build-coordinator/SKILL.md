---
name: build-coordinator
description: Run one coordinator session of the Beamer 2.0 build. Reads the queue, cuts tasks, dispatches the frontier to the Mac and lebig, lands PRs and reports what merged and what waits.
disable-model-invocation: true
---

# Build coordinator

You run one session of the build epic, #24. You turn the issue queue into merged PRs on `rust`
and bring Alex only what needs him. Alex starts a session whenever there is work to do. The build
spans many sessions, and the tracker is the only state carried from one to the next.

The queue is a **task graph**: the sub-issues of #24, with native blocked-by edges, plus the
nightly `triage:*` issues. The **frontier** is every open task that is unblocked, unclaimed and
not `needs:alex`. Your job is to keep the frontier moving. Each merge can unblock tasks, and each
unblocked task gets a fresh agent, up to the caps.

## The role

- **Sees** everything: the tracker, `rust`, every worktree on both machines.
- **Touches** the tracker (issues, labels, assignees, sub-issues, blocked-by edges, comments,
  closing and reopening), merges while the bootstrap rule holds (step 4), the milestone build,
  and task worktrees, which it creates and removes. Code reaches `rust` only through task PRs
  written by task agents. Your own `rust` worktree only fast-forwards.
- **Cuts tasks alone.** Task agents open only the triage and follow-up issues their role skill
  names.
- **Finishes** with the session summary (step 6): a comment on #24, and the same summary to Alex.

## Pointers, not pasted context

A brief names the issue, the role skill, the branch and the machine. The agent then reads
`CLAUDE.md`, the issue and its skill itself, with a fresh context. Quote nothing from the spec,
other issues or code. Pointers keep every agent's context its own and keep the test agent
**blind**: a brief that quotes implementation breaks the separation the whole build rests on.

## Steps

### 1. Open the session

1. Fast-forward your `rust` worktree: `git fetch origin && git merge --ff-only origin/rust`.
2. Run `queue.sh` (in this folder). Show Alex the `needs:alex` list first, each issue with the
   one-line ask from its last comment. Act on whatever he answers now: apply the decision and
   remove the label.
3. Settle the claims earlier sessions left. A claim with an open PR goes to step 4. A claim with
   no PR and no live agent gets the comment `Session failed: <what its trail shows>`, loses its
   assignee and rejoins the frontier.

Done when Alex has seen every `needs:alex` issue and every claim is either being watched or
released.

### 2. Cut tasks, at a milestone's start

Only when stage 0 has written the rules (#73 is closed) and the next milestone has no `test` or
`impl` tasks yet. Follow [cutting.md](cutting.md). Done when each rule the milestone's slice
needs is in exactly one `build:test` task.

### 3. Dispatch the frontier

Work through `queue.sh frontier`. For each task:

- **A role-skill label**: its agent runs the skill on that label's row in `CLAUDE.md`. A
  `build:test` task always blocks its paired `build:impl` task, and each runs its own role skill.
- **`build:infra` or `build:docs`**: no role skill. Brief a fresh agent from the issue's
  acceptance criteria, test-first with the `tdd` skill where the task writes `xtask` code.
- **Alex's hands needed** (a privacy grant, creating a cloud routine, a login): dispatch nothing.
  Label it `needs:alex` with one comment giving the exact steps, and move on.

**Place**: `lebig` for `beamer-win`, `beamer-win-ffi` and anything that must run on Windows; the
Mac for the rest. **Caps**: 3 agents building on the Mac and 2 on `lebig`, counted over live
agents. Lower a cap when builds starve a machine; Alex raises them.

Mechanics and the brief: [dispatch.md](dispatch.md). Done when every frontier task has an agent,
waits on Alex, or waits for a cap.

### 4. Land each PR

When an agent reports, check its PR: it targets `rust` from `build/<n>-<slug>`, its checks are
green, and every acceptance criterion holds in the diff. Then:

- **Merge.** Until the ruleset (#34) is closed, you merge `build:infra` PRs yourself:
  `gh pr merge <pr> -R FredeAlexandre/beamer --rebase --delete-branch`. Once #34 is closed, the
  ruleset merges every task PR when its required checks pass, and you merge nothing.
- **Close.** `Closes #n` never fires on `rust`, so after the merge: tick the issue's criteria in
  its body, comment with the PR and anything a later task must know, and close it as completed.
  Fast-forward your `rust` worktree, run `git push pc origin/rust:rust`, and remove the task's
  worktree and branch.
- **A `build:api` merge** reopens every closed `build:test` task whose rule group uses an item
  the PR changed, each with a comment naming the PR and the items.
- **A session that ends without a mergeable PR** gets the comment `Session failed: <why>`. On a
  task's second failed session, split it, turn it into a `build:triage` spec question, or label it
  `needs:alex`. It never gets a third session as it stands.
- **A session that ends blocked** has opened a blocker its role skill names, marked its task
  blocked by it, and left a draft PR. It is not a failed session: the task waits on its blocker,
  and the next session continues on its branch.

Then run `queue.sh frontier` again and go back to step 3.

### 5. End a milestone

When every `test` and `impl` task of the milestone is closed, follow
[milestone.md](milestone.md).

### 6. Close the session

The session ends when Alex says so, or when nothing is left that you can move: the frontier is
empty or waits on Alex, and no agent is live. Before you end it, wait for live agents to report;
a claim still open is settled by the next session's step 1. Post the summary as a comment on #24
and give it to Alex:

- **Merged**: each PR and the issue it closed.
- **Waits on Alex**: each `needs:alex` issue and its ask.
- **In flight**: claims still open, and failed sessions with their count.
- **Next**: the frontier as `queue.sh` now prints it.
