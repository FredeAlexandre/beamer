//! The commands the CI workflow and the nightly run (#33): `cargo xtask crates`, `nextest` and
//! `mutants`, each run on a throwaway workspace, committed to its own git repo.

use std::fs;
use std::path::Path;
use std::process::Command;

struct Workspace {
    dir: tempfile::TempDir,
}

impl Workspace {
    /// A workspace holding a library crate per name in `crates`, committed.
    fn new(crates: &[&str]) -> Self {
        let ws = Workspace {
            dir: tempfile::tempdir().expect("a temp dir"),
        };
        ws.write(
            "Cargo.toml",
            "[workspace]\nresolver = \"3\"\nmembers = [\"crates/*\"]\n",
        );
        ws.write(".gitignore", "/target/\n/mutants.out*/\n");
        for name in crates {
            ws.write(
                &format!("crates/{name}/Cargo.toml"),
                &format!("[package]\nname = \"{name}\"\nversion = \"0.0.0\"\nedition = \"2024\"\n"),
            );
            ws.write(&format!("crates/{name}/src/lib.rs"), "");
        }
        ws.git(&["init", "--quiet", "--initial-branch=main"]);
        ws.commit("the workspace");
        ws
    }

    fn root(&self) -> &Path {
        self.dir.path()
    }

    fn write(&self, rel: &str, text: &str) {
        let path = self.root().join(rel);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    fn commit(&self, message: &str) {
        self.git(&["add", "--all"]);
        self.git(&["commit", "--quiet", "--allow-empty", "-m", message]);
    }

    fn git(&self, args: &[&str]) {
        let out = Command::new("git")
            .args(["-c", "user.name=Fixture", "-c", "user.email=f@example.com"])
            .args(["-c", "core.autocrlf=false", "-c", "commit.gpgsign=false"])
            .args(args)
            .current_dir(self.root())
            .output()
            .expect("git runs");
        assert!(out.status.success(), "git {args:?} failed: {out:?}");
    }

    /// `cargo xtask <args>` from the workspace's root, with its own target dir.
    fn xtask(&self, args: &[&str]) -> Run {
        let out = Command::new(env!("CARGO_BIN_EXE_xtask"))
            .args(args)
            .current_dir(self.root())
            .env("CARGO_TARGET_DIR", self.root().join("target"))
            .output()
            .expect("the xtask binary runs");
        Run {
            ok: out.status.success(),
            stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
            text: String::from_utf8_lossy(&out.stdout).into_owned()
                + &String::from_utf8_lossy(&out.stderr),
        }
    }

    fn crates(&self, runner: &str, job: &str) -> String {
        let run = self.xtask(&["crates", "--runner", runner, "--for", job]);
        run.passes();
        run.stdout.trim().to_owned()
    }
}

struct Run {
    ok: bool,
    stdout: String,
    /// stdout and stderr together, as a person reads them.
    text: String,
}

impl Run {
    #[track_caller]
    fn passes(&self) {
        assert!(self.ok, "expected a pass, got:\n{}", self.text);
    }

