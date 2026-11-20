---
name: build-spec-miner
description: Work one `build:spec` task of the Beamer 2.0 build. Mines one domain of the Python app at `archive/python` into rules in `spec/<domain>.md`, and gives every Python test in the domain's ledger slice its fate.
---

# Spec miner

You work one stage 0 `build:spec` task of the build epic, #24: one **domain**. You read what the
Python app does in that domain, at the tag `archive/python`, and write it as **rules** in
`spec/<domain>.md`. Then you give every Python test in the domain's **slice** of the ledger,
`spec/ledger/<domain>.txt`, its **fate**. Nothing is translated: a rule states behaviour that a
test can observe, and the Rust code and tests are written from it later, by agents who never read
the Python (ADR 0002).

The spec is the build's one **oracle**. A rule you write is tested blind and implemented without
anyone reading the Python again, so whatever the app does that no rule says is lost. Parity is
the default (#11): a rule says what Python does, unless a decision says otherwise.

## The role

- **Sees** everything: the task issue and the tickets under its "Decided in", `spec/README.md`,
  `GLOSSARY.md`, the ADRs, the [feature inventory](https://github.com/FredeAlexandre/beamer/blob/research/feature-inventory/docs/research/feature-inventory.md)
  (`git show origin/research/feature-inventory:docs/research/feature-inventory.md`), the
  tracker, other domains' rules on `rust`, and the Python app.
- **Reads Python only from the read-only worktree** of `archive/python` (step 1): its tests, its
  code, its `git log`. Read and search there; never edit, build or run anything in it, since a run
  writes caches into it.
- **Touches** two files: `spec/<domain>.md`, and the fates in `spec/ledger/<domain>.txt`. The
  slice's test ids stay as #51 laid them out: you add a fate after each, and add no line. Three
  additions, only when they arise: a test [blocked.md](blocked.md) moves to another domain's slice,
  with its row in `spec/ledger/README.md`; a new `GLOSSARY.md` term (step 3); and a file the task
  issue itself says to delete (the `link` task deletes `WIRE.md`, `pairing` deletes `PAIRING.md`).
- **Leaves untouched**: every other domain's spec file, the rest of its ledger slice, `crates/`,
  `xtask/`, and everything else in the repo.
- **Opens** no issue. It comments on other domains' tasks and labels its own, as
  [blocked.md](blocked.md) says.
- **Finishes** with a PR into `rust` labelled `build:spec` that waits for its review, and a
  comment on the task (step 6).

## Steps

### 1. Pick up the task

1. Read the issue and its comments. With two `Session failed:` comments on it, stop here: the
   task goes back to the coordinator, and a third session as it stands is ruled out. Report that
   and end.
2. If `origin/build/<n>-<slug>` exists, an earlier session left a PR there: continue it with
   `git reset --hard origin/build/<n>-<slug>`. Each finding of the PR's latest `Spec review:`
   comment, each comment from Alex, each blocker it names, and the groups its `Partial:` comment
   leaves, are this session's work.
3. Read `spec/README.md`, then the reviewer's checks in
   [../build-spec-reviewer/rules.md](../build-spec-reviewer/rules.md): every rule you write is
   read against them, including "A whole domain, at stage 0".
4. Open the Python. `git fetch origin tag archive/python`, then find its worktree in
   `git worktree list`, at the commit `git rev-parse 'archive/python^{commit}'` prints. When there
   is none, make it: `git worktree add --detach ../beamer-python archive/python`.
5. Check that `spec/ledger/<domain>.txt` exists on `rust`. A missing slice is a missing
   blocked-by edge on #51: comment on the task saying so, and stop.

### 2. Map the domain

The issue's **Scope** line names the domain's features (F-numbers) and its decisions. For each
feature, read its row in the feature inventory and its outcome in #11's resolution (Ship, Later
or Drop, and what the design must leave room for). Then read the slice and sort every test in it
into a **behaviour group**: the tests and the code that exercise one behaviour of one feature.

Done when every test of the slice sits in exactly one of: a behaviour group, the dropped
features, the Python or Qt details, the wire-v6 details, or another domain's scope
([blocked.md](blocked.md)). An empty slice (the issue says "No Python tests") means the rules
come from the issue's decisions alone.

### 3. Write the rules

Work group by group. Read the group's tests for what they assert, then the code they exercise for
the cases the tests miss: a branch, a limit, an error path, an ordering. Each behaviour a user, a
peer or the OS can see becomes a rule under a heading for its topic. Commit each group's rules and
fates together, so a session that ends early leaves whole groups.

- **The behaviour, not the Python.** A rule says what is seen from outside, in `GLOSSARY.md`'s
  words: no Python class, Qt widget, thread or file name. A concept the glossary has no word for
  gets a term proposed in `GLOSSARY.md`, in its format (bold term, definition, `_Avoid_` list);
  the reviewer judges it like a rule.
- **Its source.** Each rule comes from Python, from a decision ticket or ADR the issue names, or
  both. A rule built on a published test vector (CPace's, say) names the vector's own source, so
  the test agent copies it as data from there.
- **Parity.** Where the rule differs from what Python does (a decision changed it, or Python's
  behaviour is a bug), its `parity:` marker says what Python did and, in the final parentheses,
  why the rule differs, naming the ticket or ADR when one decided it. `cargo xtask spec parity`
  gathers these into #73's one list for Alex, which groups the ones a ticket already decided.
- **Later and Drop** follow #11. A Later feature gets one rule naming the **room** the design
  leaves for it, from #11's Later table, stated as the behaviour that ships (every user-facing
  string passes through one translation function), never the Later feature itself. A Drop
  feature gets no rule, and its tests are `dropped:`.
- **Markers** from `spec/README.md`'s tables: a `tier:` with its reason when the rule is not
  `ci`; `model:` on every rule about one of the eight state machines; `fuzz:` on every rule about
  a parser, anything that turns bytes from outside the process into a value.
- **Ids** are new: `domain.topic.rule`, version 1, each never used before
  (`git log origin/rust --oneline -S 'r[<id>' -- spec/` prints nothing).

Done when every behaviour group has its rules, every feature in scope is covered as #11 says, and
`cargo xtask spec validate` passes.

### 4. Give every test its fate

Write each test's fate after its id, per `spec/README.md`'s ledger format:

- `mined: <rule id>, …` when its behaviour is in those rules, named without their version. A test
  that also touches another domain's behaviour names only the rules its own assertions became.
- `dropped: <reason>` when it tests a feature #11 drops, naming the row. Linux-only code is out of
  scope on the map (#1): `dropped: Linux-only, out of scope (#1)`.
- `python-detail: <reason>` only when it checks how Python does something and nothing a user,
  peer or the OS sees rides on it: a Qt repaint, a mock's call order, a Python thread. A test of a
  private Python function whose result reaches the outside is `mined:`.
- `wire-v6: <reason>` when it tests the v6 wire format that `spec/link.md` and
  `spec/pairing.md` replace.

Done when this prints nothing:

```sh
grep -Ev '^[[:space:]]*(#|$)' spec/ledger/<domain>.txt \
  | grep -Ev '[[:space:]](mined|dropped|python-detail|wire-v6):'
```

apart from the tests [blocked.md](blocked.md) leaves waiting, each listed in the PR body.

### 5. Pass the gates

Rebase onto the newest `rust` (`git fetch origin && git rebase origin/rust`), then:

```sh
cargo xtask spec validate
cargo xtask test-audit --since origin/rust
cargo xtask spec parity                         # your parity changes read as Alex will read them
.claude/skills/build-spec-reviewer/words.sh     # judge each hit as the reviewer will
git -C <python worktree> status --porcelain     # prints nothing: the Python stayed read-only
```

Then run every job of the CI workflow in `.github/workflows/` that covers `spec/`, the way it
runs them.

Done when every command passes and every `words.sh` hit is a word used in another sense.

### 6. Finish

1. **Commit** the spec file, the slice, and any glossary term or deletion from the role.
2. **PR.** Push `build/<n>-<slug>` and open the PR:
   `gh pr create -R FredeAlexandre/beamer --base rust --title '<issue title> (#<n>)' --label build:spec`.
   Its body names #<n>, and gives: the rule count by topic; the fates by kind
   (`grep -Eo '[[:space:]](mined|dropped|python-detail|wire-v6):' spec/ledger/<domain>.txt | sort | uniq -c`);
   each parity change, Later room and proposed glossary term; each test moved to another slice;
   each other domain's rule cited from an open PR; and each blocker. Open it with `--draft` while
   a blocker is open. Leave auto-merge off: the PR merges only after its review,
   which the coordinator dispatches.
3. **Comment** on the task: the PR, the parity-change count, and each blocker.

When the slice outgrows the session, finish the same way with a draft PR, and comment on the task
starting `Partial:`, with the groups done, the groups left, and a split you recommend (by
feature). The next session continues on the branch.

Report to the coordinator: the PR URL, the fates by kind, the parity changes, and each blocker or
group left. Leave merging and closing the issue to the coordinator.
