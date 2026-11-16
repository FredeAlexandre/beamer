//! `cargo xtask triage`: the nightly triage routine's deterministic half (#44). It reads a nightly
//! run's artifacts as `gh run download` leaves them, in `tests/fixtures/nightly/` (its README says
//! where each comes from), and prints the issues to open as JSON, or the fuzz corpus entries to
//! propose.

mod common;

use common::Run;
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::process::Command;

const REPO: &str = "FredeAlexandre/beamer";
const RUN: &str = "43951664303";
const SHA: &str = "ae22469755d1f06edb0a434cd20a1bf020ef83cb";

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/nightly")
        .join(name)
}

/// `cargo xtask triage issues <fixture> --run <RUN> --repo <REPO>` plus `args`.
fn issues(dir: &str, args: &[&str]) -> Run {
    Run::of(
        Command::new(env!("CARGO_BIN_EXE_xtask"))
            .args(["triage", "issues"])
            .arg(fixture(dir))
            .args(["--run", RUN, "--repo", REPO])
            .args(args.iter().map(|a| match a.strip_prefix('@') {
                Some(file) => fixture(file).into_os_string(),
                None => a.into(),
            })),
    )
}

/// The plan a passing run printed on stdout.
fn plan(run: &Run) -> Value {
    run.passes();
    let json = &run.text[..run.text.rfind('}').map_or(0, |i| i + 1)];
    serde_json::from_str(json).unwrap_or_else(|e| panic!("{e}: not a plan:\n{}", run.text))
}

fn to_open(plan: &Value) -> &Vec<Value> {
    plan["open"].as_array().expect("the plan has `open`")
}

const LANDSCAPE_TRUE: &str =
    "crates/beamer-types/src/lib.rs:6:5: replace is_landscape -> bool with true";
const LANDSCAPE_GE: &str = "crates/beamer-types/src/lib.rs:6:11: replace > with >= in is_landscape";

#[test]
fn the_survivors_of_one_function_open_one_triage_mutant_issue_across_shards() {
    let plan = plan(&issues("mutants-run", &[]));
    let open = to_open(&plan);
    assert_eq!(open.len(), 1, "{plan:#}");
    let issue = &open[0];
    assert_eq!(issue["label"], "triage:mutant");
    let title = issue["title"].as_str().unwrap();
    assert!(
        title.contains("is_landscape") && title.contains("beamer-types"),
        "{title}"
    );
    let body = issue["body"].as_str().unwrap();
    for survivor in [LANDSCAPE_TRUE, LANDSCAPE_GE] {
        assert!(body.contains(survivor), "no {survivor:?} in:\n{body}");
    }
    assert!(
        !body.contains("with false"),
        "a caught mutant is listed:\n{body}"
    );
    assert!(
        !body.contains("with <"),
        "a caught mutant is listed:\n{body}"
    );
}

#[test]
fn a_mutant_issue_links_each_diff_in_its_shard_artifact_and_copies_it() {
    let plan = plan(&issues(
        "mutants-run",
        &["--sha", SHA, "--artifacts", "@mutants-run.artifacts.json"],
    ));
    let body = to_open(&plan)[0]["body"].as_str().unwrap().to_owned();
    // is_landscape -> true survived in shard 0, > to >= in shard 4.
    for link in [
        "`diff/crates__beamer-types__src__lib.rs_line_6_col_5.diff` in [`mutants-linux-shard-0`](https://github.com/FredeAlexandre/beamer/actions/runs/43951664303/artifacts/14267748158)",
        "`diff/crates__beamer-types__src__lib.rs_line_6_col_11.diff` in [`mutants-linux-shard-4`](https://github.com/FredeAlexandre/beamer/actions/runs/43951664303/artifacts/14266373514)",
    ] {
        assert!(body.contains(link), "no {link:?} in:\n{body}");
    }
    assert!(
        body.contains("+    width >= /* ~ changed by cargo-mutants ~ */ height\n"),
        "the diff isn't copied:\n{body}"
    );
    assert!(
        body.contains(&format!(
            "https://github.com/FredeAlexandre/beamer/blob/{SHA}/crates/beamer-types/src/lib.rs#L3-L7"
        )),
        "the function isn't linked at the tested commit:\n{body}"
    );
    assert!(body.contains(
        "[43951664303](https://github.com/FredeAlexandre/beamer/actions/runs/43951664303)"
    ));
}