    #[track_caller]
    fn fails_naming(&self, needles: &[&str]) {
        assert!(!self.ok, "expected a failure, got a pass:\n{}", self.text);
        for needle in needles {
            assert!(
                self.text.contains(needle),
                "expected {needle:?} in:\n{}",
                self.text
            );
        }
    }
}

// `cargo xtask crates`: which crates each runner covers.

/// One of each group of the dependency table.
const EVERY_GROUP: &[&str] = &[
    "beamer-types",
    "beamer-input",
    "beamer-core",
    "beamer-platform",
    "beamer-mac",
    "beamer-win",
    "beamer-mac-ffi",
    "beamer-win-ffi",
    "beamer-opus-sys",
    "beamer-runtime",
    "beamer-viewmodel",
    "beamer-ui",
    "beamer",
    "beamer-sim",
];

#[test]
fn linux_tests_the_os_independent_crates() {
    let ws = Workspace::new(EVERY_GROUP);
    assert_eq!(
        ws.crates("linux", "test"),
        "-p beamer-core -p beamer-input -p beamer-platform -p beamer-sim -p beamer-types \
         -p beamer-viewmodel"
    );
}

#[test]
fn macos_and_windows_test_the_whole_workspace() {
    let ws = Workspace::new(EVERY_GROUP);
    assert_eq!(ws.crates("macos", "test"), "--workspace");
    assert_eq!(ws.crates("windows", "test"), "--workspace");
}

#[test]
fn each_crate_is_mutated_on_the_runner_of_its_os() {
    let ws = Workspace::new(EVERY_GROUP);
    assert_eq!(
        ws.crates("linux", "mutants"),
        "-p beamer-core -p beamer-input -p beamer-platform -p beamer-sim -p beamer-types \
         -p beamer-viewmodel"
    );
    assert_eq!(
        ws.crates("macos", "mutants"),
        "-p beamer -p beamer-mac -p beamer-runtime"
    );
    assert_eq!(ws.crates("windows", "mutants"), "-p beamer-win");
}

#[test]
fn xtask_is_tested_on_linux_and_never_mutated() {
    let ws = Workspace::new(&["beamer-types", "xtask"]);
    assert_eq!(ws.crates("linux", "test"), "-p beamer-types -p xtask");
    assert_eq!(ws.crates("linux", "mutants"), "-p beamer-types");
}

#[test]
fn a_runner_with_no_crate_prints_nothing() {
    let ws = Workspace::new(&["beamer-types"]);
    assert_eq!(ws.crates("windows", "mutants"), "");
}

#[test]
fn a_crate_missing_from_the_dependency_table_fails() {
    let ws = Workspace::new(&["beamer-types", "beamer-nowhere"]);
    ws.xtask(&["crates", "--runner", "linux", "--for", "mutants"])
        .fails_naming(&["beamer-nowhere", "xtask/src/deps/table.rs"]);
}

// `cargo xtask nextest`: a spec test merged red, panicking at a `todo!()`, is pending.

/// The repo's nextest config: the gate reads the JUnit report of its `ci` profile.
const NEXTEST_CONFIG: &str = include_str!("../../.config/nextest.toml");

/// `beamer-types` with one implemented function, one `todo!()`, and a spec test of each.
fn half_built() -> Workspace {
    let ws = Workspace::new(&["beamer-types"]);
    ws.write(".config/nextest.toml", NEXTEST_CONFIG);
    ws.write(
        "crates/beamer-types/src/lib.rs",
        "pub fn add(a: u32, b: u32) -> u32 {\n    a + b\n}\n\n\
         pub fn sub(_a: u32, _b: u32) -> u32 {\n    todo!()\n}\n",
    );
    ws.write(
        "crates/beamer-types/tests/spec.rs",
        "#[test]\nfn add_adds() {\n    assert_eq!(beamer_types::add(2, 3), 5);\n}\n\n\
         #[test]\nfn sub_subtracts() {\n    assert_eq!(beamer_types::sub(5, 3), 2);\n}\n",
    );
    ws.commit("half built");
    ws
}

#[test]
fn a_test_that_panics_at_a_todo_is_pending_and_passes_the_gate() {
    let ws = half_built();
    let run = ws.xtask(&["nextest", "--runner", "linux"]);
    run.passes();
    assert!(
        run.text.contains("pending") && run.text.contains("sub_subtracts"),
        "the red test isn't listed as pending:\n{}",
        run.text
    );
}

#[test]
fn a_test_failing_any_other_way_fails_the_gate() {
    let ws = half_built();
    ws.write(
        "crates/beamer-types/tests/wrong.rs",
        "#[test]\nfn add_is_wrong() {\n    assert_eq!(beamer_types::add(2, 3), 6);\n}\n",
    );
    ws.xtask(&["nextest", "--runner", "linux"])
        .fails_naming(&["add_is_wrong"]);
}

// `cargo xtask mutants`: the mutants gate, counting only `tests/`.

/// The repo's mutants config: the test filter the gate and the nightly share.
const MUTANTS_CONFIG: &str = include_str!("../../.cargo/mutants.toml");

/// [`half_built`] with the repo's mutants config, on a branch cut from `main`.
fn task_branch() -> Workspace {
    let ws = half_built();
    ws.write(".cargo/mutants.toml", MUTANTS_CONFIG);
    ws.commit("the mutants config");
    ws.git(&["switch", "--quiet", "-c", "task"]);
    ws
}

fn mutants_gate(ws: &Workspace) -> Run {
    ws.xtask(&["mutants", "--runner", "linux", "--in-diff", "main"])
}

#[test]
fn a_weak_spec_test_fails_the_gate_even_when_a_unit_test_kills_the_mutant() {
    let ws = task_branch();
    ws.write(
        "crates/beamer-types/src/lib.rs",
        "pub fn add(a: u32, b: u32) -> u32 {\n    a + b\n}\n\n\
         pub fn sub(_a: u32, _b: u32) -> u32 {\n    todo!()\n}\n\n\
         pub fn is_big(n: u32) -> bool {\n    n > 100\n}\n\n\
         #[cfg(test)]\nmod scaffolding {\n    #[test]\n    fn big() {\n        \
         assert!(super::is_big(101));\n        assert!(!super::is_big(100));\n    }\n}\n",
    );
    ws.write(
        "crates/beamer-types/tests/big.rs",
        "#[test]\nfn is_big_runs() {\n    let _ = beamer_types::is_big(7);\n}\n",
    );
    ws.commit("a weak test");
    mutants_gate(&ws).fails_naming(&["replace is_big -> bool with true"]);
}

#[test]
fn todo_bodies_and_the_red_tests_at_them_are_left_out_of_the_gate() {
    let ws = task_branch();
    ws.write(
        "crates/beamer-types/src/lib.rs",
        "pub fn add(a: u32, b: u32) -> u32 {\n    a + b\n}\n\n\
         pub fn sub(_a: u32, _b: u32) -> u32 {\n    todo!()\n}\n\n\
         pub fn mul(_a: u32, _b: u32) -> u32 {\n    todo!(\"in the next group\")\n}\n\n\
         pub fn is_big(n: u32) -> bool {\n    n > 100\n}\n",
    );
    ws.write(
        "crates/beamer-types/tests/more.rs",
        "#[test]\nfn mul_multiplies() {\n    assert_eq!(beamer_types::mul(2, 3), 6);\n}\n\n\
         #[test]\nfn is_big_above_100() {\n    assert!(beamer_types::is_big(101));\n    \
         assert!(!beamer_types::is_big(100));\n}\n",
    );
    ws.commit("a new todo!(), its red test, and a strong test");
    let run = mutants_gate(&ws);
    run.passes();
    assert!(
        run.text.contains("mul_multiplies, red at a todo!()")
            && run.text.contains("leaving out 2 todo!() bodies")
            && run.text.contains("mutants tested")
            && !run.text.contains("replace mul"),
        "expected the red test and both todo!() bodies left out, and is_big's mutants tested:\n{}",
        run.text
    );
}

#[test]
fn a_change_outside_the_runner_s_code_passes_without_mutating() {
    let ws = task_branch();
    ws.write("README.md", "Docs only.\n");
    ws.commit("docs");
    let run = mutants_gate(&ws);
    run.passes();
    assert!(
        run.text.contains("nothing to mutate"),
        "expected a word that nothing was mutated:\n{}",
        run.text
    );
}

// Cross-crate kills (#135): a rule's spec tests sit behind the highest seam that reaches it, often
// another crate's tests/. A mutant counts as caught when the tests/ of any crate that depends on
// the mutated one kill it.

/// `beamer-types` with `is_big`, which only `beamer-sim`'s spec tests test; `beamer-sim` depends on
/// it. On a branch cut from `main`, with the repo's mutants and nextest configs.
fn tested_from_sim() -> Workspace {
    let ws = Workspace::new(&["beamer-types", "beamer-sim"]);
    ws.write(".config/nextest.toml", NEXTEST_CONFIG);
    ws.write(".cargo/mutants.toml", MUTANTS_CONFIG);
    ws.write(
        "crates/beamer-sim/Cargo.toml",
        "[package]\nname = \"beamer-sim\"\nversion = \"0.0.0\"\nedition = \"2024\"\n\n\
         [dependencies]\nbeamer-types = { path = \"../beamer-types\" }\n",
    );
    ws.commit("beamer-sim depends on beamer-types");
    ws.git(&["switch", "--quiet", "-c", "task"]);
    ws.write(
        "crates/beamer-types/src/lib.rs",
        "pub fn is_big(n: u32) -> bool {\n    n > 100\n}\n",
    );
    ws.write(
        "crates/beamer-sim/tests/spec.rs",
        "#[test]\nfn is_big_above_100() {\n    assert!(beamer_types::is_big(101));\n    \
         assert!(!beamer_types::is_big(100));\n}\n",
    );
    ws.commit("is_big, tested from beamer-sim");
    ws
}

#[test]
fn the_gate_counts_a_mutant_killed_by_a_dependent_crate_s_spec_tests() {
    let ws = tested_from_sim();
    let run = mutants_gate(&ws);
    run.passes();
    assert!(
        run.text
            .contains("running the spec tests of beamer-sim, beamer-types")
            && run.text.contains("5 mutants tested")
            && run.text.contains(": 5 caught"),
        "expected is_big's 5 mutants caught by beamer-sim's tests:\n{}",
        run.text
    );
}

#[test]
fn the_nightly_counts_a_mutant_killed_by_a_dependent_crate_s_spec_tests() {
    let ws = tested_from_sim();
    let run = ws.xtask(&[
        "mutants",
        "--runner",
        "linux",
        "--shard",
        "0/1",
        "--",
        "--no-fail-fast",
    ]);
    run.passes();
    assert!(
        run.text
            .contains("running the spec tests of beamer-sim, beamer-types")
            && run.text.contains("5 mutants tested")
            && run.text.contains(": 5 caught"),
        "expected is_big's 5 mutants caught by beamer-sim's tests:\n{}",
        run.text
    );
}

#[test]
fn a_dependent_crate_s_unit_tests_still_kill_no_mutant() {
    let ws = tested_from_sim();
    ws.write("crates/beamer-sim/tests/spec.rs", "");
    ws.write(
        "crates/beamer-sim/src/lib.rs",
        "#[cfg(test)]\nmod scaffolding {\n    #[test]\n    fn big() {\n        \
         assert!(beamer_types::is_big(101));\n        assert!(!beamer_types::is_big(100));\n    \
         }\n}\n",
    );
    ws.commit("is_big, unit-tested from beamer-sim");
    mutants_gate(&ws).fails_naming(&["replace is_big -> bool with true", "MISSED"]);
}

#[test]
fn a_mutant_no_spec_test_reaches_is_missed() {
    let ws = tested_from_sim();
    ws.write("crates/beamer-sim/tests/spec.rs", "");
    ws.commit("no test of is_big");
    mutants_gate(&ws).fails_naming(&["replace is_big -> bool with true", "MISSED"]);
}

#[test]
fn a_dependent_crate_s_red_test_and_the_todo_it_meets_are_left_out() {
    let ws = tested_from_sim();
    ws.write(
        "crates/beamer-types/src/lib.rs",
        "pub fn is_big(n: u32) -> bool {\n    n > 100\n}\n\n\
         pub fn is_small(_n: u32) -> bool {\n    todo!()\n}\n",
    );
    ws.write(
        "crates/beamer-sim/tests/small.rs",
        "#[test]\nfn is_small_below_10() {\n    assert!(beamer_types::is_small(9));\n}\n",
    );
    ws.commit("a todo!() in beamer-types and its red test in beamer-sim");
    let run = mutants_gate(&ws);
    run.passes();
    assert!(
        run.text
            .contains("leaving out beamer-sim::small is_small_below_10, red at a todo!()")
            && run.text.contains("leaving out 1 todo!() bodies")
            && run.text.contains(": 5 caught")
            && !run.text.contains("replace is_small"),
        "expected the red test and the todo!() body left out, and is_big's mutants caught:\n{}",
        run.text
    );
}

#[test]
fn only_the_crates_that_reach_a_mutated_crate_run_their_tests() {
    // beamer-core reaches beamer-types through a dependency, beamer-sim through beamer-core, and
    // beamer-viewmodel through a dev-dependency on beamer-sim. beamer-input doesn't reach it.
    let ws = Workspace::new(&[
        "beamer-types",
        "beamer-core",
        "beamer-sim",
        "beamer-viewmodel",
        "beamer-input",
    ]);
    ws.write(".config/nextest.toml", NEXTEST_CONFIG);
    ws.write(".cargo/mutants.toml", MUTANTS_CONFIG);
    let manifest = |name: &str, deps: &str| {
        ws.write(
            &format!("crates/{name}/Cargo.toml"),
            &format!(
                "[package]\nname = \"{name}\"\nversion = \"0.0.0\"\nedition = \"2024\"\n\n{deps}"
            ),
        );
    };
    manifest(
        "beamer-core",
        "[dependencies]\nbeamer-types = { path = \"../beamer-types\" }\n",
    );
    manifest(
        "beamer-sim",
        "[dependencies]\nbeamer-core = { path = \"../beamer-core\" }\n",
    );
    manifest(
        "beamer-viewmodel",
        "[dev-dependencies]\nbeamer-sim = { path = \"../beamer-sim\" }\n",
    );
    ws.commit("the dependencies");
    ws.git(&["switch", "--quiet", "-c", "task"]);
    ws.write(
        "crates/beamer-types/src/lib.rs",
        "pub fn one() -> u32 {\n    1\n}\n",
    );
    ws.commit("a change in beamer-types");
    let run = mutants_gate(&ws);
    assert!(
        run.text.contains(
            "running the spec tests of beamer-core, beamer-sim, beamer-types, beamer-viewmodel\n"
        ),
        "expected every crate but beamer-input to run its tests:\n{}",
        run.text
    );
}
