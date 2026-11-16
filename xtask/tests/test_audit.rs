//! `cargo xtask test-audit`: the checks that keep spec tests honest (#31, ADR 0002), run on
//! throwaway git repos with a spec, a crate and, for the ledger, Python tests at a tag.

mod common;

use common::Run;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// A spec with one rule at version 1 and one at version 2.
const SPEC: &str = "# Example\n\n\
r[example.zone.push]\nA push crosses.\n\n\
r[example.zone.held+2]\nThe pointer is held at edge.\n";

/// A spec test that keeps every rule.
const GOOD_TEST: &str = "\
// r[verify example.zone.push]
#[test]
fn a_push_crosses() {
    assert!(beamer_example::push(), \"example.zone.push\");
}
";

struct Repo {
    root: PathBuf,
}

impl Repo {
    /// A git repo named after the test, wiped first, holding the spec and one crate whose single
    /// spec test keeps every rule: the skeleton's commit, then the tests' commit.
    fn new(name: &str) -> Self {
        let root = Path::new(env!("CARGO_TARGET_TMPDIR"))
            .join("test-audit")
            .join(name);
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        let repo = Repo { root };
        repo.git(&["init", "-q", "-b", "rust"]);
        repo.git(&["config", "user.name", "Test"]);
        repo.git(&["config", "user.email", "test@example.com"]);
        repo.git(&["config", "commit.gpgsign", "false"]);
        repo.file("spec/example.md", SPEC);
        repo.file(
            "crates/beamer-example/Cargo.toml",
            "[package]\nname = \"beamer-example\"\n",
        );
        repo.file(
            "crates/beamer-example/src/lib.rs",
            "pub fn push() -> bool {\n    true\n}\n",
        );
        repo.commit("Skeleton");
        repo.file("crates/beamer-example/tests/zone.rs", GOOD_TEST);
        repo.commit("Tests");
        repo
    }

    fn file(&self, path: &str, text: &str) -> &Self {
        let path = self.root.join(path);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
        self
    }

    /// A spec test file of the example crate.
    fn test(&self, text: &str) -> &Self {
        self.file("crates/beamer-example/tests/zone.rs", text)
    }

    fn git(&self, args: &[&str]) -> String {
        let out = Command::new("git")
            .args(args)
            .current_dir(&self.root)
            .output()
            .expect("git runs");
        assert!(out.status.success(), "git {args:?}: {out:?}");
        String::from_utf8(out.stdout).unwrap()
    }

    /// Commits everything and returns the commit's hash.
    fn commit(&self, message: &str) -> String {
        self.git(&["add", "-A"]);
        self.git(&["commit", "-q", "--allow-empty", "-m", message]);
        self.git(&["rev-parse", "HEAD"]).trim().to_owned()
    }

    fn audit(&self, args: &[&str]) -> Run {
        Run::of(
            Command::new(env!("CARGO_BIN_EXE_xtask"))
                .arg("test-audit")
                .arg("--root")
                .arg(&self.root)
                .args(args),
        )
    }
}

// Spec tests: a valid `r[verify]`, an assertion, a reason per skip.

#[test]
fn a_crate_whose_spec_tests_keep_every_rule_passes() {
    Repo::new("keeps_every_rule").audit(&[]).passes();
}

#[test]
fn a_spec_test_without_a_verify_line_fails() {
    let repo = Repo::new("no_verify");
    repo.test("#[test]\nfn a_push_crosses() {\n    assert!(beamer_example::push());\n}\n");
    repo.audit(&[]).fails_naming(&[
        "crates/beamer-example/tests/zone.rs",
        "a_push_crosses",
        "r[verify",
    ]);
}

#[test]
fn a_verify_line_must_sit_directly_above_its_test() {
    let repo = Repo::new("verify_not_above");
    repo.test(
        "// r[verify example.zone.push]\n\nfn helper() {}\n\n#[test]\nfn a_push_crosses() {\n    assert!(beamer_example::push());\n}\n",
    );
    repo.audit(&[]).fails_naming(&["a_push_crosses"]);
}

#[test]
fn a_verify_line_may_sit_among_the_test_s_attributes_and_doc_comment() {
    let repo = Repo::new("verify_among_attributes");
    repo.test(
        "/// Pushes.\n// r[verify example.zone.push]\n#[test]\n// r[verify example.zone.held+2]\n#[should_panic]\nfn a_push_crosses() {\n    beamer_example::push();\n}\n",
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_spec_test_citing_an_unknown_rule_fails() {
    let repo = Repo::new("unknown_rule");
    repo.test(&GOOD_TEST.replace("example.zone.push]", "example.zone.pull]"));
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "example.zone.pull"]);
}

#[test]
fn a_spec_test_citing_an_older_version_of_its_rule_fails() {
    let repo = Repo::new("older_version");
    repo.test(&GOOD_TEST.replace("example.zone.push]", "example.zone.held]"));
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "example.zone.held", "+2"]);
}