#[test]
fn a_group_whose_issue_is_open_is_not_opened_again() {
    let first = plan(&issues("mutants-run", &[]));
    let body = to_open(&first)[0]["body"].as_str().unwrap();
    let dir = tempfile::tempdir().unwrap();
    let open = dir.path().join("open.json");
    let issues_json = serde_json::json!([
        { "number": 140, "body": "Another group.\n\n<!-- nightly-triage: mutant crates/beamer-types/src/lib.rs other -->\n" },
        { "number": 141, "body": body },
    ]);
    std::fs::write(&open, issues_json.to_string()).unwrap();

    let again = plan(&issues("mutants-run", &["--open", open.to_str().unwrap()]));
    assert!(to_open(&again).is_empty(), "{again:#}");
    assert_eq!(again["skip"][0]["number"], 141, "{again:#}");
}

#[test]
fn an_open_issue_of_another_function_does_not_hold_a_group_back() {
    let dir = tempfile::tempdir().unwrap();
    let open = dir.path().join("open.json");
    let issues_json = serde_json::json!([
        { "number": 140, "body": "<!-- nightly-triage: mutant crates/beamer-types/src/lib.rs is_portrait -->" },
    ]);
    std::fs::write(&open, issues_json.to_string()).unwrap();
    let plan = plan(&issues("mutants-run", &["--open", open.to_str().unwrap()]));
    assert_eq!(to_open(&plan).len(), 1, "{plan:#}");
}

#[test]
fn each_function_with_survivors_opens_its_own_issue_and_a_timeout_survives() {
    let plan = plan(&issues("two-functions", &[]));
    let open = to_open(&plan);
    let titles: Vec<&str> = open.iter().map(|i| i["title"].as_str().unwrap()).collect();
    assert_eq!(
        titles,
        ["Survivors: beamer-k is_big", "Survivors: beamer-k unused"],
        "{plan:#}"
    );
    let is_big = open[0]["body"].as_str().unwrap();
    assert!(
        is_big.contains(
            "| `crates/beamer-k/src/lib.rs:6:5: replace is_big -> bool with true` | timed out |"
        ),
        "{is_big}"
    );
    assert!(
        !is_big.contains("with false"),
        "an unviable mutant is listed:\n{is_big}"
    );
    let unused = open[1]["body"].as_str().unwrap();
    assert!(unused.contains("these 4 mutants of `unused`"), "{unused}");
}

#[test]
fn a_directory_that_is_not_there_fails() {
    issues("no-such-run", &[]).fails_naming(&["no-such-run", "gh run download"]);
}

/// The issue of `plan` titled `title`.
fn titled<'p>(plan: &'p Value, title: &str) -> &'p Value {
    to_open(plan)
        .iter()
        .find(|i| i["title"] == title)
        .unwrap_or_else(|| panic!("no issue titled {title:?} in:\n{plan:#}"))
}

const HARNESS: &str = "crates/beamer-wire/tests/__fuzz__/decode_frame";

#[test]
fn the_crashes_of_one_fuzz_harness_open_one_triage_fuzz_issue_carrying_each_input() {
    let plan = plan(&issues(
        "failures-run",
        &["--jobs", "@failures-run.jobs.json"],
    ));
    let issue = titled(&plan, "Fuzz crash: beamer-wire decode_frame");
    assert_eq!(issue["label"], "triage:fuzz");
    let body = issue["body"].as_str().unwrap();
    // Each input, restored into the harness's corpus, where `cargo test` replays it. The base64
    // is the fixture's bytes, as `base64` prints them.
    for (input, base64) in [("crash-5f0e", "QkVBTf8ABw=="), ("crash-a913", "QkVBTQE=")] {
        let restore = format!("base64 -d > {HARNESS}/corpus/{input} <<'EOF'\n{base64}\nEOF\n");
        assert!(body.contains(&restore), "no {restore:?} in:\n{body}");
    }
    assert!(
        body.contains("https://github.com/FredeAlexandre/beamer/actions/runs/1/job/14"),
        "{body}"
    );
    assert!(
        body.ends_with(&format!("<!-- nightly-triage: fuzz {HARNESS} -->\n")),
        "{body}"
    );
}

