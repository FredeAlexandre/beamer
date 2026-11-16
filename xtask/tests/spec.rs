//! `cargo xtask spec`: tracey and the spec format check, run on a copy of the example spec in
//! `tests/fixtures/spec-example/`, which uses the repo's own tracey config.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

fn fixture() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/spec-example")
}

/// A private copy of the example, so tests can edit it and each gets its own tracey daemon.
struct Example {
    dir: tempfile::TempDir,
}

impl Example {
    fn new() -> Self {
        let dir = tempfile::tempdir().expect("a temp dir");
        copy_dir(&fixture(), dir.path());
        Example { dir }
    }

    fn path(&self, rel: &str) -> PathBuf {
        self.dir.path().join(rel)
    }

    fn write(&self, rel: &str, text: &str) {
        let path = self.path(rel);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    fn replace(&self, rel: &str, from: &str, to: &str) {
        let text = fs::read_to_string(self.path(rel)).unwrap();
        assert!(text.contains(from), "{rel} has no {from:?}");
        fs::write(self.path(rel), text.replacen(from, to, 1)).unwrap();
    }

    fn spec(&self, args: &[&str]) -> Run {
        let out = Command::new(env!("CARGO_BIN_EXE_xtask"))
            .arg("spec")
            .args(args)
            .arg("--root")
            .arg(self.dir.path())
            .output()
            .expect("the xtask binary runs");
        Run::from(out)
    }
}

fn copy_dir(from: &Path, to: &Path) {
    fs::create_dir_all(to).unwrap();
    for entry in fs::read_dir(from).unwrap() {
        let entry = entry.unwrap();
        let target = to.join(entry.file_name());
        if entry.file_type().unwrap().is_dir() {
            copy_dir(&entry.path(), &target);
        } else {
            fs::copy(entry.path(), target).unwrap();
        }
    }
}

struct Run {
    ok: bool,
    /// stdout and stderr together: the messages are for a person, whichever stream they use.
    text: String,
}

impl From<Output> for Run {
    fn from(out: Output) -> Self {
        let text = String::from_utf8_lossy(&out.stdout).into_owned()
            + &String::from_utf8_lossy(&out.stderr);
        Run {
            ok: out.status.success(),
            text,
        }
    }
}

impl Run {
    fn passes(&self) {
        assert!(self.ok, "expected success, got:\n{}", self.text);
    }