#[test]
fn a_spec_test_with_no_assertion_fails() {
    let repo = Repo::new("no_assertion");
    repo.test(
        "// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    let _ = beamer_example::push();\n}\n",
    );
    repo.audit(&[]).fails_naming(&["a_push_crosses", "assert"]);
}

#[test]
fn a_debug_assertion_is_not_an_assertion() {
    let repo = Repo::new("debug_assert");
    repo.test(
        "// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    debug_assert!(beamer_example::push());\n}\n",
    );
    repo.audit(&[]).fails_naming(&["a_push_crosses"]);
}

#[test]
fn an_assertion_in_a_helper_of_the_crate_s_tests_counts() {
    let repo = Repo::new("helper_asserts");
    repo.test(
        "mod common;\n\n// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    common::Probe::new().crosses();\n}\n",
    );
    repo.file(
        "crates/beamer-example/tests/common/mod.rs",
        "pub struct Probe;\nimpl Probe {\n    pub fn new() -> Self { Probe }\n    pub fn crosses(&self) { check(beamer_example::push()) }\n}\nfn check(b: bool) {\n    assert!(b);\n}\n",
    );
    repo.audit(&[]).passes();
}

#[test]
fn an_assertion_in_a_nested_block_counts() {
    let repo = Repo::new("nested_assertion");
    repo.test(
        "// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    for _ in 0..2 {\n        assert!(beamer_example::push());\n    }\n}\n",
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_name_that_starts_like_an_assertion_is_not_one() {
    let repo = Repo::new("assert_named_variable");
    repo.test(
        "// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    let asserted = beamer_example::push();\n    let _ = asserted;\n}\n",
    );
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "asserts nothing"]);
}

#[test]
fn a_test_returning_a_result_is_found() {
    let repo = Repo::new("result_test");
    repo.test(
        "#[test]\nfn a_push_crosses() -> Result<(), String> {\n    assert!(beamer_example::push());\n    Ok(())\n}\n",
    );
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "r[verify"]);
}

#[test]
fn a_test_attribute_with_a_path_marks_a_test() {
    let repo = Repo::new("tokio_test");
    repo.test(
        "#[tokio::test]\nasync fn a_push_crosses() {\n    assert!(beamer_example::push());\n}\n",
    );
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "r[verify"]);
}

#[test]
fn attributes_end_at_the_item_they_belong_to() {
    let repo = Repo::new("attributes_end");
    repo.test(
        "#[should_panic]\nconst LIMIT: u8 = 1;\n\n// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    let _ = beamer_example::push();\n}\n",
    );
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "asserts nothing"]);
}

#[test]
fn a_declaration_without_a_body_keeps_its_neighbour_s_body() {
    let repo = Repo::new("trait_helper");
    repo.test(
        "trait Check {\n    fn value(&self) -> bool;\n    fn verify(&self) {\n        assert!(self.value());\n    }\n}\n\nimpl Check for bool {\n    fn value(&self) -> bool {\n        *self\n    }\n}\n\n// r[verify example.zone.push]\n#[test]\nfn a_push_crosses() {\n    beamer_example::push().verify();\n}\n",
    );
    repo.audit(&[]).passes();
}