#[test]
fn a_failing_model_test_opens_a_triage_model_issue_with_its_counterexample() {
    let plan = plan(&issues("failures-run", &[]));
    let issue = titled(&plan, "Model failure: beamer-model one_owner_at_a_time");
    assert_eq!(issue["label"], "triage:model");
    let body = issue["body"].as_str().unwrap();
    assert!(
        body.contains(
            "  Deliver { src: 0, dst: 1, msg: Claim }\n  Deliver { src: 2, dst: 1, msg: Claim }\n"
        ),
        "no counterexample in:\n{body}"
    );
    assert!(
        body.contains("cargo test --release -p beamer-model --test handover -- --include-ignored --exact one_owner_at_a_time"),
        "no replay command in:\n{body}"
    );
    assert!(
        !body.contains("handover_completes"),
        "a passing test is in:\n{body}"
    );
}

#[test]
fn a_model_package_that_did_not_build_opens_one_issue_with_its_errors_and_a_passing_one_none() {
    let plan = plan(&issues("failures-run", &[]));
    let issue = titled(&plan, "Model failure: beamer-broken did not run");
    assert_eq!(issue["label"], "triage:model");
    let body = issue["body"].as_str().unwrap();
    assert!(
        body.contains("error[E0425]: cannot find value `owner` in this scope"),
        "{body}"
    );
    let titles: Vec<&str> = to_open(&plan)
        .iter()
        .map(|i| i["title"].as_str().unwrap())
        .collect();
    assert!(
        !titles.iter().any(|t| t.contains("beamer-sim")),
        "{titles:?}"
    );
}

#[test]
fn jobs_that_failed_leaving_nothing_to_triage_open_one_needs_alex_issue() {
    let plan = plan(&issues(
        "failures-run",
        &["--jobs", "@failures-run.jobs.json"],
    ));
    let broken: Vec<&Value> = to_open(&plan)
        .iter()
        .filter(|i| i["label"] == "needs:alex")
        .collect();
    assert_eq!(broken.len(), 1, "{plan:#}");
    let body = broken[0]["body"].as_str().unwrap();
    assert!(body.starts_with("Alex: "), "{body}");
    assert!(
        body.contains("- [Mutants linux 2/8](https://github.com/FredeAlexandre/beamer/actions/runs/1/job/12): failure\n"),
        "{body}"
    );
    assert!(
        body.contains("- [Bench (divan trends)]("),
        "a cancelled job isn't listed:\n{body}"
    );
    // The fuzz and model jobs failed with their findings: those are their own issues.
    assert!(!body.contains("[Fuzz (bolero)]"), "{body}");
    assert!(!body.contains("[Model (stateright"), "{body}");
}

#[test]
fn a_fuzz_job_that_failed_without_a_crash_counts_as_broken() {
    let dir = tempfile::tempdir().unwrap();
    let jobs = dir.path().join("jobs.json");
    std::fs::write(
        &jobs,
        r#"{"jobs":[{"name":"Fuzz (bolero)","conclusion":"failure","url":"https://example/job/14"}]}"#,
    )
    .unwrap();
    let plan = plan(&issues("mutants-run", &["--jobs", jobs.to_str().unwrap()]));
    let broken = to_open(&plan).iter().find(|i| i["label"] == "needs:alex");
    let body = broken.unwrap_or_else(|| panic!("{plan:#}"))["body"]
        .as_str()
        .unwrap();
    assert!(
        body.contains("- [Fuzz (bolero)](https://example/job/14): failure"),
        "{body}"
    );
}

