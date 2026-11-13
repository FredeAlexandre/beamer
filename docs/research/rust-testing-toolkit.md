# Rust testing toolkit for tests with teeth

Research for [#10](https://github.com/FredeAlexandre/beamer/issues/10), part of the Rust port map
[#1](https://github.com/FredeAlexandre/beamer/issues/1). Checked against primary sources on
2026-11-13: tool repositories, their books and changelogs, crates.io metadata, and the tools' own
CI runs. Sources are linked inline and listed at the end.

## The question

Which Rust tools make the four test rules from the map enforceable in practice, and what do they
cost to run? The rules, as numbered in this note:

- **R1 Separation.** Tests are written first, from the spec, by an agent that has not seen the
  implementation. A different agent implements against them.
- **R2 Mutation gate.** `cargo-mutants` runs in CI. Every surviving mutant is killed or justified.
- **R3 Traceability.** Every test names the spec rule or invariant it guards. A test that guards
  nothing is deleted.
- **R4 Properties over examples.** State machines get property and model-based tests. Every parser
  gets fuzzed.

## Short answer

- **R2** is `cargo-mutants` 27.x. It is mature, active and made for CI: `--in-diff` on every
  change, a sharded full run on a schedule. Its real cost is roughly one incremental build plus
  one test run per mutant. On its own CI that is about 9 s per mutant, and 780 mutants take ten
  runners about 15 minutes. Two traps matter for Beamer. It does not understand `#[cfg(target_os)]`,
  so platform code must be mutated on its own OS. Random tests (proptest, bolero) must run with
  fixed seeds during a mutation run.
- **R4** is proptest plus `proptest-state-machine` for state machines (crossing, ownership,
  pairing, link lifecycle), and bolero for parsers. A bolero harness is an ordinary `#[test]`, so
  it runs under `cargo test` on stable on both OSes, replays its corpus, and counts for
  cargo-mutants. The same harness is fuzzed with libFuzzer (`cargo bolero test`) on a schedule.
  shuttle covers thread interleavings (loom only for small lock-free cores). Two peers in one
  process means a **sans-I/O protocol core** pumped by a deterministic harness, with turmoil only
  for the thin tokio shell.
- **R3** is a `// r[verify rule.id]` comment above each test, in the syntax of
  [tracey](https://github.com/bearcove/tracey), against `r[rule.id]` markers in the Markdown spec.
  A small in-repo `xtask test-audit` fails CI when a test has no `verify` line or the id does not
  exist. "Guards nothing" is caught in layers: no assertion (static), passes against the `todo!()`
  skeleton (red-first gate), kills no mutant (a nightly kill matrix built on cargo-mutants'
  diffs).
- **R1** is mostly workflow, but three mechanical checks back it up. Spec tests live in each
  crate's `tests/` directory, so they can only reach the public API. They must fail against the
  skeleton before the implementation exists. Implementation commits may not edit them.
- **Coverage** (`cargo-llvm-cov`) and **snapshots** (`insta`) are diagnostics, not gates. A
  snapshot recorded from the implementation's own output breaks R1 and is the textbook slop test.

The recommended toolkit, with the CI layout, is at the [end](#recommended-toolkit).

---

## 1. Mutation gate: cargo-mutants (R2)

### Maturity

- Version 27.1.0 was released 2026-06-02. The repository has unreleased work on `main` and was
  last pushed 2026-10-19. It has about 1.4k stars and 914k downloads
  ([releases](https://github.com/sourcefrog/cargo-mutants/releases),
  [NEWS.md](https://github.com/sourcefrog/cargo-mutants/blob/main/NEWS.md)). The author is
  Martin Pool.
- The [book](https://mutants.rs/) is complete and current. Output formats and CLI may still change
  between releases ([limitations](https://mutants.rs/limitations.html)), so pin the version in CI.

### What it mutates

Each mutant replaces a function body with a type-appropriate value (`Ok(Default::default())`,
`0`/`1`/`-1`, `true`/`false`, `vec![]`, `None`, and so on). It also swaps binary operators (`==`
and `!=`, `<` and `>`, `&&` and `||`, `+` and `-`), deletes `!` and unary `-`, deletes match arms
when a wildcard arm exists, replaces match guards with `true`/`false`, and deletes fields from
struct literals that use `..Default::default()`
([mutation patterns](https://mutants.rs/mutants.html)). Several exclusions are automatic: test
functions (any attribute whose path ends in `test`, such as `#[tokio::test]`), `#[cfg(test)]`
code and `unsafe fn`.

### Cost in practice

- **The model.** Per mutant there is one incremental `cargo test --no-run` build, plus the tests of
  the mutated package. A shard additionally pays for a clean build and a baseline test run
  ([sharding](https://mutants.rs/shards.html), [performance](https://mutants.rs/performance.html)).
  Total CPU is about `N_mutants × (incremental build + test)`.
- **Measured on cargo-mutants' own scheduled CI**
  ([run 42473585237](https://github.com/sourcefrog/cargo-mutants/actions/runs/42473585237),
  2026-11-08). Shard 0 of 10 found 78 mutants, so about 780 in total for roughly 470 KB of Rust
  under `src/`. Incremental builds took 4 to 6 s each, using the Wild linker and nightly
  `-Zunstable-options --fail-fast`. Shards took 8 to 15 minutes each, so the whole run took about
  15 minutes of wall time and about 115 runner-minutes.
- **Extrapolation to Beamer (my estimate, not measured).** The Python is 86k lines. A Rust port of
  similar size, at cargo-mutants' density, gives a few thousand mutants. At about 10 s each that
  is several CPU-hours for a full run. That fits a nightly job sharded over 8 to 16 runners, not a
  per-PR job. Per PR, `--in-diff` usually leaves tens of mutants, which takes minutes.
- **Levers the book documents.**
  - A `[profile.mutants]` with `debug = "none"`.
  - Skipping doctests with `-- --all-targets`.
  - A faster linker: mold is about 20% faster, and Wild more than halved one tree's run.
  - A ramdisk `TMPDIR`.
  - `-j2`/`-j3` at most. The book warns against higher values.
  - Keeping each crate's own tests fast, because by default a mutant only runs the tests of its
    own package ([workspaces](https://mutants.rs/workspaces.html)).

  Crate granularity is therefore a performance lever.
- **nextest** is supported (`--test-tool=nextest`) but is not automatically faster. It does not run
  doctests ([nextest chapter](https://mutants.rs/nextest.html)).

### CI modes

- **Incremental:** `cargo mutants --in-diff git.diff`, fed with `git diff origin/main..`. The book
  ships a GitHub Actions workflow for it
  ([in-diff](https://mutants.rs/in-diff.html),
  [example workflow](https://github.com/sourcefrog/cargo-mutants/blob/main/examples/workflows/in-diff.yml)).
  The author's caveats: a diff that only changes tests runs no mutants, and edits can weaken tests
  of untouched code. So incremental runs complement the full run and do not replace it.
- **Full:** `--shard k/n` in a CI matrix, with `--baseline=skip` when a previous step already ran
  the tests, and `--in-place` on throwaway checkouts. Each shard should get at least 10 mutants;
  8 to 32 shards is the suggested start ([sharding](https://mutants.rs/shards.html),
  [CI](https://mutants.rs/ci.html)). `taiki-e/install-action` installs a prebuilt binary.
- **Local loop:** `--iterate` skips mutants that were already caught
  ([iterate](https://mutants.rs/iterate.html)). Missed mutants show as GitHub annotations
  automatically.
- **Output:** `mutants.out/` holds `missed.txt`, `caught.txt`, `outcomes.json`, and one diff per
  mutant under `diff/` ([mutants.out](https://mutants.rs/mutants-out.html)). Section 9 builds on
  those diffs.

### Exclusions, and how "justified" becomes mechanical

- In code: `#[mutants::skip]` on a function, impl, trait, module, file, const or expression. It
  needs the tiny `mutants` crate as a normal dependency. The book suggests a comment explaining
  why ([attrs](https://mutants.rs/attrs.html)). The finer `#[mutants::exclude_re("…")]` is
  documented, but it is under *Unreleased* in NEWS and needs `mutants` 0.0.5, while crates.io has
  0.0.4. **Do not rely on it until it ships.**
- In config: `exclude_re`, `examine_re`, file globs and `skip_calls` in `.cargo/mutants.toml`
  ([filtering](https://mutants.rs/filter_mutants.html), [skip calls](https://mutants.rs/skip_calls.html)).
  Since 27.0 the command-line filters add to the config filters instead of replacing them.
- Recommended justification rule: every `mutants::skip` and every `exclude_re` entry carries a
  `// mutants-justified: <reason> (rule id)` comment, and `xtask test-audit` rejects bare ones. A
  justification is then reviewable in the diff like any other code.

### Traps for Beamer

1. **`cfg` is not understood.** "cargo-mutants does not yet understand conditional compilation,
   such as `#[cfg(target_os = "linux")]`. It will report functions for other platforms as missed"
   ([limitations](https://mutants.rs/limitations.html); still open as
   [issue #50](https://github.com/sourcefrog/cargo-mutants/issues/50)). cargo-mutants' own CI
   works around it with `--exclude=windows.rs`. For Beamer, put platform code in per-OS crates or
   files, and run mutants for the mac crates on a Mac and the windows crates on Windows. That
   means `--package` or `--file` per runner. The platform seam that the architecture ticket draws
   should allow this.
2. **Random tests make verdicts flaky.** The author says cargo-mutants "can only help if the test
   suite is hermetic", and in an open issue about proptest he suggests one fixed
   `PROPTEST_RNG_SEED` per run, a higher `PROPTEST_CASES`, and committing failing seeds
   ([issue #661](https://github.com/sourcefrog/cargo-mutants/issues/661)). Do the same for bolero
   (`BOLERO_RANDOM_SEED`, `BOLERO_RANDOM_ITERATIONS`). Otherwise each bolero test runs for its
   default 1 s time budget under `cargo test`
   ([bolero-engine rng.rs](https://github.com/camshaft/bolero/blob/master/lib/bolero-engine/src/rng.rs)),
   which also costs 1 s per mutant.
3. **Tests in another package do not run.** By default a mutant only runs its own package's
   tests. If spec tests live in a separate crate, set `test_package` or `test_workspace`
   ([workspaces](https://mutants.rs/workspaces.html)), or keep them in the crate's own `tests/`
   directory, which is the recommendation below.
4. **Hangs.** A mutant like `should_stop() -> false` hangs. The default timeout is 5× the
   baseline, with a minimum of 20 s ([timeouts](https://mutants.rs/timeouts.html)). Long-running
   loops (link keep-alive, the capture loop) need skip attributes with a justification.

---

## 2. Property testing: proptest vs bolero vs quickcheck (R4)

| | proptest 1.12 | bolero 0.13 | quickcheck 1.1 |
|---|---|---|---|
| Activity | Release 2026-10-10; self-described "mainly passive maintenance", "close to feature-complete" ([README](https://github.com/proptest-rs/proptest)) | Release 0.13.7 on 2026-10-03; commits weekly ([repo](https://github.com/camshaft/bolero)) | 1.1.0 on 2026-02-10; occasional commits ([repo](https://github.com/BurntSushi/quickcheck)) |
| Use (crates.io recent downloads) | 75.7M | 1.5M | 15.7M |
| Generation | Explicit `Strategy` values: many strategies per type, constraints built in | `TypeGenerator` derive, or bytes; also driven by fuzz engines | One `Arbitrary` per type, newtypes for variants |
| Shrinking | Integrated, per value ([vs quickcheck](https://proptest-rs.github.io/proptest/proptest/vs-quickcheck.html)) | Supported on all engines ([book](https://camshaft.github.io/bolero/features/input-shrinking.html)) | Per type, binary search |
| Regressions | Failing **seeds** persisted to `proptest-regressions/`, meant to be committed ([failure persistence](https://proptest-rs.github.io/proptest/proptest/failure-persistence.html)) | `corpus/` and `crashes/` replayed by `cargo test` ([corpus replay](https://camshaft.github.io/bolero/features/corpus-replay.html)) | none built in |
| State machines | `proptest-state-machine` 0.9 (below) | `Vec<Op>` plus an oracle, by hand ([structured testing](https://camshaft.github.io/bolero/features/structured-testing.html)) | none |
| Coverage-guided | no | yes: libFuzzer, AFL, honggfuzz; Kani as a verifier ([unified interface](https://camshaft.github.io/bolero/features/unified-interface.html)) | no |
| Determinism knobs | `PROPTEST_RNG_SEED`, `PROPTEST_CASES`, `PROPTEST_RNG_ALGORITHM` ([config.rs](https://github.com/proptest-rs/proptest/blob/main/proptest/src/test_runner/config.rs)) | `BOLERO_RANDOM_SEED`, `_ITERATIONS`, `_TEST_TIME_MS` | `QUICKCHECK_TESTS`, … |
| Cost | Default 256 cases per test; complex values can be up to 10× slower to generate than quickcheck's (proptest's own note) | 1 s per test by default under `cargo test` | cheapest |

**Verdict.**

- **proptest** is the default for properties and state machines. It is the most used, it has the
  richest generation model, and it has the only maintained model-based layer.
- **bolero** is the choice for anything shaped like a parser, because the same harness fuzzes. AWS
  uses it this way in s2n-quic's codec crate
  ([example](https://github.com/aws/s2n-quic/blob/main/common/s2n-codec/src/decoder/checked_range.rs)).
- **quickcheck** brings nothing that proptest lacks.
- **Watch list:** [Hegel](https://github.com/hegeldev/hegel) (`hegeltest` on crates.io), a
  Hypothesis-based engine with a Rust front end. It is in beta, explicitly warns about breaking
  changes, and moves fast (0.52.2 on 2026-11-10). It is not ready to be the base of a big-bang
  build.

---

## 3. Model-based and state-machine testing (R4)

### proptest-state-machine

Version 0.9.0 was released 2026-10-10 from the proptest repository
([changelog](https://github.com/proptest-rs/proptest/blob/main/proptest-state-machine/CHANGELOG.md)).
The design follows Erlang's `eqc_statem`
([book chapter](https://proptest-rs.github.io/proptest/proptest/state-machine.html)):

- `ReferenceStateMachine` is the **abstract model** (`init_state`, `transitions`, `apply`,
  `preconditions`). It generates valid transition sequences.
- `StateMachineTest` drives the **real implementation** with the same transitions. Postconditions
  go in `apply`, and `check_invariants` runs after every step.
- `prop_state_machine! { #[test] fn name(sequential 1..40 => MyTest); }` produces an ordinary
  test. On failure it shrinks by deleting transitions, then simplifying each one, then the
  initial state.
- Limits: sequential only. A concurrent mode is "planned" but does not exist yet.

This shape fits R1 and R3 directly. The test-writing agent writes the **reference model from the
spec** without seeing the implementation, and each invariant asserted in `check_invariants`
carries its rule id. A sketch for Beamer's crossing, simplified the way the proptest book
simplifies its signatures:

```rust
// tests/crossing_model.rs, in the crossing crate, so only the public API is visible
struct OwnershipModel; // r[crossing.owner.unique]: exactly one machine owns the pointer

impl ReferenceStateMachine for OwnershipModel {
    type State = Model;        // who owns the pointer, which links are up, held keys
    type Transition = Step;    // PointerAtEdge(edge), LinkDown(peer), KeyDown(k), KeyUp(k), Tick(ms)
    fn init_state() -> BoxedStrategy<Model> { Just(Model::home()).boxed() }
    fn transitions(m: &Model) -> BoxedStrategy<Step> { step_strategy(m) }
    fn apply(m: Model, s: &Step) -> Model { m.after(s) }     // written from the spec
}

impl StateMachineTest for CrossingTest {
    type SystemUnderTest = Crossing;
    type Reference = OwnershipModel;
    fn init_test(_: &Model) -> Crossing { Crossing::new(fixture_layout()) }
    fn apply(mut sut: Crossing, m: &Model, s: Step) -> Crossing {
        let out = sut.handle(s.into(), s.now());
        // r[verify crossing.input.fence]: no input reaches a peer that does not own the pointer
        assert!(out.injected_to().all(|p| Some(p) == m.owner), "crossing.input.fence");
        sut
    }
    fn check_invariants(sut: &Crossing, m: &Model) {
        assert_eq!(sut.owner(), m.owner, "crossing.owner.unique");
        // r[verify crossing.keys.released-on-leave]
        assert!(sut.held_keys_on_others().is_empty() || m.owner_is_remote(), "crossing.keys.released-on-leave");
    }
}

prop_state_machine! {
    // r[verify crossing.owner.unique] r[verify crossing.input.fence] r[verify crossing.keys.released-on-leave]
    #[test]
    fn crossing_agrees_with_the_ownership_model(sequential 1..60 => CrossingTest);
}
```

Compare this with today's Python suite. `core/tests/test_crossing_state.py` has about 30
hand-picked examples, such as `test_old_peer_moves_once_and_answers_once` and
`test_majority_and_tie_have_one_owner_before_intent_narrows`. Each pins one scenario. A model
test explores sequences nobody wrote down and shrinks a failure to its shortest form. Those
examples are still worth keeping, but as regression cases next to the model, each with its rule
id.

### Alternatives considered

- **bolero with `Vec<Op>` and an oracle**: the same idea without preconditions or a separate model
  abstraction, but coverage-guided when fuzzed. Good for data structures (a key table, a
  clipboard chunk reassembler).
- **[stateright](https://github.com/stateright/stateright)**: an explicit-state model checker for
  actor systems, version 0.31.0, last commit 2025-07-27 (quiet). It could exhaustively check a
  small abstract model of the ownership/handover protocol between three machines. That is
  optional and outside the build's critical path.
- **[Kani](https://github.com/model-checking/kani)** 0.68, a bounded model checker (very active):
  it proves the absence of panics or overflow on small pure functions such as a length-prefix
  decoder, and runs through bolero's `--engine kani`. It is a later nice-to-have, not a gate.

---

## 4. Fuzzing every parser (R4)

| | cargo-fuzz 0.13.2 | bolero 0.13 |
|---|---|---|
| Engine | libFuzzer only | libFuzzer, AFL, honggfuzz, Kani, plus a random engine under `cargo test` |
| Toolchain | **nightly** required | random engine and corpus replay on **stable**; sanitizers need nightly ([rust-stable](https://camshaft.github.io/bolero/features/rust-stable.html)) |
| Platforms | README: x86-64/aarch64, "only on Unix-like operating systems (not Windows)". The Rust Fuzz Book has a newer [Windows page](https://rust-fuzz.github.io/book/cargo-fuzz/windows.html) using MSVC ASan, which contradicts the README | `cargo test` path: anywhere Rust runs (not verified on Windows); fuzz engines: Linux documented, macOS expected (not verified) |
| Harness | separate `fuzz/` crate, `#![no_main]` targets, **not run by `cargo test`** | a normal `#[test]` with `bolero::check!()` |
| Seen by cargo-mutants | no (unless the corpus is replayed by a separate test) | yes: corpus plus a random run on every `cargo test` |
| Structure-aware | `arbitrary`, and custom `fuzz_mutator!` (better coverage in the maintainers' [experiment](https://fitzgen.com/2026/06/01/structure-aware-fuzzing-experiment.html), cited in the [book](https://rust-fuzz.github.io/book/cargo-fuzz/structure-aware-fuzzing.html)) | `TypeGenerator` derive |
| CI recipe | Book: a matrix of targets, `-max_total_time=300` each ([fuzzing in CI](https://rust-fuzz.github.io/book/cargo-fuzz/ci.html)) | `cargo bolero test <target> -T 5min` (equivalent) |

**Verdict: bolero for every parser.** One harness per parser serves three purposes. It is a
property test in the normal suite on both OSes. It is a regression net through the committed
corpus. And it fuzzes coverage-guided in a scheduled job (on Linux or the Mac, with the nightly
toolchain). That keeps "every parser is fuzzed" visible to the mutation gate and to the
traceability checker, which a separate cargo-fuzz crate would not be. Example:

```rust
// r[verify wire.frame.decode-total]: decoding never panics, any input
// r[verify wire.frame.round-trip]: decode(encode(f)) == f
#[test]
fn frame_decoder_is_total_and_round_trips() {
    bolero::check!().for_each(|bytes: &[u8]| {
        if let Ok(frame) = Frame::decode(bytes) {
            assert_eq!(Frame::decode(&frame.encode()).as_ref(), Ok(&frame), "wire.frame.round-trip");
        }
    });
}
```

What "every parser" covers in Beamer:

- link frames
- pairing messages (the current `core/tests/pairing_vectors.json` CPace exchange shows how many
  fields are involved)
- the settings and peer store
- clipboard payloads
- audio and camera framing
- DDC replies

The **published test vectors stay as example tests**. Vectors from an RFC or a draft are
spec-derived by construction, which is exactly what R1 asks for. The Python
`test_pairing_cpace.py` already works this way.

`afl.rs` (`cargo afl`, active) is an alternative engine, also reachable through bolero.

---

## 5. Concurrency: loom vs shuttle

| | loom 0.7.3 | shuttle 0.9.10 |
|---|---|---|
| Approach | Exhaustive exploration of interleavings under the C11 memory model, with state reduction ([README](https://github.com/tokio-rs/loom)) | Randomized scheduling, including PCT with probabilistic guarantees ([README](https://github.com/awslabs/shuttle)) |
| Sound? | Within its model. It treats `SeqCst` as `AcqRel` (false alarms) and does not explore load buffering (can miss bugs) | No. A pass proves nothing, but it scales to much larger tests |
| Scale | Small tests only. Bound with `LOOM_MAX_PREEMPTIONS=2..3`, which the docs say catches most bugs ([lib.rs docs](https://github.com/tokio-rs/loom/blob/master/src/lib.rs)) | Whole components. Wrappers exist for tokio, parking_lot, rand, std sync ([wrappers](https://github.com/awslabs/shuttle/blob/main/wrappers/README.md)) |
| Integration | `RUSTFLAGS="--cfg loom"`, `--release`, swap imports | Cargo feature `shuttle` and wrapper crates, swap imports |
| Reproducible | yes | yes: a failing schedule replays deterministically |
| Activity | Release 2026-11-08 (the previous one was 2025-08) | Releases weekly (2026-10/11) |

**Verdict.**

- **shuttle** is the tool for Beamer's threaded components. Two examples: the hand and keep-alive
  shutdown that `test_hand_close_waits_for_keep_alive_thread_to_exit` tests today with real
  threads and a 50 ms wall-clock wait, and the audio and camera pipelines' producer/consumer
  hand-offs.
- **loom** is for small lock-free cores only: the ring buffer in the audio engine, if it is
  written with atomics.
- Both require routing sync primitives through a facade module (`crate::sync`) chosen by cfg or
  feature. That is an architecture constraint to record now.

---

## 6. Two peers in one process: deterministic network simulation

There are three options, in order of preference.

1. **A sans-I/O protocol core.** Make the link, pairing and crossing protocols pure state machines
   (`handle(input, now) -> outputs`) with no sockets or clocks inside. The pattern is proven in
   Rust by `quinn-proto`, the "deterministic state machine of the protocol which performs no I/O",
   whose test suite "uses simulated IO for reproducibility"
   ([quinn README](https://github.com/quinn-rs/quinn)). A test then holds a Mac peer and a PC peer
   in one process and pumps datagrams between them through a lossy pipe with a fake clock. The
   loss, delay and reorder schedule is generated by proptest, so a failure shrinks to the shortest
   bad schedule. This costs nothing at run time (microseconds per step), needs no runtime magic,
   works on stable on both OSes, and its tests are fully visible to cargo-mutants.
2. **[turmoil](https://github.com/tokio-rs/turmoil)** 0.7.2 (2026-04-24, commits this week) runs
   many hosts on one thread with simulated TCP and UDP, including UDP broadcast and multicast,
   which matters for discovery. It injects latency, drops, partitions and crashes from a seeded
   RNG ([changelog](https://github.com/tokio-rs/turmoil/blob/main/crates/turmoil/CHANGELOG.md)).
   Use it for the thin tokio shell around the core: reconnects, partitions, a peer restarting.
   Caveats:
   - The network layer is being split out into `turmoil-net` ("expect the shape of this crate's
     API to change"), and the unreleased changelog already has a breaking change.
   - Code must use `turmoil::net` types in tests, behind a facade.
   - Whether a QUIC stack (quinn) can run over turmoil's UDP was **not verified**.
3. **[madsim](https://github.com/madsim-rs/madsim)** replaces tokio wholesale (`--cfg madsim`)
   and requires `[patch.crates-io]` forks of getrandom and others. RisingWave uses it in
   production, but it was last pushed 2026-02-16. It is too invasive for this project.

**Verdict:** a sans-I/O core with a deterministic two-peer harness is the main tool. turmoil adds a
handful of end-to-end tests of the I/O shell. This is the strongest single architectural decision
for testability, and the architecture ticket should adopt it.

---

## 7. Snapshot testing: insta, and when it turns into slop

[insta](https://github.com/mitsuhiko/insta) 1.49 (2026-10-03) stores an expected rendering in a
`.snap` file. By default (`INSTA_UPDATE=auto`) it writes `.snap.new` files for review, never in
CI, and `cargo insta review` accepts them
([crate docs](https://github.com/mitsuhiko/insta/blob/master/insta/src/lib.rs)).

It **becomes slop** when the snapshot is produced by running the implementation and accepting the
output. The test then asserts "the code does what the code did". That breaks R1, since the oracle
came from the implementation and not the spec. It also passes R2 trivially, because every mutant
changes the output and is "caught", even when the recorded output was wrong in the first place. An
agent can mass-accept snapshots with `INSTA_UPDATE=always` or `cargo insta accept` and get a green
suite that encodes its bugs.

Acceptable uses:

- Human-facing text that a person reviews once, then freezes: the tray menu, notification
  strings, log lines that are part of the support contract.
- Golden files **written by hand from the spec before the code exists**, such as protocol frames
  as annotated hex. These count as spec vectors, not snapshots.

Guard rails if insta is used at all:

- CI runs with `INSTA_UPDATE=no` (the default under CI), never `INSTA_FORCE_PASS`, and with
  `--unreferenced=reject` so orphaned snapshots fail.
- A snapshot file change in an implementation commit needs the same review as a test change (see
  R1 below).

---

## 8. Coverage tools

- **[cargo-llvm-cov](https://github.com/taiki-e/cargo-llvm-cov)** 0.9.1 (2026-09) wraps rustc's
  `-C instrument-coverage`. It works on macOS, Windows and Linux, supports `cargo test` and
  `cargo nextest`, gives line and region coverage on stable (branch and MC/DC on nightly), offers
  `--fail-under-*` thresholds, and exports lcov, cobertura and codecov.
- **[tarpaulin](https://github.com/xd009642/tarpaulin)** 0.37.6 (2026-11-05) uses ptrace on Linux
  x86-64 and the same LLVM instrumentation on macOS and Windows. It adds nothing over
  cargo-llvm-cov here.

**Role: a diagnostic, not a gate.** The cargo-mutants author puts it well: coverage shows code
that is reached, "not whether the test really *checks* anything"
([vs coverage](https://mutants.rs/vs-coverage.html)). A coverage threshold invites exactly the
assertion-free tests that R3 forbids. Run cargo-llvm-cov in the nightly job to show
`--show-missing-lines` per crate (unreached code needs a test or a deletion). It is cheap: one
instrumented test run, plus a clean build of the instrumented crates.

---

## 9. Tying a test to a spec rule id (R3)

### Options

| Mechanism | How | Strength | Weakness |
|---|---|---|---|
| Naming | `fn crossing_owner_unique__two_peers_tie()` | zero tooling | ids with dots and versions do not fit identifiers; renames are silent |
| Attribute macro | `#[guards("crossing.owner.unique")]`, a no-op proc macro, read by a checker, or checked at compile time against a generated id list | the compiler rejects an unknown id | compile cost; does not work inside `proptest!` or `prop_state_machine!` bodies without macro gymnastics |
| `inventory`/`linkme` registry | the macro registers `(test, rule)` at link time ([inventory](https://github.com/dtolnay/inventory)) | queryable at run time | more machinery for little gain |
| **Comment annotation** | `// r[verify crossing.owner.unique]` above the test | works everywhere, including inside macros; tool-supported (below) | a convention, so it needs a checker |

### Existing tools

- **[tracey](https://github.com/bearcove/tracey)** 1.4.0 (bearcove; 2026-05 release, active
  2026-10, 259 stars) was made for exactly this.
  - Requirements are defined in the Markdown spec as `r[crossing.owner.unique]`, with an optional
    version suffix `+2`. Code references them as `// r[impl …]` and tests as `// r[verify …]`.
  - `tracey query untested` lists rules with no verifying test. `tracey query validate` (non-zero
    exit on errors) checks broken references. `tracey query stale` flags tests written against an
    older version of a rule's text, and a pre-commit hook refuses rule edits without a version
    bump.
  - It ships an LSP, an MCP server and a Claude Code skill (`tracey ai --claude`), so agents can
    query uncovered and untested rules directly.
  - It scans comments with tree-sitter in many languages, so the C++ HAL driver and virtual camera
    can carry annotations too.
  - **Not verified:** whether tracey flags a test that has *no* annotation. Its model is "rules →
    references", not "tests → rules".
- **[mantra](https://github.com/mhatzl/mantra)** 0.9.0 (2026-09) does requirement tracing with a
  SQL store and coverage-joined reports. It has very low adoption (130 recent downloads).
- **qa** (0.1.1, 2026-02, no repository listed) and **trace-doc** (0.1.0, 2026-01) are too young
  to rely on.

### Recommendation

Use tracey's syntax and run tracey in CI. Back it with a ~200-line in-repo checker
(`cargo xtask test-audit`) for the per-test direction that tracey does not cover:

1. Get the ground-truth list of tests from `cargo nextest list --message-format json` (it includes
   tests generated by `proptest!`, `prop_state_machine!` and bolero harnesses).
2. Locate each test's `fn` in source (with `syn`, or a token scan inside macro bodies). Collect the
   `r[verify …]` comments directly above it.
3. Fail when a test has no `verify` reference, or names an id that `tracey query validate` rejects.
   Also fail on `mutants::skip` or `exclude_re` without a `mutants-justified:` reason (section 1),
   and on the static "guards nothing" checks below.
4. Per crate, fail when a rule in that crate's spec section is `untested`, once the crate is past
   its skeleton stage.

Also put the rule id in the assertion message (`assert_eq!(…, "crossing.owner.unique")`), so a
failure names the rule it broke. Clippy cannot enforce this: `missing_assert_message` explicitly
ignores test functions
([source](https://github.com/rust-lang/rust-clippy/blob/master/clippy_lints/src/missing_assert_message.rs)).
The xtask can enforce it instead.

---

## 10. Detecting tests that guard nothing (R3)

No single tool does this. These layers do it cheaply, in order of cost:

1. **Static (per PR, about a second).** `xtask test-audit` rejects test bodies that contain no
   assertion: no `assert*!`, `prop_assert*!`, `panic!`, `#[should_panic]`, or `?` on a checked
   result. Clippy's default `assertions_on_constants` already rejects `assert!(true)`.
2. **Red first (per test PR; part of R1).** A spec test must **fail against the interface
   skeleton** (bodies are `todo!()`) before any implementation exists. A test that passes against
   `todo!()` either never calls the code or asserts something vacuous. CI runs the new tests at the
   skeleton commit and requires each one to fail with a panic, not a compile error. This costs one
   test run of the changed crate.
3. **Kill matrix (nightly).** cargo-mutants answers "which mutants survive", not "which tests kill
   anything". Its fail-fast runs stop at the first failing test, and per-test attribution is an
   open request ([#427](https://github.com/sourcefrog/cargo-mutants/issues/427),
   [#24](https://github.com/sourcefrog/cargo-mutants/issues/24)). The building blocks are there,
   though:
   - `cargo mutants --list --json` or `mutants.out/diff/` gives one patch per mutant.
   - For each mutant: apply the patch, build, and run `cargo nextest run --no-fail-fast` with
     JUnit output.
   - Record which tests failed.

   A test whose row in the matrix is empty kills no mutant: it guards nothing that cargo-mutants
   can express. The test is then either deleted or listed with a justification, for example when
   it guards a platform behaviour or a timing budget. The cost is the same number of builds as a
   full mutants run, but each mutant runs the whole crate suite without fail-fast. Run it nightly
   or weekly, sharded, and restrict it to crates whose tests changed.
4. **Per rule (on demand).** tracey knows a rule's `impl` sites and `verify` tests. Running
   `cargo mutants --file <impl files> -- -E 'test(=<name>)'` checks that the tests named for a rule
   kill mutants **in that rule's code**. That catches a test that is real but mislabelled.

Flaky tests are a "guards nothing" in disguise, because a mutation gate cannot trust them. Run the
suite under nextest with `--retries 1` and `--flaky-result fail`. By default nextest treats a test
that passes on retry as a success, and this setting turns it into a failure
([retries](https://nexte.st/docs/features/retries/)).

---

## 11. Making the separation rule mechanical (R1)

R1 is mostly orchestration (the map's "agent work breakdown" item), but four mechanical checks
back it up:

1. **Spec tests go in each crate's `tests/` directory.** Rust compiles those as separate crates
   that can only call the library's public API
   ([Rust Book 11.3](https://doc.rust-lang.org/book/ch11-03-test-organization.html)). They still
   belong to the same package, so cargo-mutants runs them by default (trap 3 above).
   Implementation-internal unit tests (`#[cfg(test)] mod tests`) remain allowed for the
   implementer, under the same R3 rules.
2. **Interfaces first.** A skeleton commit holds the public types and signatures with `todo!()`
   bodies, plus the spec section with its `r[…]` ids. The test agent works in a worktree containing
   only the skeleton and the spec.
3. **Red-first gate**, as in section 10, layer 2.
4. **Implementation commits do not touch spec tests.** `xtask test-audit --since <skeleton>` fails
   when an implementation change edits `tests/`, `proptest-regressions/`, bolero corpora or
   snapshots without a separate, reviewed test change. This stops the implementer from "fixing"
   the oracle.

---

## 12. Cost summary

| Tool | Per PR | Scheduled | CI setup | Rules |
|---|---|---|---|---|
| nextest (runner) | the suite, one process per test | n/a | `install-action`; `.config/nextest.toml` profile `ci` with retries=1 and flaky=fail | R3 (flakes) |
| proptest + proptest-state-machine | 256 cases per property (state machine: cases × sequence length); seconds per crate | `PROPTEST_CASES=10000` nightly | dev-dependency only; commit `proptest-regressions/` | R4, R1 (model from spec) |
| bolero | random run (bounded by `BOLERO_RANDOM_ITERATIONS`) plus corpus replay inside `cargo test` | `cargo bolero test` 5 min per parser, Linux or Mac, nightly toolchain; commit the corpus | `cargo-bolero` binary; nightly for sanitizers | R4 |
| cargo-mutants | `--in-diff`: tens of mutants, minutes; Mac and Windows crates on their own OS | full run, sharded: several CPU-hours estimated for a port of Beamer's size | `install-action`, `.cargo/mutants.toml`, `[profile.mutants]`, fixed seeds | R2, R3 |
| kill matrix (xtask on top of cargo-mutants) | n/a | about one full mutants run without fail-fast; weekly or changed crates | small custom script | R3 |
| tracey + `xtask test-audit` | about a second | n/a | tracey binary (prebuilt for mac, linux and windows); xtask crate | R3, R1 |
| shuttle | `check_random(…, N)`; seconds | larger N nightly | feature flag plus a sync facade | R4 (concurrency) |
| loom | small tests; `--release`, preemption bound 2 to 3 | n/a | `--cfg loom` build | R4 (concurrency) |
| sans-I/O harness | microseconds per simulated step | longer schedules nightly | none (in-repo) | R4, R1 |
| turmoil | a few end-to-end tests, seconds | n/a | dev-dependency; net facade | R4 |
| cargo-llvm-cov | not gated | one instrumented run, report | `install-action`; `llvm-tools-preview` | none (diagnostic) |
| insta | only for reviewed human-facing text | n/a | `cargo-insta` locally; `INSTA_UPDATE=no` in CI | none (at risk of violating R1) |

---

## Recommended toolkit

**Libraries (dev-dependencies)**

- `proptest` 1.12 + `proptest-state-machine` 0.9: every state machine (crossing and ownership,
  link lifecycle, pairing, shared-display handover, settings sync) gets a reference model written
  from the spec, plus regression examples.
- `bolero` 0.13: one harness per parser or decoder, with a committed corpus. Published test vectors
  (CPace and so on) stay as example tests.
- `shuttle` 0.9 for threaded components, and `loom` 0.7 only for small atomics-based cores. Both go
  through a `crate::sync` facade.
- A **sans-I/O protocol core** with an in-repo two-peer deterministic harness (fake clock, lossy
  pipe, proptest-generated schedules). `turmoil` 0.7 covers a few end-to-end tests of the tokio
  shell.
- `mutants` (the attribute crate) for justified skips.
- Not adopted: quickcheck, madsim, cargo-fuzz as a separate crate (bolero drives libFuzzer
  itself), tarpaulin, and insta except for reviewed human-facing text.

**Tools**

- `cargo-nextest` as the runner (CI profile: `retries = 1`, `flaky-result = "fail"`, JUnit).
- `cargo-mutants` 27.x (pinned), configured in `.cargo/mutants.toml`, with `profile = "mutants"`
  (no debug info) and a fast linker.
- `tracey` for spec ↔ code ↔ test traceability: `r[id]` in the spec, `// r[impl id]` and
  `// r[verify id]` in code.
- `cargo xtask test-audit` (in-repo). It checks that every test has a valid `verify` id and an
  assertion, that every mutants skip has a reason, and that implementation commits leave spec tests
  untouched. It also runs the red-first check and builds the kill matrix.
- `cargo-llvm-cov` as a nightly diagnostic.
- `cargo-bolero` for scheduled fuzzing.

**CI shape** (runner choice is the separate "Dev loop for Rust" item on the map)

- **Every PR, on both macOS and Windows:**
  - `nextest` with the CI profile
  - `xtask test-audit`
  - `tracey query validate`
  - `cargo mutants --in-diff`, with fixed seeds (`PROPTEST_RNG_SEED`, `BOLERO_RANDOM_SEED`,
    `BOLERO_RANDOM_ITERATIONS`), for the crates the OS can build, any missed mutant failing the job
  - the red-first check for PRs that add spec tests
- **Nightly:**
  - full `cargo mutants`, sharded per OS
  - the kill matrix for crates whose tests changed
  - `cargo bolero test` for each parser for a fixed time, with new corpus entries proposed as a PR
  - proptest at high `PROPTEST_CASES`
  - shuttle at high iteration counts
  - the coverage report

**Architecture constraints this implies** (for the architecture ticket)

- A sans-I/O core.
- OS-specific code in its own crates or files, because of cargo-mutants' cfg blindness.
- Sync and net facades, so shuttle and turmoil can be swapped in.
- Crates small enough that each one's own tests stay fast, because mutation cost is multiplied by
  that test time.
- A Markdown spec with `r[…]` rule ids as the single source of truth for tests.

## Not verified

- **Run times on Beamer's future workspace.** The cargo-mutants numbers come from its own CI, and
  the Beamer figures are an estimate. Windows link times, which multiply mutation cost, were not
  measured.
- **cargo-fuzz on Windows.** The README says no, but a Rust Fuzz Book page describes MSVC ASan
  support. This does not matter if fuzzing runs on Linux or the Mac.
- **bolero on macOS and Windows.** Neither the fuzz engines on macOS nor the `cargo test` path on
  Windows was run. The `cargo test` path is expected to work, since it is plain Rust in the random
  and replay modes.
- **tracey.** Whether it flags un-annotated tests, and how its Unix-socket daemon behaves on
  Windows. The prebuilt Windows binaries exist. The xtask covers the first gap, and running tracey
  on the Mac or Linux covers the second.
- **QUIC (quinn) over turmoil's simulated UDP.**
- **`mutants::exclude_re`.** Documented but unreleased (`mutants` 0.0.5 is not on crates.io as of
  today).

## Sources

- cargo-mutants: book <https://mutants.rs/> (chapters: performance, ci, in-diff, shards,
  parallelism, workspaces, attrs, filter_mutants, skip_calls, nextest, limitations, iterate,
  baseline, timeouts, mutants-out, vs-coverage, mutants); NEWS
  <https://github.com/sourcefrog/cargo-mutants/blob/main/NEWS.md>; issues
  [#50](https://github.com/sourcefrog/cargo-mutants/issues/50),
  [#661](https://github.com/sourcefrog/cargo-mutants/issues/661),
  [#427](https://github.com/sourcefrog/cargo-mutants/issues/427),
  [#24](https://github.com/sourcefrog/cargo-mutants/issues/24); CI workflow
  <https://github.com/sourcefrog/cargo-mutants/blob/main/.github/workflows/tests.yml> and run
  <https://github.com/sourcefrog/cargo-mutants/actions/runs/42473585237>.
- proptest: <https://github.com/proptest-rs/proptest> (README, CHANGELOGs, `config.rs`); book
  chapters state-machine, failure-persistence, vs-quickcheck, tutorial/config
  <https://proptest-rs.github.io/proptest/>.
- bolero: <https://github.com/camshaft/bolero>, book <https://camshaft.github.io/bolero/>,
  `lib/bolero-engine/src/rng.rs`; s2n-quic usage <https://github.com/aws/s2n-quic>.
- quickcheck: <https://github.com/BurntSushi/quickcheck>. Hegel: <https://github.com/hegeldev/hegel>.
- cargo-fuzz: <https://github.com/rust-fuzz/cargo-fuzz> (README, CHANGELOG); Rust Fuzz Book
  <https://rust-fuzz.github.io/book/> (ci, windows, structure-aware-fuzzing). afl.rs:
  <https://github.com/rust-fuzz/afl.rs>.
- loom: <https://github.com/tokio-rs/loom> (README, `src/lib.rs` docs). shuttle:
  <https://github.com/awslabs/shuttle> (README, wrappers/README).
- turmoil: <https://github.com/tokio-rs/turmoil> (README, crates/turmoil README and CHANGELOG).
  madsim: <https://github.com/madsim-rs/madsim>. quinn: <https://github.com/quinn-rs/quinn>.
- insta: <https://github.com/mitsuhiko/insta> (`insta/src/lib.rs` docs).
- cargo-llvm-cov: <https://github.com/taiki-e/cargo-llvm-cov>. tarpaulin:
  <https://github.com/xd009642/tarpaulin>.
- nextest: <https://nexte.st/docs/features/retries/>, <https://nexte.st/docs/listing/>.
- tracey: <https://github.com/bearcove/tracey> (README, docs/content/guide/annotating-code.md).
  mantra: <https://github.com/mhatzl/mantra>. crates.io metadata for qa, trace-doc, inventory, linkme.
- stateright: <https://github.com/stateright/stateright>. Kani: <https://github.com/model-checking/kani>.
- clippy lints: <https://github.com/rust-lang/rust-clippy/tree/master/clippy_lints/src>
  (`missing_assert_message.rs`, `assertions_on_constants.rs`).
- Rust Book, test organization: <https://doc.rust-lang.org/book/ch11-03-test-organization.html>.
- Versions, dates and download counts: crates.io API, `api/v1/crates?ids[]=…`, read 2026-11-13.
- Grounding in the current suite: `core/tests/test_crossing_state.py`,
  `core/tests/test_pairing_cpace.py`, `core/tests/pairing_vectors.json`.