#[test]
fn should_panic_counts_as_an_assertion() {
    let repo = Repo::new("should_panic");
    repo.test(
        "// r[verify example.zone.push]\n#[test]\n#[should_panic(expected = \"no peer\")]\nfn a_push_crosses() {\n    beamer_example::push();\n}\n",
    );
    repo.audit(&[]).passes();
}

#[test]
fn tests_inside_a_macro_such_as_proptest_are_audited() {
    let repo = Repo::new("proptest");
    repo.test(
        "proptest::proptest! {\n    #![proptest_config(Config::with_cases(8))]\n\n    // r[verify example.zone.push]\n    #[test]\n    fn pushes(x in 0..10u8) {\n        prop_assert!(beamer_example::push());\n    }\n\n    #[test]\n    fn uncited(x in 0..10u8) {\n        prop_assert!(beamer_example::push());\n    }\n}\n",
    );
    let run = repo.audit(&[]);
    run.fails_naming(&["uncited"]);
    assert!(!run.text.contains("pushes"), "{}", run.text);
}

// State-machine tests: one per state machine (#15), audited like any other spec test.

/// The reference model and the system under test of a state machine, with `{model}` in the
/// model's `apply` and `{sut}` in the system under test's `apply` and `check_invariants`.
fn state_machine(model: &str, sut: &str) -> String {
    format!(
        "use proptest_state_machine::{{ReferenceStateMachine, StateMachineTest}};

pub struct ZoneModel;

impl ReferenceStateMachine for ZoneModel {{
    type State = bool;
    type Transition = ();
    fn init_state() -> BoxedStrategy<bool> {{
        Just(false).boxed()
    }}
    fn transitions(_: &bool) -> BoxedStrategy<()> {{
        Just(()).boxed()
    }}
    fn apply(state: bool, _: &()) -> bool {{
        {model}
        !state
    }}
}}

pub struct ZoneTest;

impl StateMachineTest for ZoneTest {{
    type SystemUnderTest = bool;
    type Reference = ZoneModel;
    fn init_test(_: &bool) -> bool {{
        false
    }}
    fn apply(sut: bool, reference: &bool, _: ()) -> bool {{
        {sut}
        !sut
    }}
    fn check_invariants(sut: &bool, reference: &bool) {{
        {sut}
    }}
}}
"
    )
}

/// A `prop_state_machine!` test of [`state_machine`], with `cites` above its `#[test]`.
fn macro_test(cites: &str) -> String {
    format!(
        "prop_state_machine! {{
    {cites}
    #[test]
    fn zone_holds(sequential 1..20 => ZoneTest);
}}
"
    )
}

const SUT_ASSERTS: &str = "assert_eq!(sut, *reference, \"example.zone.push\");";

#[test]
fn a_state_machine_test_citing_its_rules_passes() {
    let repo = Repo::new("state_machine");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", SUT_ASSERTS)
            + &macro_test("// r[verify example.zone.push]\n    // r[verify example.zone.held+2]")),
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_state_machine_test_without_a_verify_line_fails() {
    let repo = Repo::new("state_machine_no_verify");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", SUT_ASSERTS) + &macro_test("")),
    );
    repo.audit(&[]).fails_naming(&[
        "crates/beamer-example/tests/model_zone.rs",
        "zone_holds",
        "r[verify",
    ]);
}

#[test]
fn a_state_machine_test_citing_an_unknown_rule_fails() {
    let repo = Repo::new("state_machine_unknown_rule");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", SUT_ASSERTS) + &macro_test("// r[verify example.zone.pull]")),
    );
    repo.audit(&[])
        .fails_naming(&["zone_holds", "example.zone.pull"]);
}

#[test]
fn a_state_machine_test_citing_an_older_version_of_its_rule_fails() {
    let repo = Repo::new("state_machine_older_version");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", SUT_ASSERTS) + &macro_test("// r[verify example.zone.held]")),
    );
    repo.audit(&[])
        .fails_naming(&["zone_holds", "example.zone.held", "+2"]);
}

