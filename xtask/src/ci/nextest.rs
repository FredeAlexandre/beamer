//! `cargo xtask nextest`: the merge gate's test run. Spec tests are merged red, each panicking at
//! a `todo!()` of the skeleton, and stay red until their group is implemented (#23). Such a test
//! is pending, not failed; a test failing any other way fails the gate.

use std::fs;
use std::path::Path;
use std::process::Command;

use super::crates::{self, Job, Runner};
use crate::red_first::TODO_PANIC;
use crate::{Result, workspace};

/// A test that failed, as nextest's JUnit report has it.
#[derive(Debug, PartialEq, Eq)]
pub struct Failed {
    /// The test binary, as nextest names it: `<crate>::<binary>`.
    pub binary: String,
    pub name: String,
    /// It panicked at a `todo!()`.
    pub pending: bool,
}

/// Runs nextest's `ci` profile on the crates `runner` tests, with `args` added, and judges it.
pub fn run(root: &Path, runner: Runner, args: &[String]) -> Result {
    let selection = crates::select(root, runner, Job::Test)?;
    let report = workspace::target_dir(root).join("nextest/ci/junit.xml");
    let _ = fs::remove_file(&report);
    let status = Command::new(std::env::var_os("CARGO").unwrap_or("cargo".into()))
        .args(["nextest", "run", "--profile", "ci"])
        .args(selection.args())
        .args(args)
        .current_dir(root)
        .status()
        .map_err(|e| format!("cannot run cargo nextest: {e}"))?;

    let failed = match fs::read_to_string(&report) {
        Ok(xml) => failures(&xml)?,
        Err(_) if status.success() => Vec::new(),
        Err(_) => return Err(format!("nextest failed ({status}) before it ran the tests")),
    };
    let (pending, failed): (Vec<_>, Vec<_>) = failed.into_iter().partition(|f| f.pending);
    for test in &pending {
        println!(
            "pending  {} {}: panics at a todo!()",
            test.binary, test.name
        );
    }
    for test in &failed {
        println!("FAILED   {} {}", test.binary, test.name);
    }
    if !failed.is_empty() {
        return Err(format!(
            "{} test(s) failed, not at a todo!(); {} pending",
            failed.len(),
            pending.len()
        ));
    }
    if !status.success() && pending.is_empty() {
        return Err(format!("nextest failed ({status})"));
    }
    println!(
        "nextest gate: passed, {} test(s) pending at a todo!()",
        pending.len()
    );
    Ok(())
}

/// The failed tests of a nextest JUnit report.
pub fn failures(xml: &str) -> Result<Vec<Failed>> {
    let doc = roxmltree::Document::parse(xml)
        .map_err(|e| format!("nextest's JUnit report doesn't parse: {e}"))?;
    let mut failed = Vec::new();
    for case in doc.descendants().filter(|n| n.has_tag_name("testcase")) {
        let Some(failure) = case
            .children()
            .find(|n| n.has_tag_name("failure") || n.has_tag_name("error"))
        else {
            continue;
        };
        // The panic message is in the failure's text, and in the captured stderr beside it.
        let output = std::iter::once(failure)
            .chain(case.children().filter(|n| n.has_tag_name("system-err")))
            .flat_map(|n| n.text())
            .collect::<String>();
        failed.push(Failed {
            binary: case.attribute("classname").unwrap_or_default().to_owned(),
            name: case.attribute("name").unwrap_or_default().to_owned(),
            pending: panics_at_todo(&output),
        });
    }
    Ok(failed)
}

/// Whether a test's output is a panic at a `todo!()`: the line after `panicked at` is the
/// `todo!()` message, bare or with its own message after a colon.
fn panics_at_todo(output: &str) -> bool {
    let mut lines = output.lines();
    while let Some(line) = lines.next() {
        if line.contains("panicked at")
            && let Some(message) = lines.next()
        {
            return message == TODO_PANIC || message.starts_with(&format!("{TODO_PANIC}: "));
        }
    }
    false
}