    fn fails_saying(&self, needle: &str) {
        assert!(!self.ok, "expected failure, got success:\n{}", self.text);
        assert!(
            self.text.contains(needle),
            "expected {needle:?} in:\n{}",
            self.text
        );
    }
}

#[test]
fn the_example_spec_validates() {
    Example::new().spec(&["validate"]).passes();
}

#[test]
fn a_test_citing_an_unknown_rule_fails_validate() {
    let example = Example::new();
    example.replace(
        "crates/beamer-example/tests/example.rs",
        "r[verify example.zone.leads-to-peer]",
        "r[verify example.zone.no-such-rule]",
    );
    example
        .spec(&["validate"])
        .fails_saying("example.zone.no-such-rule");
}

#[test]
fn the_example_has_no_stale_reference() {
    Example::new().spec(&["stale"]).passes();
}

#[test]
fn a_bumped_rule_makes_its_tests_stale() {
    let example = Example::new();
    example.replace(
        "spec/example.md",
        "r[example.zone.push+2]",
        "r[example.zone.push+3]",
    );
    let stale = example.spec(&["stale"]);
    stale.fails_saying("example.zone.push");
    stale.fails_saying("crates/beamer-example/tests/example.rs");
    example
        .spec(&["validate"])
        .fails_saying("example.zone.push");
}

impl Example {
    /// Appends `rule` to the example spec, then runs `spec validate`.
    fn validate_with(&self, rule: &str) -> Run {
        let spec = fs::read_to_string(self.path("spec/example.md")).unwrap();
        self.write("spec/example.md", &format!("{spec}\n{rule}\n"));
        self.spec(&["validate"])
    }
}

#[test]
fn a_tier_other_than_ci_machine_or_human_fails() {
    Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - tier: nightly (runs at night)")
        .fails_saying("example.zone.extra");
}

#[test]
fn a_machine_or_human_tier_needs_a_reason() {
    let run =
        Example::new().validate_with("> r[example.zone.extra]\n> Text.\n>\n> - tier: machine");
    run.fails_saying("example.zone.extra");
    run.fails_saying("reason");
}

#[test]
fn ci_is_the_default_tier_and_is_never_written() {
    Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - tier: ci (hosted runners)")
        .fails_saying("example.zone.extra");
}

#[test]
fn a_parity_change_says_what_python_did_and_why() {
    let run = Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - parity: Python crossed at once");
    run.fails_saying("example.zone.extra");
    run.fails_saying("parity");
}

#[test]
fn a_model_is_one_of_the_eight_state_machines() {
    Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - model: crossing")
        .fails_saying("driving, link, pairing, handover, store, clipboard, audio, camera");
}

#[test]
fn every_one_of_the_eight_models_is_accepted() {
    let example = Example::new();
    let mut rules = String::new();
    for model in [
        "driving",
        "link",
        "pairing",
        "handover",
        "store",
        "clipboard",
        "audio",
        "camera",
    ] {
        rules += &format!("> r[example.model.{model}]\n> Text.\n>\n> - model: {model}\n\n");
    }
    example.validate_with(&rules).passes();
}

#[test]
fn a_fuzz_harness_is_named_in_kebab_case() {
    Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - fuzz: Link Frame")
        .fails_saying("example.zone.extra");
}

#[test]
fn a_misspelt_marker_fails() {
    Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - teir: machine (real input)")
        .fails_saying("`teir:` is not a marker");
}

#[test]
fn markers_under_a_plain_rule_fail_because_tracey_would_not_version_them() {
    Example::new()
        .validate_with("r[example.zone.extra]\nText.\n- tier: machine (real input)")
        .fails_saying("blockquote");
}

#[test]
fn a_marker_in_the_middle_of_a_rule_fails() {
    Example::new()
        .validate_with("> r[example.zone.extra]\n> - tier: machine (real input)\n>\n> More text.")
        .fails_saying("example.zone.extra");
}

#[test]
fn a_rule_may_end_in_a_list_of_its_own_before_its_markers() {
    Example::new()
        .validate_with(
            "> r[example.zone.extra]\n> A zone is one of:\n>\n> - a whole edge\n> - a corner\n>\n> - tier: human (seen)\n> - do: Cross to the PC\n> - see: The pointer lands on the zone",
        )
        .passes();
}

#[test]
fn a_human_tier_rule_says_what_to_do_and_what_to_see() {
    let run = Example::new()
        .validate_with("> r[example.zone.extra]\n> Text.\n>\n> - tier: human (seen at the desk)");
    run.fails_saying("example.zone.extra");
    run.fails_saying("`- do:");
    run.fails_saying("`- see:");
}

#[test]
fn a_human_tier_rule_with_what_to_do_and_see_passes() {
    Example::new()
        .validate_with(
            "> r[example.zone.extra]\n> Text.\n>\n> - tier: human (seen at the desk)\n\
             > - do: Push the pointer against the zone\n> - see: The pointer stops at the edge",
        )
        .passes();
}

#[test]
fn only_a_human_tier_rule_says_what_to_do_and_see() {
    for (marker, rule) in [
        ("do", "> - tier: machine (real input)\n> - do: Push"),
        ("see", "> - see: The pointer stops"),
    ] {
        Example::new()
            .validate_with(&format!("> r[example.zone.extra]\n> Text.\n>\n{rule}"))
            .fails_saying(&format!("a `{marker}:` marker on a rule that is not `human` tier"));
    }
}

#[test]
fn what_to_do_and_see_is_said_once() {
    Example::new()
        .validate_with(
            "> r[example.zone.extra]\n> Text.\n>\n> - tier: human (seen)\n> - do: Push\n\
             > - do: Push again\n> - see: It stops",
        )
        .fails_saying("a second `do:` marker");
}

#[test]
fn a_rule_id_starts_with_its_file_s_domain() {
    Example::new()
        .validate_with("r[crossing.zone.extra]\nText.")
        .fails_saying("crossing.zone.extra");
}

#[test]
fn a_rule_id_has_a_domain_a_topic_and_a_rule() {
    Example::new()
        .validate_with("r[example.extra]\nText.")
        .fails_saying("example.extra");
}

#[test]
fn a_rule_id_is_lowercase_kebab_case() {
    Example::new()
        .validate_with("r[example.zone.Extra_rule]\nText.")
        .fails_saying("example.zone.Extra_rule");
}

#[test]
fn version_one_has_no_suffix() {
    Example::new()
        .validate_with("r[example.zone.extra+1]\nText.")
        .fails_saying("example.zone.extra");
}

#[test]
fn rule_markers_in_code_fences_are_examples_not_rules() {
    Example::new()
        .validate_with("```markdown\nr[crossing.not.checked]\n- tier: nightly\n```")
        .passes();
}

#[test]
fn parity_lists_every_parity_change_with_what_python_did_and_why() {
    let example = Example::new();
    example.write(
        "spec/other.md",
        "# Other\n\n> r[other.topic.rule+2]\n> The rule now.\n>\n> - parity: Python did it then (it was slow)\n",
    );
    let run = example.spec(&["parity"]);
    run.passes();
    assert!(
        run.text.contains(
            "- `example.zone.held-at-edge` (spec/example.md:15)\n  \
             - Python: Python crossed on the first pixel of the edge, with no push pressure\n  \
             - Why: #11 keeps push pressure, to stop accidental crossings\n"
        ),
        "{}",
        run.text
    );
    assert!(
        run.text.contains(
            "- `other.topic.rule+2` (spec/other.md:3)\n  \
             - Python: Python did it then\n  \
             - Why: it was slow\n  \
             - Rule: The rule now.\n"
        ),
        "{}",
        run.text
    );
}

#[test]
fn a_spec_without_parity_markers_has_no_parity_changes() {
    let example = Example::new();
    example.replace(
        "spec/example.md",
        "> - parity: Python crossed on the first pixel of the edge, with no push pressure \
         (#11 keeps push pressure, to stop accidental crossings)\n",
        "",
    );
    let run = example.spec(&["parity"]);
    run.passes();
    assert!(run.text.contains("No parity changes."), "{}", run.text);
}

/// The README's examples sit in code fences, so the spec as it stands validates.
#[test]
fn the_repo_spec_validates() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let out = Command::new(env!("CARGO_BIN_EXE_xtask"))
        .args(["spec", "validate", "--root"])
        .arg(&root)
        .output()
        .expect("the xtask binary runs");
    Run::from(out).passes();
}

