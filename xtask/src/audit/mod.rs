//! `cargo xtask test-audit`: what keeps the spec tests honest (#15, ADR 0002), and the Python
//! ledger. The rules it checks are written in `spec/README.md`.

mod commits;
mod ledger;
mod python;
mod source;

use std::collections::HashMap;
use std::fmt;
use std::path::Path;

use crate::spec;

/// What the audit runs on, from the command line.
pub struct Options<'a> {
    pub root: &'a Path,
    /// Check only the commits after this one.
    pub since: Option<&'a str>,
    /// Where the Python tests the ledger accounts for live.
    pub python_ref: &'a str,
    /// Require the ledger to account for every Python test.
    pub complete: bool,
}

/// Runs every check, prints every problem, and fails if there is one.
pub fn run(options: &Options) -> bool {
    let mut problems = Vec::new();
    let rules = match spec::rules(options.root) {
        Ok(rules) => rules
            .into_iter()
            .map(|r| (r.id, r.version))
            .collect::<HashMap<_, _>>(),
        Err(message) => {
            eprintln!("test-audit: {message}");
            return false;
        }
    };

    let tests = spec_tests(options.root, &rules, &mut problems);
    println!("test-audit: {tests} spec tests in crates/*/tests/");
    for (file, line) in source::verify_lines_outside_tests(options.root) {
        problems.push(Problem::at(
            &file,
            line,
            "an `r[verify]` line outside tests/: only the spec tests in a crate's tests/ \
             verify rules, and an implementer's #[cfg(test)] unit tests never count"
                .into(),
        ));
    }
    for (file, line) in source::unjustified_skips(options.root) {
        problems.push(Problem::at(
            &file,
            line,
            "a `mutants::skip` without its reason: put `// mutants-justified: <reason> \
             (<rule id>)` directly above it"
                .into(),
        ));
    }
    let commits = commits::check(options.root, options.since, &mut problems);
    println!("test-audit: {commits} commits checked for test and implementation files together");
    ledger::check(options, &rules, &mut problems);

    for problem in &problems {
        eprintln!("{problem}");
    }
    if problems.is_empty() {
        println!("test-audit: no problems");
        true
    } else {
        eprintln!(
            "test-audit: {} problem(s); the rules are in spec/README.md",
            problems.len()
        );
        false
    }
}

/// Checks each spec test's citations, assertion and skip reason, and returns how many there are.
fn spec_tests(root: &Path, rules: &HashMap<String, u32>, problems: &mut Vec<Problem>) -> usize {
    let tests = source::spec_tests(root, problems);
    for test in &tests {
        let at = |message: String| {
            Problem::at(&test.file, test.line, format!("{}: {message}", test.name))
        };
        if test.cites.is_empty() {
            problems.push(at(
                "no `// r[verify <rule id>]` line directly above it: every spec test cites the \
                 rules it verifies"
                    .into(),
            ));
        }
        for cite in &test.cites {
            match rules.get(&cite.id) {
                None => problems.push(at(format!(
                    "cites `{}`, which is not a rule in spec/",
                    cite.id
                ))),
                Some(&current) if current != cite.version => problems.push(at(format!(
                    "cites `{}` at version {}, but the rule is at version {current}: cite `{}`",
                    cite.id,
                    cite.version,
                    cited(&cite.id, current)
                ))),
                Some(_) => {}
            }
        }
        if !test.asserts {
            problems.push(at(
                "asserts nothing: no `assert*!`, `prop_assert*!` or `panic!`, in it, in a helper \
                 of the crate's tests/ it calls or in the reference model and system under test \
                 of the state machine it runs, and no #[should_panic]"
                    .into(),
            ));
        }
        if test.ignored_without_reason {
            problems.push(at(
                "ignored without a reason: write `#[ignore = \"<why>\"]`".into(),
            ));
        }
    }
    tests.len()
}

/// A rule id as a test cites it: with `+n` from version 2 on.
fn cited(id: &str, version: u32) -> String {
    match version {
        1 => id.to_owned(),
        n => format!("{id}+{n}"),
    }
}

/// One thing the audit found wrong, at a place in a file or in general.
pub struct Problem {
    place: Option<(String, usize)>,
    message: String,
}

impl Problem {
    fn at(file: &str, line: usize, message: String) -> Self {
        Problem {
            place: Some((file.to_owned(), line)),
            message,
        }
    }

    fn general(message: String) -> Self {
        Problem {
            place: None,
            message,
        }
    }
}

impl fmt::Display for Problem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some((file, line)) = &self.place {
            write!(f, "{file}:{line}: ")?;
        }
        f.write_str(&self.message)
    }
}