#[test]
fn a_state_machine_test_asserting_in_its_model_passes() {
    let repo = Repo::new("state_machine_model_asserts");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("assert!(!state || state);", "")
            + &macro_test("// r[verify example.zone.push]")),
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_state_machine_test_asserting_in_a_helper_of_its_invariant_check_passes() {
    let repo = Repo::new("state_machine_helper_asserts");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", "same(sut, reference);")
            + "fn same(a: &bool, b: &bool) {\n    assert_eq!(a, b);\n}\n"
            + &macro_test("// r[verify example.zone.push]")),
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_state_machine_s_types_may_be_paths_with_generics() {
    let repo = Repo::new("state_machine_paths");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("assert!(!state || state);", "")
            .replace("ZoneModel;", "ZoneModel<T>(T);")
            .replace("for ZoneModel", "for self::ZoneModel<u8>")
            .replace("= ZoneModel;", "= self::ZoneModel<u8>;")
            .replace("for ZoneTest", "for crate::ZoneTest")
            + &macro_test("// r[verify example.zone.push]")),
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_state_machine_test_with_no_assertion_anywhere_fails() {
    let repo = Repo::new("state_machine_no_assertion");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", "") + &macro_test("// r[verify example.zone.push]")),
    );
    repo.audit(&[])
        .fails_naming(&["zone_holds", "asserts nothing"]);
}

#[test]
fn a_state_machine_test_is_credited_only_with_its_own_state_machine_s_assertions() {
    let repo = Repo::new("state_machine_other_asserts");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", "") + &macro_test("// r[verify example.zone.push]")),
    );
    repo.file(
        "crates/beamer-example/tests/model_edge.rs",
        &state_machine("", SUT_ASSERTS)
            .replace("ZoneModel", "EdgeModel")
            .replace("ZoneTest", "EdgeTest"),
    );
    repo.audit(&[])
        .fails_naming(&["zone_holds", "asserts nothing"]);
}

#[test]
fn an_impl_in_type_position_keeps_the_next_test() {
    let repo = Repo::new("impl_in_type_position");
    repo.test(&format!("type Pushes = impl Fn() -> bool;\n\n#[test]\nfn uncited() {{\n    assert!(beamer_example::push());\n}}\n\n{GOOD_TEST}"));
    repo.audit(&[]).fails_naming(&["uncited", "r[verify"]);
}

/// A test that runs [`state_machine`] by hand, in `proptest!`, rather than with
/// `prop_state_machine!`.
const BY_HAND: &str = "proptest! {
    // r[verify example.zone.push]
    #[test]
    fn zone_holds((initial, transitions, seen) in ZoneModel::sequential_strategy(1..20)) {
        ZoneTest::test_sequential(Default::default(), initial, transitions, seen)
    }
}
";

#[test]
fn a_state_machine_test_run_by_hand_asserting_in_its_invariant_check_passes() {
    let repo = Repo::new("state_machine_by_hand");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", SUT_ASSERTS) + BY_HAND),
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_state_machine_test_run_by_hand_with_no_assertion_anywhere_fails() {
    let repo = Repo::new("state_machine_by_hand_no_assertion");
    repo.file(
        "crates/beamer-example/tests/model_zone.rs",
        &(state_machine("", "") + BY_HAND),
    );
    repo.audit(&[])
        .fails_naming(&["zone_holds", "asserts nothing"]);
}

#[test]
fn every_problem_is_reported_with_its_line() {
    let repo = Repo::new("lines");
    repo.test(&format!(
        "{GOOD_TEST}\n#[test]\nfn first() {{\n    assert!(true);\n}}\n\n#[test]\nfn second() {{\n    assert!(true);\n}}\n"
    ));
    repo.audit(&[]).fails_naming(&[
        "crates/beamer-example/tests/zone.rs:8",
        "first",
        "crates/beamer-example/tests/zone.rs:13",
        "second",
    ]);
}

#[test]
fn an_ignored_spec_test_needs_a_reason() {
    let repo = Repo::new("ignore_without_reason");
    repo.test(&GOOD_TEST.replace("#[test]\n", "#[test]\n#[ignore]\n"));
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "#[ignore = \""]);
}

#[test]
fn an_empty_reason_is_no_reason() {
    let repo = Repo::new("ignore_empty_reason");
    repo.test(&GOOD_TEST.replace("#[test]\n", "#[test]\n#[ignore = \" \"]\n"));
    repo.audit(&[])
        .fails_naming(&["a_push_crosses", "#[ignore = \""]);
}

