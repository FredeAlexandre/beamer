//! `cargo xtask kill-matrix`: which spec test kills which mutant, read from the `mutants.out`
//! directories of a nightly run (#15, ADR 0002).
//!
//! cargo-mutants stops a mutant's tests at the first failure, and so knows one killer at most. The
//! nightly passes `--no-fail-fast` to nextest, so each mutant's log names every test that failed
//! under it, and the baseline's log names every test that ran. A spec test is one in a crate's
//! `tests/`: nextest names its binary `<crate>::<test file>`. Unit tests, whose binary is the bare
//! crate, never count, so a mutant only they catch is still a survivor.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

use serde_json::Value;

/// Prints the matrix of `runs` as Markdown, or fails naming what can't be read.
pub fn run(runs: &[impl AsRef<Path>]) -> bool {
    let mut matrix = Matrix::default();
    for dir in runs {
        if let Err(message) = matrix.read(dir.as_ref()) {
            eprintln!("kill-matrix: {}: {message}", dir.as_ref().display());
            return false;
        }
    }
    print!("{}", matrix.markdown(runs.len()));
    true
}

#[derive(Default)]
struct Matrix {
    /// Every spec test that ran in a baseline.
    tests: BTreeSet<String>,
    /// Each mutant, in source order, with its outcome and the spec tests that killed it.
    mutants: BTreeMap<(String, u64, u64, String), Mutant>,
    unviable: usize,
}

struct Mutant {
    name: String,
    outcome: Outcome,
    killers: BTreeSet<String>,
}

#[derive(PartialEq)]
enum Outcome {
    Caught,
    Missed,
    Timeout,
}

impl Matrix {
    fn read(&mut self, dir: &Path) -> Result<(), String> {
        let text = fs::read_to_string(dir.join("outcomes.json"))
            .map_err(|e| format!("no outcomes.json ({e}): not a mutants.out directory"))?;
        let outcomes: Value =
            serde_json::from_str(&text).map_err(|e| format!("outcomes.json: {e}"))?;
        let outcomes = outcomes["outcomes"]
            .as_array()
            .ok_or("outcomes.json has no outcomes")?;
        let log = |outcome: &Value| -> Result<String, String> {
            let path = outcome["log_path"]
                .as_str()
                .ok_or("an outcome has no log")?;
            fs::read_to_string(dir.join(path)).map_err(|e| format!("{path}: {e}"))
        };

        let mut baseline = false;
        for outcome in outcomes {
            let summary = outcome["summary"].as_str().unwrap_or_default();
            if outcome["scenario"] == "Baseline" {
                if summary != "Success" {
                    return Err(format!("its baseline is {summary}, not a success"));
                }
                baseline = true;
                self.tests.extend(
                    test_lines(&log(outcome)?)
                        .into_iter()
                        .filter(|(status, _)| status == "PASS")
                        .map(|(_, test)| test),
                );
                continue;
            }
            let mutant = &outcome["scenario"]["Mutant"];
            let name = mutant["name"]
                .as_str()
                .ok_or("a mutant has no name")?
                .to_owned();
            let outcome_kind = match summary {
                "CaughtMutant" => Outcome::Caught,
                "MissedMutant" => Outcome::Missed,
                "Timeout" => Outcome::Timeout,
                _ => {
                    self.unviable += 1;
                    continue;
                }
            };
            let mut killers = BTreeSet::new();
            if outcome_kind == Outcome::Caught {
                if !no_fail_fast(outcome) {
                    return Err(format!(
                        "`{name}` was tested without nextest's --no-fail-fast, so its log names \
                         only the first test that killed it: run `cargo mutants … -- \
                         --no-fail-fast`, as the nightly does"
                    ));
                }
                killers = summary_failures(&log(outcome)?)
                    .into_iter()
                    .filter(|test| is_spec_test(test))
                    .collect();
            }
            let start = &mutant["span"]["start"];
            let key = (
                mutant["file"].as_str().unwrap_or_default().to_owned(),
                start["line"].as_u64().unwrap_or(0),
                start["column"].as_u64().unwrap_or(0),
                name.clone(),
            );
            self.mutants.insert(
                key,
                Mutant {
                    name,
                    outcome: outcome_kind,
                    killers,
                },
            );
        }
        if !baseline {
            return Err(
                "it has no baseline, so the tests that ran are unknown: run cargo mutants \
                        without --baseline=skip"
                    .into(),
            );
        }
        self.tests.retain(|t| is_spec_test(t));
        Ok(())
    }

