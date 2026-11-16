//! Test commits and implementation commits, told apart by the files they touch.
//!
//! In a crate, `crates/<crate>/`, a **test file** is anything under its `tests/`, and anything in
//! a `proptest-regressions/`, `__fuzz__/` (bolero corpora) or `snapshots/` (insta) directory. Any
//! other file of the crate is an **implementation file**, apart from its `Cargo.toml`, which a
//! test commit may need for a dev-dependency. Files outside the crates belong to neither.
//!
//! A commit that touches a test file is a test commit; one that touches an implementation file is
//! an implementation commit. No commit may be both, so an implementation commit never touches a
//! test file. Rebase-merging keeps the commits apart on `rust` (#23), so the check holds over its
//! whole history. Merge commits are skipped: their changes are their parents'.

use std::path::Path;
use std::process::Command;

use super::Problem;

/// Checks every commit in `since..HEAD`, or in all of `HEAD`'s history.
pub fn check(root: &Path, since: Option<&str>, problems: &mut Vec<Problem>) -> usize {
    let range = match since {
        Some(since) => format!("{since}..HEAD"),
        None => "HEAD".to_owned(),
    };
    let out = Command::new("git")
        .arg("-C")
        .arg(root)
        .args([
            "log",
            "--no-merges",
            "--no-renames",
            "--format=%x1e%H%x1f%s",
            "--name-only",
            &range,
        ])
        .output();
    let out = match out {
        Ok(out) if out.status.success() => out,
        Ok(out) => {
            problems.push(Problem::general(format!(
                "git log {range} failed: {}",
                String::from_utf8_lossy(&out.stderr).trim()
            )));
            return 0;
        }
        Err(error) => {
            problems.push(Problem::general(format!("git doesn't run: {error}")));
            return 0;
        }
    };
    let log = String::from_utf8_lossy(&out.stdout);
    let mut count = 0;
    for commit in log.split('\x1e').filter(|c| !c.trim().is_empty()) {
        count += 1;
        let mut lines = commit.lines();
        let (hash, subject) = lines
            .next()
            .unwrap_or_default()
            .split_once('\x1f')
            .unwrap_or_default();
        let files: Vec<&str> = lines.filter(|l| !l.is_empty()).collect();
        let tests: Vec<&str> = files
            .iter()
            .copied()
            .filter(|f| kind(f) == Kind::Test)
            .collect();
        let implementation: Vec<&str> = files
            .iter()
            .copied()
            .filter(|f| kind(f) == Kind::Implementation)
            .collect();
        if !tests.is_empty() && !implementation.is_empty() {
            problems.push(Problem::general(format!(
                "commit {} \"{subject}\" touches test files and implementation files: a test \
                 commit and an implementation commit are separate commits\n  test files: {}\n  \
                 implementation files: {}",
                &hash[..hash.len().min(12)],
                tests.join(", "),
                implementation.join(", ")
            )));
        }
    }
    count
}

#[derive(PartialEq)]
enum Kind {
    Test,
    Implementation,
    Neither,
}

fn kind(path: &str) -> Kind {
    let parts: Vec<&str> = path.split('/').collect();
    let ["crates", _, in_crate @ ..] = parts.as_slice() else {
        return Kind::Neither;
    };
    match in_crate {
        ["Cargo.toml"] | [] => Kind::Neither,
        ["tests", ..] => Kind::Test,
        [dirs @ .., _file]
            if dirs
                .iter()
                .any(|d| matches!(*d, "proptest-regressions" | "__fuzz__" | "snapshots")) =>
        {
            Kind::Test
        }
        _ => Kind::Implementation,
    }
}
