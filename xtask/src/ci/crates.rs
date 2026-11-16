//! `cargo xtask crates`: the crates each CI runner covers, read from the dependency table. The CI
//! workflow and the nightly both ask it, so they split the workspace the same way.
//!
//! - **test** (clippy and nextest): Linux takes the OS-independent crates, the sans-I/O ones and
//!   `xtask`, which also proves the core builds off macOS and Windows. macOS and Windows take the
//!   whole workspace (#22).
//! - **mutants**: each crate is mutated on one runner, the one of its OS, because cargo-mutants
//!   doesn't understand `#[cfg(target_os)]` (#15): `beamer-win` on Windows; `beamer-mac`,
//!   `beamer-runtime` and `beamer` on macOS, where they are tested; the OS-independent crates on
//!   Linux. `xtask` is no app crate and is never mutated, and neither are the crates
//!   [`unmutated`] justifies.

use std::fmt;
use std::path::Path;

use clap::ValueEnum;

use crate::Result;
use crate::deps::table::{self, Group};
use crate::deps::workspace::Workspace;

#[derive(Clone, Copy, PartialEq, Eq, Debug, ValueEnum)]
pub enum Runner {
    Linux,
    Macos,
    Windows,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, ValueEnum)]
pub enum Job {
    /// clippy and nextest.
    Test,
    /// cargo mutants.
    Mutants,
}

/// The crates a job covers on a runner.
#[derive(Debug, PartialEq, Eq)]
pub enum Selection {
    Workspace,
    /// These packages, sorted; empty when the runner has nothing to do.
    Packages(Vec<String>),
}

impl Selection {
    /// The cargo arguments that select it.
    pub fn args(&self) -> Vec<String> {
        match self {
            Selection::Workspace => vec!["--workspace".into()],
            Selection::Packages(names) => names
                .iter()
                .flat_map(|name| ["-p".to_owned(), name.clone()])
                .collect(),
        }
    }
}

impl fmt::Display for Selection {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.args().join(" "))
    }
}

/// Why a crate is never mutated, for the crates that aren't. Each reason is the crate's one
/// justification (#33); the triage of survivors never sees these crates.
pub fn unmutated(group: Group) -> Option<&'static str> {
    match group {
        Group::Ffi => Some(
            "a -ffi shim only forwards to the OS (#16, rule 3): it makes no decision a test \
             could pin, and the machine tier exercises it",
        ),
        Group::Ui => Some(
            "beamer-ui only draws the view-model with Rune: its decisions live in \
             beamer-viewmodel, which is mutated, and the human tier checks what it draws",
        ),
        Group::Sys => {
            Some("a -sys crate is bindings to vendored C, with no Rust logic of ours to mutate")
        }
        _ => None,
    }
}

/// The runner a crate's mutants run on, or `None` when it is never mutated.
fn mutants_runner(name: &str, group: Group) -> Option<Runner> {
    if unmutated(group).is_some() || group == Group::Tool {
        return None;
    }
    Some(match (group, name) {
        (Group::Adapter, "beamer-win") => Runner::Windows,
        (Group::Adapter | Group::Runtime | Group::Bin, _) => Runner::Macos,
        _ => Runner::Linux,
    })
}

/// Whether Linux runs a crate's clippy and tests.
fn tested_on_linux(group: Group) -> bool {
    group.is_sans_io() || group == Group::Tool
}

/// The crates `job` covers on `runner`, in the workspace at `root`.
pub fn select(root: &Path, runner: Runner, job: Job) -> Result<Selection> {
    if job == Job::Test && runner != Runner::Linux {
        return Ok(Selection::Workspace);
    }
    let workspace = Workspace::load(&root.join("Cargo.toml"))?;
    let mut names = Vec::new();
    for krate in &workspace.crates {
        let group = table::row(&krate.name)
            .ok_or_else(|| {
                format!(
                    "{} has no row in the dependency table (xtask/src/deps/table.rs), so no \
                     runner is chosen for it; `cargo xtask deps` fails until it has one",
                    krate.name
                )
            })?
            .group;
        let covered = match job {
            Job::Test => tested_on_linux(group),
            Job::Mutants => mutants_runner(&krate.name, group) == Some(runner),
        };
        if covered {
            names.push(krate.name.clone());
        }
    }
    names.sort();
    Ok(Selection::Packages(names))
}