#[test]
fn a_nightly_whose_jobs_all_passed_opens_no_needs_alex_issue() {
    let plan = plan(&issues(
        "mutants-run",
        &["--jobs", "@mutants-run.jobs.json"],
    ));
    assert!(
        to_open(&plan).iter().all(|i| i["label"] != "needs:alex"),
        "{plan:#}"
    );
}

#[test]
fn failed_jobs_while_their_issue_is_open_become_a_comment_on_it() {
    let first = plan(&issues(
        "failures-run",
        &["--jobs", "@failures-run.jobs.json"],
    ));
    let open_body = to_open(&first)
        .iter()
        .find(|i| i["label"] == "needs:alex")
        .unwrap()["body"]
        .clone();
    let dir = tempfile::tempdir().unwrap();
    let open = dir.path().join("open.json");
    std::fs::write(
        &open,
        serde_json::json!([{ "number": 150, "body": open_body }]).to_string(),
    )
    .unwrap();

    let again = plan(&issues(
        "failures-run",
        &[
            "--jobs",
            "@failures-run.jobs.json",
            "--open",
            open.to_str().unwrap(),
        ],
    ));
    assert!(
        to_open(&again).iter().all(|i| i["label"] != "needs:alex"),
        "{again:#}"
    );
    let comment = &again["comment"][0];
    assert_eq!(comment["number"], 150, "{again:#}");
    assert!(
        comment["body"].as_str().unwrap().contains(
            "[Mutants linux 2/8](https://github.com/FredeAlexandre/beamer/actions/runs/1/job/12)"
        ),
        "{again:#}"
    );
}

/// A checkout holding `beamer-wire`, whose `decode_frame` corpus already has the entry `0c1d`.
fn checkout() -> tempfile::TempDir {
    let root = tempfile::tempdir().unwrap();
    let corpus = root.path().join(HARNESS).join("corpus");
    std::fs::create_dir_all(&corpus).unwrap();
    std::fs::write(corpus.join("0c1d"), b"BEAM\x02\x00").unwrap();
    root
}

/// `cargo xtask triage corpus` on the fixture's `fuzz-corpus` artifact, in `root`.
fn corpus(root: &Path, args: &[&str]) -> Run {
    Run::of(
        Command::new(env!("CARGO_BIN_EXE_xtask"))
            .args(["triage", "corpus"])
            .arg(fixture("failures-run/fuzz-corpus"))
            .arg("--root")
            .arg(root)
            .args(args),
    )
}

#[test]
fn corpus_lists_the_entries_the_checkout_lacks() {
    let root = checkout();
    let run = corpus(root.path(), &[]);
    run.passes();
    assert!(
        run.text.contains(&format!("{HARNESS}/corpus/7e2a\n")),
        "{}",
        run.text
    );
    assert!(
        !run.text.contains("0c1d"),
        "an entry the checkout has is listed:\n{}",
        run.text
    );
    assert!(
        !root.path().join(HARNESS).join("corpus/7e2a").exists(),
        "an entry was copied without --write"
    );
}

#[test]
fn corpus_skips_and_names_a_harness_whose_crate_the_checkout_lacks() {
    let root = checkout();
    let run = corpus(root.path(), &["--write"]);
    run.passes();
    assert!(run.text.contains("crates/beamer-gone"), "{}", run.text);
    assert!(!root.path().join("crates/beamer-gone").exists());
}

#[test]
fn corpus_write_copies_the_new_entries_into_the_checkout() {
    let root = checkout();
    corpus(root.path(), &["--write"]).passes();
    let copied = std::fs::read(root.path().join(HARNESS).join("corpus/7e2a")).unwrap();
    assert_eq!(copied, b"BEAM\x03");
    // A second run finds nothing new.
    let again = corpus(root.path(), &[]);
    again.passes();
    assert!(!again.text.contains("/corpus/"), "{}", again.text);
}

#[test]
fn an_empty_sha_links_no_source_lines() {
    let plan = plan(&issues("mutants-run", &["--sha", ""]));
    let body = to_open(&plan)[0]["body"].as_str().unwrap();
    assert!(!body.contains("/blob/"), "{body}");
    assert!(
        body.contains("in `crates/beamer-types/src/lib.rs`, mutated"),
        "{body}"
    );
}
