# Ending a milestone

A milestone ends when every one of its `test` and `impl` tasks is closed. The commands below come
from `xtask machine` (#104), `xtask checklist` (#49) and `xtask bundle` and `xtask deploy` (#102,
#103); their issues say how to run them.

1. **Machine tier.** Run the full `cargo xtask machine` on the Mac and `lebig`, with the
   milestone's perf gate: input budgets at M1, media budgets at M3. Each failure becomes a
   `build:triage` issue under #24 with the failing log lines, or `needs:alex` if only Alex can
   judge it.
2. **Checklist.** Run `cargo xtask checklist M<n>`, with an `--only <rule-id prefix>` per rule
   group in the milestone's slice. It opens the milestone's issue for Alex, labelled `needs:alex`,
   linking the newest report from step 1. His ticks verify the `human`-tier rules. Run it again
   after a fix: it updates the same issue and keeps his ticks.
3. **Build.** Build `2.0.0-m<n>` and deploy it to both machines. From M2 on, Alex uses it daily.
4. **Record.** Tick the milestone in the epic's Milestones section, and name the build, the
   machine run and the checklist issue in the session summary.

Done when the machine tier passes, the checklist issue is open and `2.0.0-m<n>` runs on both
machines.

The milestone ends here, without a whole-branch code review and fix pass: the per-crate review
runs once, at the end of M4, as #23's "Done, and landing" says.