#[test]
fn an_ignored_spec_test_with_a_reason_passes() {
    let repo = Repo::new("ignore_with_reason");
    repo.test(&GOOD_TEST.replace(
        "#[test]\n",
        "#[test]\n#[ignore = \"the nightly model job runs it\"]\n",
    ));
    repo.audit(&[]).passes();
}

// Unit tests: allowed, and never counted.

#[test]
fn an_implementer_unit_test_needs_no_verify_line_or_assertion() {
    let repo = Repo::new("unit_test");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "pub fn push() -> bool {\n    true\n}\n\n#[cfg(test)]\nmod tests {\n    #[test]\n    fn scaffolding() {\n        let _ = super::push();\n    }\n}\n",
    );
    repo.audit(&[]).passes();
}

#[test]
fn a_verify_line_outside_tests_fails_because_a_unit_test_never_counts() {
    let repo = Repo::new("verify_in_src");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "pub fn push() -> bool {\n    true\n}\n\n#[cfg(test)]\nmod tests {\n    // r[verify example.zone.push]\n    #[test]\n    fn scaffolding() {\n        assert!(super::push());\n    }\n}\n",
    );
    repo.audit(&[])
        .fails_naming(&["crates/beamer-example/src/lib.rs:7", "tests/"]);
}

// Mutant skips: each one justified.

#[test]
fn a_mutants_skip_without_a_justification_fails() {
    let repo = Repo::new("bare_skip");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "#[mutants::skip]\npub fn push() -> bool {\n    true\n}\n",
    );
    repo.audit(&[])
        .fails_naming(&["crates/beamer-example/src/lib.rs:1", "mutants-justified:"]);
}

#[test]
fn a_justified_mutants_skip_passes() {
    let repo = Repo::new("justified_skip");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "/// Pushes.\n// mutants-justified: OS call, verified at machine tier (example.zone.push)\n#[inline]\n#[mutants::skip]\npub fn push() -> bool {\n    true\n}\n",
    );
    repo.audit(&[]).passes();
}

// Commits: a test commit and an implementation commit are never the same commit.

#[test]
fn a_commit_touching_tests_and_implementation_together_fails() {
    let repo = Repo::new("mixed_commit");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "pub fn push() -> bool {\n    false\n}\n",
    );
    repo.test(&GOOD_TEST.replace("assert!(", "assert!(!"));
    let mixed = repo.commit("Make push pass");
    repo.audit(&[]).fails_naming(&[
        &mixed[..12],
        "Make push pass",
        "crates/beamer-example/tests/zone.rs",
        "crates/beamer-example/src/lib.rs",
    ]);
}

#[test]
fn every_kind_of_test_file_counts_as_a_test_file() {
    for (n, path) in [
        "crates/beamer-example/proptest-regressions/zone.txt",
        "crates/beamer-example/src/__fuzz__/frame/corpus/a",
        "crates/beamer-example/tests/snapshots/zone__strings.snap",
        "crates/beamer-example/src/snapshots/lib__strings.snap",
    ]
    .iter()
    .enumerate()
    {
        let repo = Repo::new(&format!("test_file_{n}"));
        repo.file(path, "x\n");
        repo.file(
            "crates/beamer-example/src/lib.rs",
            "pub fn push() -> bool {\n    !false\n}\n",
        );
        repo.commit("Both");
        repo.audit(&[]).fails_naming(&[path]);
    }
}

#[test]
fn a_file_at_the_crate_s_root_is_an_implementation_file() {
    let repo = Repo::new("build_rs");
    repo.file("crates/beamer-example/build.rs", "fn main() {}\n");
    repo.test(&GOOD_TEST.replace("a_push_crosses", "a_push_still_crosses"));
    repo.commit("Both");
    repo.audit(&[])
        .fails_naming(&["crates/beamer-example/build.rs"]);
}

#[test]
fn every_commit_is_counted() {
    let run = Repo::new("commit_count").audit(&[]);
    run.passes();
    assert!(run.text.contains("2 commits checked"), "{}", run.text);
}

