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
| `build:spec` | `build-spec-miner`: mines one domain of the Python app at `archive/python` into rules in `spec/<domain>.md`, and gives every Python test in the domain's ledger slice its fate |
| `build:skeleton`, `build:api` | `build-skeleton-writer`: writes one crate's public API with `todo!()` bodies, every item citing its rules, or one change to that API after the skeleton |
| `build:test` | `build-test-agent`: writes one rule group's spec tests blind, in a `cargo xtask blind` export, each red against the `todo!()` skeleton |
| `build:impl` | `build-implementer`: makes one rule group's merged, red tests pass and kills every in-diff mutant, never touching the tests |
| `build:triage`, `triage:*` | `build-mutant-triage`, not written yet (#41) |
| Review of PRs that add or change rules, and of `build:skeleton` and `build:api` PRs | `build-spec-reviewer`: approves observable, traceable rules in glossary words and skeletons outside Alex's three contracts, or requests changes, or sends the PR to Alex (`needs:alex`) |

## Working a task

1. **Claim** the issue before anything else: `gh issue edit <n> -R FredeAlexandre/beamer
   --add-assignee @me`, plus a comment naming your branch and machine. An assigned issue is
   claimed; leave it to its claimant.
2. **One worktree per task**, on branch `build/<issue#>-<slug>` cut from `origin/rust`:
   `git worktree add ../<dir> -b build/<n>-<slug> origin/rust`. Git refuses branches under
   `rust/` while `rust` exists, hence `build/`.
3. **Land through a PR into `rust`**, merged by rebase so history stays linear. Name the issue in
   the PR body. `Closes #n` never fires on `rust` (it isn't the default branch), so the issue is
   ticked and closed by hand once the PR merges. The ruleset on `rust` (#34) refuses direct
   pushes, and merges a PR with auto-merge on once it is up to date with `rust` and every CI
   check passes.
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

## Building on the Mac

Up to three agents build on the Mac at once (#23). Each worktree keeps its own `target/`: never
set `CARGO_TARGET_DIR`, and never clean or delete another worktree's `target/`. What they share is
one **sccache** disk cache (#35):

- `~/.cargo/config.toml` sets `build.rustc-wrapper` to `/opt/homebrew/bin/sccache`, so every cargo
  build on the Mac, in any worktree, compiles through it. Don't override `RUSTC_WRAPPER`.
- The cache is `~/Library/Caches/Mozilla.sccache`, limited to **50 GiB** by
  `~/Library/Application Support/Mozilla.sccache/config`. Past that limit, sccache evicts the least
  recently used entries.
- One sccache server serves every worktree, so `sccache --show-stats` counts all agents' builds.
  To measure your own build, diff `--stats-format json` taken before and after it. Don't run
  `--stop-server` or `--zero-stats` on the shared server: other agents rely on it and its stats.
  For an isolated measurement, start a private server on the same cache with
  `SCCACHE_SERVER_PORT=<free port>`, and stop that server when you're done.
- A worktree's clean build hits the cache for dependencies that never see a path inside its own
  worktree. It misses for crates that `include!` a file their build script wrote to `OUT_DIR`
  (inside `target/`), for crates downstream of those, and for crates that use a proc-macro. It
  never caches proc-macros, bins or incremental workspace crates. sccache's `basedirs` doesn't
  change this for Rust.

`dev/README.md` ("Shared sccache on the Mac") holds the setup recipe and the measured hit rate.
`lebig` and CI don't use sccache: CI caches with `Swatinem/rust-cache` (#22).
