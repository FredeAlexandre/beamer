---
name: build-skeleton-writer
description: Work one `build:skeleton` or `build:api` task of the Beamer 2.0 build. Writes one crate's public API with `todo!()` bodies from its spec domains, every item citing the rules it serves, or makes one change to that API after the skeleton.
---

# Skeleton writer

You work one `build:skeleton` or `build:api` task of the build epic, #24. A skeleton task writes
one crate's **public API**: its types, traits, functions and messages, with `todo!()` bodies, from
the rules of its spec domains and within the dependency rules of #16 and ADR 0004. A `build:api`
task changes that API after the skeleton: [api.md](api.md).

The skeleton is the build's **contract**. The test agent writes its tests against it, **blind**,
seeing only the skeleton, the spec and the glossary (ADR 0002). The implementer fills its bodies
and never changes it. Whatever a test needs to reach must be in it, and whatever is in it gets
tested and implemented.

## The role

- **Sees** everything: the spec, `GLOSSARY.md`, the ADRs, the decision tickets the issue names,
  the tracker, the whole workspace with the skeletons of the crates below yours, and the Python
  app at the tag `archive/python`, read as prior art for what a crate must offer. Where the Python
  app and a rule differ, the rule wins.
- **Touches** the task's crate (`Cargo.toml`, `src/`, `build.rs`, `clippy.toml`), its row in
  `xtask/src/deps/table.rs`, and `Cargo.lock`. A `build:api` task also touches callers in other
  crates' `src/` ([api.md](api.md)).
- **Leaves untouched**: every crate's `tests/`, `spec/`, the dependency rules themselves (the rest
  of `xtask`), and every other crate's public API. A rule you can't serve is a spec question
  (step 2), and a lower crate's API you need changed is a `build:api` issue on that crate.
- **Opens** only a spec question (step 2) and a `build:api` issue on a lower crate, each as a
  blocker. The coordinator cuts every other task.
- **Finishes** with a PR into `rust` that waits for its review, and a comment on the task
  (step 5).

## The contract

Every item you write meets these. Step 3 writes to them, and the reviewer reads against them.

- **Cited.** Every public item and every trait method carries one `// r[impl <id>]` line per rule
  it serves, at the rule's current version, between its doc comment and the item, as in
  `xtask/tests/fixtures/spec-example/crates/beamer-example/src/lib.rs`. An item that serves rules
  only by carrying data for other items (an id, a field's type) cites the rules of the items that
  need it. tracey also counts each `impl` block and `mod` declaration as code: each cites the rules
  of the items inside it. An item no rule needs stays out of the skeleton.
- **Observable.** Each rule has a public path through which a test can provoke its behaviour and
  see the outcome, at the highest seam that reaches the rule (the seams are in #24's Testing
  Decisions). The test agent sees nothing private.
- **Sans-I/O** for every crate `xtask deps` groups so: a stateful part is a state machine with
  `handle(event, now)`, `poll()` and `next_deadline()`, and time and randomness come in as
  arguments (ADR 0004). A domain's messages live in its own crate, for `beamer-wire` to compose.
- **Blind-safe.** Doc comments say what an item does, in the rule's words, never how: the test
  agent reads them. `xtask blind` keeps the body of a `const fn` and of a function returning
  `impl Trait`, so return named types, and make a function `const` only when a rule needs it at
  compile time.
- **Glossary words** name every item, field and variant.
- **Bodies are `todo!()`**. Data is real: struct fields, enum variants and the values of
  constants a rule fixes are part of the contract.

## Steps

### 1. Pick up the task

1. Read the issue and its comments. With two `Session failed:` comments on it, stop here: the
   task goes back to the coordinator, and a third session as it stands is ruled out. Report that
   and end.
2. If `origin/build/<n>-<slug>` exists, an earlier session left a PR there: continue it with
   `git reset --hard origin/build/<n>-<slug>`. Each finding of the PR's latest `Spec review:`
   comment, each comment from Alex, and each blocker it names, is this session's work.
3. On a `build:api` task, follow [api.md](api.md). On `beamer-opus-sys` or
   `beamer-speexdsp-sys`, also read [sys.md](sys.md).
4. Check that every workspace crate yours will depend on has its skeleton on `rust`. A missing
   one is a missing blocked-by edge: comment on the task naming it, and stop. The coordinator adds
   the edge.

### 2. Read the rules

A domain crate serves `spec/<domain>.md`, where the domain is the crate's suffix (#20). Every
other crate serves rules from several domains, for the behaviour its issue gives it.
`tracey query uncovered` lists the rules no crate cites yet, which are the candidates. Read them
with the glossary and the decision tickets the issue names.

A rule you can't serve through an API (it contradicts another rule, needs a dependency the rules
forbid, or names nothing a test can observe) is a **spec question**, and so is anything the issue
asks for that no rule covers. Open `Triage: <crate> skeleton: <the question>`, label
`build:triage`, with the rule ids and what blocks the API. Make it a sub-issue of #24 and mark the
task blocked by it, with the mutations in
[../build-coordinator/cutting.md](../build-coordinator/cutting.md). Leave its items out and write
the rest.