#[test]
fn a_since_that_is_not_a_commit_fails() {
    Repo::new("bad_since")
        .audit(&["--since", "no-such-rev"])
        .fails_naming(&["no-such-rev"]);
}

#[test]
fn test_commits_and_implementation_commits_apart_pass() {
    let repo = Repo::new("separate_commits");
    repo.test(&format!(
        "{GOOD_TEST}\n// r[verify example.zone.held+2]\n#[test]\nfn held() {{\n    assert!(beamer_example::push());\n}}\n"
    ));
    repo.file(
        "crates/beamer-example/proptest-regressions/zone.txt",
        "cc 1\n",
    );
    repo.commit("Tests for example.zone.held");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "pub fn push() -> bool {\n    !false\n}\n",
    );
    repo.commit("Implement example.zone.held");
    repo.audit(&[]).passes();
}

#[test]
fn the_manifest_and_files_outside_crates_go_with_either_kind_of_commit() {
    let repo = Repo::new("neutral_files");
    repo.test(&GOOD_TEST.replace("a_push_crosses", "a_push_still_crosses"));
    repo.file(
        "crates/beamer-example/Cargo.toml",
        "[package]\nname = \"beamer-example\"\n\n[dev-dependencies]\nproptest = \"1\"\n",
    );
    repo.file(
        "spec/example.md",
        &format!("{SPEC}\nr[example.zone.new]\nNew.\n"),
    );
    repo.file("Cargo.lock", "# lock\n");
    repo.commit("Tests, with their dev-dependency");
    repo.audit(&[]).passes();
}

#[test]
fn since_limits_the_check_to_the_commits_after_it() {
    let repo = Repo::new("since");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "pub fn push() -> bool {\n    !false\n}\n",
    );
    repo.test(&GOOD_TEST.replace("a_push_crosses", "a_push_still_crosses"));
    let mixed = repo.commit("Old mixed commit");
    repo.file(
        "crates/beamer-example/src/lib.rs",
        "pub fn push() -> bool {\n    true\n}\n",
    );
    repo.commit("Implementation");
    repo.audit(&[]).fails_naming(&[&mixed[..12]]);
    repo.audit(&["--since", &mixed]).passes();
}

// The tree this audit runs on in CI.

#[test]
fn this_repo_passes_the_audit() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    Run::of(
        Command::new(env!("CARGO_BIN_EXE_xtask"))
            .args(["test-audit", "--root"])
            .arg(&root),
    )
    .passes();
}

// The Python ledger.

/// Python tests with the shapes the scan must get right. The ids it holds are in `PYTHON_TESTS`.
const PYTHON: &str = r##"import unittest

def test_top():
    pass

class ZoneTests(unittest.TestCase):
    def test_push(self):
        return [1,
"""],
""",
]

    async def test_async(self):
        pass

    # A comment with a ( in it.
    note = "a ( paren"
    escaped = "a \" ( paren"
    other = '\'' + "#"
    raw = """a \""" still in the string"""
    total = 1 + \
2

    def test_after_strings(self):
        pass

    class Inner:
        def test_inner(self):
            pass

    def helper(self):
        def test_nested_in_a_function(self):
            pass
        return test_nested_in_a_function

SOURCE = """
class Fake:
    def test_in_a_string(self):
        pass
"""

if True:
    def test_under_if():
        pass

def test_after(
    x,
):
    pass
"##;

const PYTHON_TESTS: [&str; 8] = [
    "core/tests/test_zone.py::test_top",
    "core/tests/test_zone.py::ZoneTests::test_push",
    "core/tests/test_zone.py::ZoneTests::test_async",
    "core/tests/test_zone.py::ZoneTests::test_after_strings",
    "core/tests/test_zone.py::ZoneTests::Inner::test_inner",
    "core/tests/test_zone.py::test_under_if",
    "core/tests/test_zone.py::test_after",
    "mac_app/tests/test_tray.py::test_tray",
];