#[test]
fn the_example_uses_the_repo_s_tracey_config() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let repo = fs::read_to_string(root.join(".config/tracey/config.styx")).unwrap();
    let example = fs::read_to_string(fixture().join(".config/tracey/config.styx")).unwrap();
    assert_eq!(
        repo, example,
        "the example's tracey config drifted from the repo's"
    );
}

/// tracey exits zero on a config error, having checked nothing.
#[test]
fn a_broken_tracey_config_fails_validate_and_stale() {
    let example = Example::new();
    example.replace(
        ".config/tracey/config.styx",
        "include (spec/*.md)",
        "include (nowhere/*.md)",
    );
    example.spec(&["validate"]).fails_saying("CONFIG ERROR");
    example.spec(&["stale"]).fails_saying("CONFIG ERROR");
}

#[test]
fn a_spec_with_no_rules_yet_passes_without_tracey() {
    let example = Example::new();
    fs::remove_file(example.path("spec/example.md")).unwrap();
    example.write(
        "spec/README.md",
        "# The spec\n\n```markdown\nr[x.y.z]\nText.\n```\n",
    );
    example.spec(&["validate"]).passes();
    example.spec(&["stale"]).passes();
}

#[test]
fn without_tracey_validate_fails_and_says_how_to_install_it() {
    let example = Example::new();
    let out = Command::new(env!("CARGO_BIN_EXE_xtask"))
        .args(["spec", "validate", "--root"])
        .arg(example.dir.path())
        .env("PATH", "")
        .output()
        .expect("the xtask binary runs");
    Run::from(out).fails_saying(
        "cargo install --locked --git https://github.com/bearcove/tracey --tag v1.4.0",
    );
}
