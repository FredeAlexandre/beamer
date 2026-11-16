//! `cargo xtask kill-matrix`: which spec test kills which mutant, read from real `mutants.out`
//! directories in `tests/fixtures/mutants/` (its README says how each was made).

mod common;

use common::Run;
use std::path::Path;
use std::process::Command;

fn kill_matrix(runs: &[&str]) -> Run {
    let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/mutants");
    Run::of(
        Command::new(env!("CARGO_BIN_EXE_xtask"))
            .arg("kill-matrix")
            .args(runs.iter().map(|r| fixtures.join(r))),
    )
}

const DOUBLE_0: &str = "crates/beamer-k/src/lib.rs:2:5: replace double -> u32 with 0";

#[test]
fn each_caught_mutant_lists_every_spec_test_that_kills_it() {
    let run = kill_matrix(&["shard-0", "shard-1"]);
    run.passes();
    assert!(
        run.text.contains(&format!(
            "- `{DOUBLE_0}`: `beamer-k::k doubles`, `beamer-k::k doubles_again`\n"
        )),
        "{}",
        run.text
    );
}

#[test]
fn a_spec_test_that_kills_no_mutant_is_listed() {
    let run = kill_matrix(&["shard-0", "shard-1"]);
    run.passes();
    let section = section(&run.text, "## Spec tests that kill no mutant (1)");
    assert!(
        section.contains("- `beamer-k::k guards_nothing`\n"),
        "{}",
        run.text
    );
    assert!(!section.contains("doubles"), "{}", run.text);
}

#[test]
fn every_spec_test_has_its_kill_count() {
    let run = kill_matrix(&["shard-0", "shard-1"]);
    run.passes();
    for row in [
        "| `beamer-k::k big` | 5 |",
        "| `beamer-k::k doubles` | 4 |",
        "| `beamer-k::k doubles_again` | 4 |",
        "| `beamer-k::k guards_nothing` | 0 |",
    ] {
        assert!(run.text.contains(row), "no {row:?} in:\n{}", run.text);
    }
}

#[test]
fn the_shards_of_one_nightly_merge_into_one_matrix() {
    let one = kill_matrix(&["shard-0"]);
    one.passes();
    assert!(
        one.text.contains("| `beamer-k::k big` | 3 |"),
        "{}",
        one.text
    );
    let both = kill_matrix(&["shard-0", "shard-1"]);
    assert!(
        both.text
            .contains("13 mutants from 2 runs: 9 caught, 4 missed, 0 timed out, 0 unviable"),
        "{}",
        both.text
    );
}

#[test]
fn missed_mutants_are_listed_as_killed_by_no_spec_test() {
    let run = kill_matrix(&["shard-0", "shard-1"]);
    let section = section(&run.text, "## Mutants no spec test kills (4)");
    assert!(
        section
            .contains("- `crates/beamer-k/src/lib.rs:10:7: replace + with - in unused` (missed)\n"),
        "{}",
        run.text
    );
}

#[test]
fn a_unit_test_never_counts_as_a_killer() {
    let run = kill_matrix(&["with-unit"]);
    run.passes();
    assert!(!run.text.contains("tests::unit"), "{}", run.text);
    let section = section(&run.text, "## Mutants no spec test kills (4)");
    assert!(
        section.contains(
            "- `crates/beamer-k/src/lib.rs:10:7: replace + with - in unused` (caught by no spec test)\n"
        ),
        "{}",
        run.text
    );
}

#[test]
fn a_run_without_no_fail_fast_fails_because_it_names_only_the_first_killer() {
    kill_matrix(&["fail-fast"]).fails_naming(&["fail-fast", "--no-fail-fast"]);
}

#[test]
fn a_directory_that_is_not_a_mutants_out_fails() {
    kill_matrix(&["beamer-k"]).fails_naming(&["beamer-k", "outcomes.json"]);
}

/// The text from `heading` to the next heading.
fn section<'t>(text: &'t str, heading: &str) -> &'t str {
    let start = text
        .find(heading)
        .unwrap_or_else(|| panic!("no {heading:?} in:\n{text}"));
    let rest = &text[start + heading.len()..];
    &rest[..rest.find("\n## ").unwrap_or(rest.len())]
}

#[test]
fn a_timed_out_mutant_is_killed_by_no_spec_test_and_an_unviable_one_is_only_counted() {
    let run = kill_matrix(&["timeout-unviable"]);
    run.passes();
    assert!(
        run.text
            .contains("7 mutants from 1 run: 5 caught, 0 missed, 1 timed out, 1 unviable"),
        "{}",
        run.text
    );
    let section = section(&run.text, "## Mutants no spec test kills (1)");
    assert!(
        section.contains(
            "- `crates/beamer-k/src/lib.rs:6:5: replace is_big -> bool with true` (timed out)\n"
        ),
        "{}",
        run.text
    );
    assert!(!run.text.contains("with false"), "{}", run.text);
}