impl Repo {
    /// Commits the Python app, tags it `archive/python`, then deletes it as `rust` did.
    fn python(&self) -> &Self {
        self.file("core/tests/test_zone.py", PYTHON);
        self.file("mac_app/tests/test_tray.py", "def test_tray():\n    pass\n");
        self.file(
            "core/zone.py",
            "def test_helper_outside_tests():\n    pass\n",
        );
        self.commit("Python app");
        self.git(&["tag", "archive/python"]);
        self.git(&["rm", "-q", "-r", "core", "mac_app"]);
        self.commit("Cut rust");
        self
    }

    fn ledger(&self, domain: &str, text: &str) -> &Self {
        self.file(&format!("spec/ledger/{domain}.txt"), text)
    }
}

/// Every Python test once, `fate` given to each.
fn whole_ledger(fate: &str) -> String {
    let mut all: Vec<&str> = PYTHON_TESTS.to_vec();
    all.push("core/zone.py::test_helper_outside_tests");
    all.iter().map(|id| format!("{id}  {fate}\n")).collect()
}

#[test]
fn without_a_ledger_the_ledger_check_has_nothing_to_check() {
    let repo = Repo::new("no_ledger");
    let run = repo.audit(&[]);
    run.passes();
    assert!(run.text.contains("no ledger"), "{}", run.text);
}

#[test]
fn a_partial_ledger_passes_per_pr() {
    let repo = Repo::new("partial_ledger");
    repo.python()
        .ledger(
            "crossing",
            "# Owned by crossing.\n\
             core/tests/test_zone.py::ZoneTests::test_push  mined: example.zone.push, example.zone.held\n\
             core/tests/test_zone.py::ZoneTests::test_async\n\
             \n\
             core/tests/test_zone.py::test_top  dropped: gestures, dropped by #11\n",
        )
        .ledger(
            "tray",
            "mac_app/tests/test_tray.py::test_tray  python-detail: Qt's tray icon refresh\n\
             core/tests/test_zone.py::test_after  wire-v6: the v6 hello frame\n",
        );
    let run = repo.audit(&[]);
    run.passes();
    assert!(
        run.text
            .contains("the ledger holds 5 of the 9 Python tests at archive/python, 4 with a fate"),
        "{}",
        run.text
    );
}

#[test]
fn the_ledger_holds_every_python_def_test_with_its_class_path() {
    let repo = Repo::new("whole_ledger");
    repo.python()
        .ledger("crossing", &whole_ledger("dropped: gestures"));
    repo.audit(&["--complete"]).passes();
}

#[test]
fn a_test_in_two_domains_fails() {
    let repo = Repo::new("ledger_duplicate");
    repo.python()
        .ledger("crossing", "core/tests/test_zone.py::test_top\n")
        .ledger(
            "tray",
            "mac_app/tests/test_tray.py::test_tray\ncore/tests/test_zone.py::test_top\n",
        );
    repo.audit(&[]).fails_naming(&[
        "core/tests/test_zone.py::test_top",
        "spec/ledger/crossing.txt:1",
        "spec/ledger/tray.txt:2",
    ]);
}

#[test]
fn a_test_twice_in_one_domain_fails() {
    let repo = Repo::new("ledger_duplicate_in_file");
    repo.python().ledger(
        "crossing",
        "core/tests/test_zone.py::test_top\ncore/tests/test_zone.py::test_top  dropped: gestures\n",
    );
    repo.audit(&[]).fails_naming(&[
        "spec/ledger/crossing.txt:2",
        "core/tests/test_zone.py::test_top",
    ]);
}

#[test]
fn a_test_that_is_not_a_python_test_fails() {
    for (n, id) in [
        "core/tests/test_zone.py::test_gone",
        "core/tests/test_zone.py::Fake::test_in_a_string",
        "core/tests/test_zone.py::ZoneTests::test_nested_in_a_function",
        "core/tests/test_zone.py::ZoneTests::helper::test_nested_in_a_function",
    ]
    .iter()
    .enumerate()
    {
        let repo = Repo::new(&format!("ledger_unknown_test_{n}"));
        repo.python().ledger("crossing", &format!("{id}\n"));
        repo.audit(&[])
            .fails_naming(&["spec/ledger/crossing.txt:1", id, "archive/python"]);
    }
}