    fn markdown(&self, runs: usize) -> String {
        let count = |o: Outcome| self.mutants.values().filter(|m| m.outcome == o).count();
        let mut kills: BTreeMap<&str, usize> = self.tests.iter().map(|t| (t.as_str(), 0)).collect();
        for mutant in self.mutants.values() {
            for killer in &mutant.killers {
                *kills.entry(killer).or_default() += 1;
            }
        }
        let mut out = String::from("# Kill matrix\n\n");
        out += &format!(
            "{} mutants from {runs} run{}: {} caught, {} missed, {} timed out, {} unviable. {} spec \
             tests ran.\n",
            self.mutants.len() + self.unviable,
            if runs == 1 { "" } else { "s" },
            count(Outcome::Caught),
            count(Outcome::Missed),
            count(Outcome::Timeout),
            self.unviable,
            self.tests.len()
        );

        let idle: Vec<&&str> = kills
            .iter()
            .filter(|(_, n)| **n == 0)
            .map(|(t, _)| t)
            .collect();
        out += &format!("\n## Spec tests that kill no mutant ({})\n\n", idle.len());
        out +=
            "Each guards nothing cargo-mutants can express: it is deleted or justified (#15).\n\n";
        for test in idle {
            out += &format!("- `{test}`\n");
        }

        let survivors: Vec<&Mutant> = self
            .mutants
            .values()
            .filter(|m| m.killers.is_empty())
            .collect();
        out += &format!("\n## Mutants no spec test kills ({})\n\n", survivors.len());
        for mutant in survivors {
            let why = match mutant.outcome {
                Outcome::Missed => "missed",
                Outcome::Timeout => "timed out",
                Outcome::Caught => "caught by no spec test",
            };
            out += &format!("- `{}` ({why})\n", mutant.name);
        }

        out += "\n## Mutants each spec test kills\n\n| Spec test | Mutants killed |\n|---|---:|\n";
        for (test, n) in &kills {
            out += &format!("| `{test}` | {n} |\n");
        }

        out += "\n## The spec tests that kill each mutant\n\n";
        for mutant in self.mutants.values().filter(|m| !m.killers.is_empty()) {
            let killers: Vec<String> = mutant.killers.iter().map(|t| format!("`{t}`")).collect();
            out += &format!("- `{}`: {}\n", mutant.name, killers.join(", "));
        }
        out
    }
}

/// Whether the mutant's test phase ran nextest with `--no-fail-fast`.
fn no_fail_fast(outcome: &Value) -> bool {
    outcome["phase_results"].as_array().is_some_and(|phases| {
        phases.iter().any(|phase| {
            phase["phase"] == "Test"
                && phase["argv"]
                    .as_array()
                    .is_some_and(|argv| argv.iter().any(|a| a == "--no-fail-fast"))
        })
    })
}

/// A spec test's nextest binary is `<crate>::<test file>`; a unit test's is the bare crate, and
/// a binary target's or bench's is `<crate>::bin/<name>` or the like.
fn is_spec_test(test: &str) -> bool {
    let binary = test.split(' ').next().unwrap_or_default();
    binary
        .split_once("::")
        .is_some_and(|(_, target)| !target.contains('/'))
}

/// The tests in nextest's last run summary of a log: each one that didn't pass.
fn summary_failures(log: &str) -> BTreeSet<String> {
    let summary = log.rfind("Summary [").map_or("", |at| &log[at..]);
    test_lines(summary.split_once('\n').map_or("", |(_, rest)| rest))
        .into_iter()
        .filter(|(status, _)| !matches!(status.as_str(), "PASS" | "SKIP" | "LEAK"))
        .map(|(_, test)| test)
        .collect()
}

/// nextest's per-test lines: `<STATUS> [ <time>] (<i>/<n>) <binary> <test>`, as the status (with
/// any `TRY n` dropped) and `<binary> <test>`.
fn test_lines(text: &str) -> Vec<(String, String)> {
    let mut lines = Vec::new();
    for line in text.lines() {
        let Some((status, rest)) = line.split_once('[') else {
            continue;
        };
        let Some((time, rest)) = rest.split_once(']') else {
            continue;
        };
        if !time.trim().ends_with('s') {
            continue;
        }
        let status = status.split_whitespace().last().unwrap_or_default();

        let mut rest = rest.trim();
        if rest.starts_with('(') {
            rest = rest.split_once(')').map_or("", |(_, r)| r.trim());
        }
        let mut words = rest.split_whitespace();
        if let (Some(binary), Some(test), None) = (words.next(), words.next(), words.next()) {
            lines.push((status.to_owned(), format!("{binary} {test}")));
        }
    }
    lines
}