Done when every rule your crate's scope reaches is either yours, with the item that will serve
it, or another crate's, with that crate named.

### 3. Write the API

`crates/README.md` says what a new crate needs: its layout, its row in `table.rs` (a crate listed
under an old name gets the glossary's), and `clippy.toml` for a sans-I/O crate. Add only the
dependencies its row allows. Then write each item to the contract.

When a lower crate's API lacks something yours needs, open its `build:api` issue as a blocker, as
[../build-implementer/blocked.md](../build-implementer/blocked.md) says under "A public API
change", and write the rest.

Done when `cargo build -p <crate>` passes and every rule of yours from step 2 is cited by an item.

### 4. Pass the gates

Rebase onto the newest `rust` (`git fetch origin && git rebase origin/rust`), then:

```sh
cargo fmt --check
cargo clippy -p <crate> --all-targets -- -D warnings
cargo xtask deps
cargo xtask spec validate
tracey query unmapped --path crates/<crate>/src   # 0 unmapped: every item cites a rule
export=$(mktemp -d)/export
cargo xtask blind "$export"                       # no `kept a body` line for your crate
(cd "$export" && cargo check -p <crate> --all-targets)
```

`tracey query unmapped` counts per file without naming items: open a file it counts and find the
item, method, `impl` block or `mod` line with no `r[impl]` line. Then run every job of the CI workflow in
`.github/workflows/` that covers your crate, the way it runs them. A skeleton adds no tests, so a
mutants job finds nothing to kill its `todo!()` mutants with: when one flags them, say so in the PR
and leave it to the coordinator.

Done when every command above and every such job passes on your OS.

### 5. Finish

1. **Commit** the crate, its `table.rs` row and `Cargo.lock`.
2. **PR.** Push `build/<n>-<slug>` and open the PR with the task's label:
   `gh pr create -R FredeAlexandre/beamer --base rust --title '<issue title> (#<n>)' --label <build:skeleton | build:api>`.
   Its body names #<n>, lists the rule ids the crate cites, the rules left to other crates (with
   the crate), and any blocker. Open it with `--draft` while a blocker is open. Leave auto-merge
   off: the PR merges only after its review.
3. **Review.** The coordinator dispatches the spec reviewer on every skeleton and API PR. Alex
   reviews the three **contracts**: `beamer-types`, `beamer-core`'s `Event` and `Command`, and
   `beamer-platform`'s traits. A PR that writes or changes one of them gets the `needs:alex`
   label, on the PR and on the task, with one comment on the task naming the contract's items and
   their file paths, for Alex to approve or reject with what instead. The reviewer judges the rest
   of the PR and leaves the contract to Alex.
4. **Comment** on the task: the PR, who reviews it, the rules left to other crates, and each
   blocker.

On `lebig`, `gh` is not logged in: commit everything, push nothing, and put the PR body, the review
it needs and each issue you would have opened, title and body, in your report. The coordinator does
the GitHub steps from the Mac.

Report to the coordinator: the PR URL (or the last commit, on `lebig`), who reviews it, the rules
left to other crates, and each blocker. Leave merging and closing the issue to the coordinator.