#[test]
fn a_test_mined_into_an_unknown_rule_fails() {
    let repo = Repo::new("ledger_unknown_rule");
    repo.python().ledger(
        "crossing",
        "core/tests/test_zone.py::test_top  mined: example.zone.push, example.zone.pull\n",
    );
    repo.audit(&[])
        .fails_naming(&["spec/ledger/crossing.txt:1", "example.zone.pull"]);
}

#[test]
fn a_mined_test_names_rule_ids_without_their_version() {
    let repo = Repo::new("ledger_versioned_rule");
    repo.python().ledger(
        "crossing",
        "core/tests/test_zone.py::test_top  mined: example.zone.held+2\n",
    );
    repo.audit(&[])
        .fails_naming(&["spec/ledger/crossing.txt:1", "example.zone.held+2"]);
}

#[test]
fn a_fate_that_is_not_one_of_the_four_fails() {
    let repo = Repo::new("ledger_bad_fate");
    repo.python().ledger(
        "crossing",
        "core/tests/test_zone.py::test_top  ported: as is\n",
    );
    repo.audit(&[]).fails_naming(&[
        "spec/ledger/crossing.txt:1",
        "mined, dropped, python-detail, wire-v6",
    ]);
}

#[test]
fn every_fate_but_mined_gives_its_reason_and_mined_names_a_rule() {
    for (n, fate) in [
        "dropped:",
        "python-detail:  ",
        "wire-v6",
        "mined:",
        "mined: ,",
    ]
    .iter()
    .enumerate()
    {
        let repo = Repo::new(&format!("ledger_no_reason_{n}"));
        repo.python().ledger(
            "crossing",
            &format!("core/tests/test_zone.py::test_top  {fate}\n"),
        );
        repo.audit(&[])
            .fails_naming(&["spec/ledger/crossing.txt:1"]);
    }
}

#[test]
fn a_ledger_file_is_named_for_a_domain() {
    let repo = Repo::new("ledger_bad_domain");
    repo.python()
        .ledger("Crossing_Map", "core/tests/test_zone.py::test_top\n");
    repo.audit(&[])
        .fails_naming(&["spec/ledger/Crossing_Map.txt"]);
}

#[test]
fn complete_requires_every_python_test() {
    let repo = Repo::new("ledger_incomplete");
    let ledger = whole_ledger("dropped: gestures").replace(
        "mac_app/tests/test_tray.py::test_tray  dropped: gestures\n",
        "",
    );
    repo.python().ledger("crossing", &ledger);
    repo.audit(&[]).passes();
    repo.audit(&["--complete"])
        .fails_naming(&["mac_app/tests/test_tray.py::test_tray", "1 Python test"]);
}

#[test]
fn complete_requires_a_fate_for_every_test() {
    let repo = Repo::new("ledger_no_fate");
    let ledger = whole_ledger("dropped: gestures").replace(
        "core/tests/test_zone.py::test_top  dropped: gestures",
        "core/tests/test_zone.py::test_top",
    );
    repo.python().ledger("crossing", &ledger);
    repo.audit(&["--complete"]).fails_naming(&[
        "spec/ledger/crossing.txt:1",
        "core/tests/test_zone.py::test_top",
        "fate",
    ]);
}

#[test]
fn complete_without_a_ledger_fails() {
    let repo = Repo::new("ledger_none_complete");
    repo.python();
    repo.audit(&["--complete"]).fails_naming(&["9 Python test"]);
}

#[test]
fn a_ledger_without_the_python_tag_fails_and_says_how_to_fetch_it() {
    let repo = Repo::new("ledger_no_tag");
    repo.ledger("crossing", "core/tests/test_zone.py::test_top\n");
    repo.audit(&[])
        .fails_naming(&["archive/python", "git fetch origin tag archive/python"]);
}

#[test]
fn python_ref_reads_the_python_tests_from_another_ref() {
    let repo = Repo::new("ledger_other_ref");
    repo.python();
    repo.git(&["tag", "elsewhere", "archive/python"]);
    repo.git(&["tag", "-d", "archive/python"]);
    repo.ledger("crossing", "core/tests/test_zone.py::test_top\n");
    repo.audit(&["--python-ref", "elsewhere"]).passes();
}
