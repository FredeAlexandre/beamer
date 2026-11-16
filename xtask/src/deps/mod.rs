//! `cargo xtask deps`: the dependency and seam rules of #16 and ADR 0004.

mod source;
mod table;
mod workspace;

use source::SourceFile;
use std::path::Path;
use std::process::ExitCode;
use table::{Group, Row};
use workspace::{Crate, DepKind, Workspace};

/// Checks the workspace at `manifest`, prints every broken rule, and fails if there is one.
pub fn run(manifest: &Path) -> ExitCode {
    let workspace = match Workspace::load(manifest) {
        Ok(workspace) => workspace,
        Err(error) => {
            eprintln!("deps: {error}");
            return ExitCode::FAILURE;
        }
    };
    let problems = check(&workspace);
    if problems.is_empty() {
        println!(
            "deps: every rule holds across {} crates",
            workspace.crates.len()
        );
        ExitCode::SUCCESS
    } else {
        for problem in &problems {
            eprintln!("{problem}");
        }
        eprintln!("deps: {} broken rules", problems.len());
        ExitCode::FAILURE
    }
}

fn check(workspace: &Workspace) -> Vec<String> {
    let mut problems = Vec::new();
    for krate in &workspace.crates {
        let row = table::row(&krate.name);
        match &row {
            Some(row) => allowed_dependencies(workspace, krate, row, &mut problems),
            None => problems.push(format!(
                "{}: not in the allowed-dependency table (xtask/src/deps/table.rs); add its row",
                krate.name
            )),
        }
        let group = row.map(|row| row.group);
        if group.is_some_and(Group::is_sans_io) {
            clippy_lists(krate, &mut problems);
        }
        ffi_fence(krate, &mut problems);
        os_bindings(krate, &mut problems);
        let files = source::parse_all(&krate.name, &krate.dir, &mut problems);
        if !matches!(group, Some(Group::Ffi | Group::Sys)) {
            no_foreign_abi(krate, &files, &mut problems);
        }
        if group == Some(Group::Ffi) {
            thin_shims(krate, &files, &mut problems);
        }
    }
    problems
}

/// The table's rows: normal and build dependencies, on workspace crates and, where the row lists
/// them, on crates from outside. The `-ffi` crates have their own rule, [`ffi_fence`].
fn allowed_dependencies(
    workspace: &Workspace,
    krate: &Crate,
    row: &Row,
    problems: &mut Vec<String>,
) {
    for dep in krate.deps.iter().filter(|dep| dep.kind != DepKind::Dev) {
        let (allowed, may_use) = if workspace.has(&dep.name) {
            let allowed =
                ffi_owner(&dep.name).is_some() || row.may_use.contains(&dep.name.as_str());
            (allowed, list(&row.may_use))
        } else if let Some(outside) = row.outside {
            (outside.contains(&dep.name.as_str()), list(outside))
        } else if row.group.is_sans_io() && table::IO_CRATES.contains(&dep.name.as_str()) {
            (false, "no crate that does I/O".into())
        } else {
            continue;
        };
        if !allowed {
            problems.push(format!(
                "{}: depends on {}, which the table doesn't allow (it may use: {may_use})",
                krate.name, dep.name
            ));
        }
    }
}

/// The lists every sans-I/O crate carries as its `clippy.toml`.
const SANS_IO_CLIPPY: &str = include_str!("../../sans-io.clippy.toml");

/// Rules 1, 2 and 5: a sans-I/O crate carries `sans-io.clippy.toml` as its `clippy.toml`, so
/// `cargo clippy` rejects its clocks, threads, sockets and files. Line endings don't count.
fn clippy_lists(krate: &Crate, problems: &mut Vec<String>) {
    let fix = "copy xtask/sans-io.clippy.toml into the crate as clippy.toml";
    match std::fs::read_to_string(krate.dir.join("clippy.toml")) {
        Err(_) => problems.push(format!("{}: has no clippy.toml; {fix}", krate.name)),
        Ok(copy) if copy.replace("\r\n", "\n") != SANS_IO_CLIPPY.replace("\r\n", "\n") => {
            problems.push(format!(
                "{}: clippy.toml differs from xtask/sans-io.clippy.toml; {fix}",
                krate.name
            ));
        }
        Ok(_) => {}
    }
}

/// Rule 3: a `-ffi` crate is used only by its own adapter, as any kind of dependency.
fn ffi_fence(krate: &Crate, problems: &mut Vec<String>) {
    for dep in &krate.deps {
        if let Some(owner) = ffi_owner(&dep.name)
            && krate.name != owner
        {
            problems.push(format!(
                "{}: depends on {}; only {owner} may use {}",
                krate.name, dep.name, dep.name
            ));
        }
    }
}

/// `beamer-mac` for `beamer-mac-ffi`.
fn ffi_owner(name: &str) -> Option<&str> {
    name.strip_suffix("-ffi")
        .filter(|owner| owner.starts_with("beamer-"))
}

/// Rule 3: the OS binding crates are named only by their own `-ffi` crate.
fn os_bindings(krate: &Crate, problems: &mut Vec<String>) {
    for dep in &krate.deps {
        let owner = if dep.name == "windows" || dep.name.starts_with("windows-") {
            "beamer-win-ffi"
        } else if dep.name.starts_with("objc2") {
            "beamer-mac-ffi"
        } else {
            continue;
        };
        if krate.name != owner {
            problems.push(format!(
                "{}: names {}; only {owner} may name the OS bindings",
                krate.name, dep.name
            ));
        }
    }
}

/// Rule 3: no `extern "C"` (or any foreign ABI) outside the `-ffi` and `-sys` crates.
fn no_foreign_abi(krate: &Crate, files: &[SourceFile], problems: &mut Vec<String>) {
    for file in files {
        for (abi, line) in source::extern_abis(&file.syntax) {
            problems.push(format!(
                "{}: names extern \"{abi}\" at {}:{line}; foreign calls live in an -ffi crate",
                krate.name, file.path
            ));
        }
    }
}

/// The `-ffi` lint (#15, #16): an `-ffi` crate's functions are thin shims that branch only to
/// map errors, since the crate is skipped for mutation and verified only on the real machines.
fn thin_shims(krate: &Crate, files: &[SourceFile], problems: &mut Vec<String>) {
    let adapter = ffi_owner(&krate.name).unwrap_or("its adapter");
    for file in files.iter().filter(|file| file.path.starts_with("src/")) {
        for branch in source::branches(&file.syntax) {
            problems.push(format!(
                "{}: `{}` in {} ({}:{}) does more than map an error; move the logic to {adapter}",
                krate.name, branch.construct, branch.function, file.path, branch.line
            ));
        }
    }
}

fn list(names: &[&str]) -> String {
    if names.is_empty() {
        "nothing".into()
    } else {
        names.join(", ")
    }
}
