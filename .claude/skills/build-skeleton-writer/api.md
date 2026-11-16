# A `build:api` task

After the skeleton, a crate's public API changes only through a `build:api` task: one change to
one crate, in its own PR, with at most one such PR open per crate (#23). An implementer, a test
agent or a skeleton writer opened the issue as `API: <crate>::<item>`, with the change and the rule
ids that need it; later cases arrive as comments on it. This file replaces steps 2 and 3 of
SKILL.md. Steps 4 and 5 hold, with the additions at the end.

## Before you start

Look for another open API PR on the crate:
`gh pr list -R FredeAlexandre/beamer --base rust --search 'API: <crate>'`. One means this task waits for it: comment on the task naming that PR, and stop.

## The change

- Make the change the issue and its comments ask for, and nothing beside it: no new logic, no
  tests, no spec. Every new or changed item meets the contract in SKILL.md, citations included.
- **Tests stay as they are.** Find each test that uses an item you change:
  `grep -rn '<item>' crates/*/tests`.
  - No test uses it: change it in place.
  - A test uses it: **expand** instead. Add the new item beside the old one, and give the old one
    the doc line `Replaced by <new item> (#<n>).` Its removal is a later `build:api` task, which
    the coordinator cuts once the tests have moved to the new item.
- **Bodies.** An implemented body the change breaks gets the mechanical edit only (a renamed
  field, an argument passed through). A body that needs more becomes `todo!()`, for the impl task
  the change unblocks.
- **Callers** in other crates' `src/` get the same mechanical edits, so the workspace compiles.

Done when `cargo build --workspace --all-targets` passes and every item the issue names has
changed.

## Finishing

Run step 4's gates with `--workspace` in place of `-p <crate>`, and check the blind export of each
crate whose `src/` you edited. The PR body adds:

- the items changed, and the tests in `tests/` that use them, by file: the coordinator reopens
  their test tasks;
- each old item kept beside its replacement.

A change to one of the three contracts goes to Alex, as SKILL.md step 5 says.
