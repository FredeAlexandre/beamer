# Beamer 2.0 build guide

`rust` is the integration branch of the Rust rewrite. Every session on it works one task of the
build epic, #24: read the task's issue first. Its acceptance criteria are the definition of done.

## Where things are decided

Follow these pointers; this guide restates none of them.

- **The plan**: the epic, #24. Labels, milestones, order, task size and escalation, plus a link
  to every decision ticket and ADR.
- **What Beamer does**: `spec/<domain>.md`, one file per domain. `spec/README.md` (#29) defines
  the rule format; stage 0 writes the domains.
- **The words**: `GLOSSARY.md`. Name things in code, specs, tests, commits and issues with its words.
- **Why it is built this way**: `docs/adr/`.
- **The Python app** lives only at the tag `archive/python` (#50). Spec miners read it from a
  read-only worktree of that tag.

## Labels and skills

Every task issue carries exactly one `build:*` label. Work it with the skill on its row, in
`.claude/skills/`. Each role-skill ticket fills in its own row.

| Label | Skill |
|---|---|
| `build:epic` (the queue as a whole) | `build-coordinator`: the session Alex starts to cut tasks, dispatch the frontier and land PRs. Task agents are started by it |
| `build:infra`, `build:docs` | No role skill. The coordinator briefs a fresh agent from the issue's acceptance criteria, test-first where the task writes `xtask` code |
| `build:spec` | `build-spec-miner`, not written yet (#37) |
| `build:skeleton`, `build:api` | `build-skeleton-writer`, not written yet (#38) |
| `build:test` | `build-test-agent`, not written yet (#39) |
| `build:impl` | `build-implementer`: makes one rule group's merged, red tests pass and kills every in-diff mutant, never touching the tests |
| `build:triage`, `triage:*` | `build-mutant-triage`, not written yet (#41) |
| Review of new rules and skeleton PRs | `build-spec-reviewer`, not written yet (#42) |

## Working a task

1. **Claim** the issue before anything else: `gh issue edit <n> -R FredeAlexandre/beamer
   --add-assignee @me`, plus a comment naming your branch and machine. An assigned issue is
   claimed; leave it to its claimant.
2. **One worktree per task**, on branch `build/<issue#>-<slug>` cut from `origin/rust`:
   `git worktree add ../<dir> -b build/<n>-<slug> origin/rust`. Git refuses branches under
   `rust/` while `rust` exists, hence `build/`.
3. **Land through a PR into `rust`**, merged by rebase so history stays linear. Name the issue in
   the PR body. `Closes #n` never fires on `rust` (it isn't the default branch), so the issue is
   ticked and closed by hand once the PR merges.
4. **Escalate** with the `needs:alex` label and one comment saying exactly what Alex must decide or
   do, then stop. A task that has failed its gates in two sessions goes back to the coordinator.

## Always

- Every `gh` call carries `-R FredeAlexandre/beamer`. These clones' `gh` default is upstream,
  kalkman-code/beamer, which this build only reads.
- Everything for this build lands on `rust`. `main` is the Python app's line.

## The two machines

The Mac and the Windows PC `lebig` (`ssh lebig`, the `pc` git remote) are documented in
`dev/README.md`: read it before anything touches `lebig` or `pc`. Its SSH, session-0 and `pc`
remote sections hold; its Python build, venv and `dev/deploy.sh` sections describe `main` until
`xtask deploy` (#103) rewrites it. `gh` is not logged in on `lebig` (2026-11-16), so GitHub steps
for work done there run from the Mac.
