//! `cargo xtask copy-tests <export>`: a test agent's work, from a blind export back onto the real
//! task branch (#32).
//!
//! Only tests come back, with the manifest and lock changes their dev-dependencies need. `blind`
//! keeps a copy of those files as they were exported, under [`BASE`], so a file changed both in
//! the export and on the branch since is refused instead of overwritten.

use std::fs;
use std::path::Path;

use crate::{Result, workspace};

/// Where an export keeps its returnable files as they were when it was written.
pub const BASE: &str = ".blind/base";

/// Whether a file may come back from an export: a crate's `tests/`, a `Cargo.toml` or the
/// `Cargo.lock`.
pub fn returnable(rel: &str) -> bool {
    let parts: Vec<&str> = rel.split('/').collect();
    matches!(
        parts.as_slice(),
        ["crates", _, "tests", _, ..] | ["Cargo.lock"]
    ) || parts.last() == Some(&"Cargo.toml")
}

pub fn run(export: &Path) -> Result {
    let root = workspace::root()?;
    let copied = copy_back(export, &root)?;
    if copied.is_empty() {
        println!(
            "Nothing to copy: no test or manifest changed in {}",
            export.display()
        );
    }
    for rel in copied {
        println!("copied {rel}");
    }
    Ok(())
}

/// Copies every returnable file the test agent changed from `export` into `root`, and returns
/// their paths. Copies nothing if any of them also changed in `root` since the export.
fn copy_back(export: &Path, root: &Path) -> Result<Vec<String>> {
    let base = export.join(BASE);
    if !base.is_dir() {
        return Err(format!(
            "{} is not a blind export: it has no {BASE}",
            export.display()
        ));
    }
    let mut changed = Vec::new();
    let mut conflicts = Vec::new();
    for rel in files_under(export)?
        .into_iter()
        .filter(|rel| returnable(rel))
    {
        let ours = fs::read(export.join(&rel)).map_err(|e| format!("cannot read {rel}: {e}"))?;
        let exported = fs::read(base.join(&rel)).ok();
        let theirs = fs::read(root.join(&rel)).ok();
        if exported.as_ref() == Some(&ours) || theirs.as_ref() == Some(&ours) {
            continue; // the agent didn't touch it, or the branch already has it
        }
        if theirs == exported {
            changed.push((rel, ours));
        } else {
            conflicts.push(rel);
        }
    }
    if !conflicts.is_empty() {
        return Err(format!(
            "these files changed both in the export and here since the export was written; \
             nothing was copied. Merge them by hand:\n  {}",
            conflicts.join("\n  ")
        ));
    }
    for (rel, bytes) in &changed {
        let to = root.join(rel);
        fs::create_dir_all(to.parent().expect("a file has a parent"))
            .and_then(|()| fs::write(&to, bytes))
            .map_err(|e| format!("cannot write {}: {e}", to.display()))?;
    }
    Ok(changed.into_iter().map(|(rel, _)| rel).collect())
}

/// Every file under `dir`, relative and with `/` separators, skipping build output (`target/`)
/// and the export's own bookkeeping (`.blind/`).
fn files_under(dir: &Path) -> Result<Vec<String>> {
    let mut files = Vec::new();
    let mut pending = vec![String::new()];
    while let Some(rel_dir) = pending.pop() {
        let entries = fs::read_dir(dir.join(&rel_dir))
            .map_err(|e| format!("cannot list {}: {e}", dir.join(&rel_dir).display()))?;
        for entry in entries {
            let entry = entry.map_err(|e| e.to_string())?;
            let name = entry.file_name().to_string_lossy().into_owned();
            let rel = if rel_dir.is_empty() {
                name.clone()
            } else {
                format!("{rel_dir}/{name}")
            };
            let kind = entry.file_type().map_err(|e| e.to_string())?;
            if kind.is_dir() && name != "target" && name != ".blind" {
                pending.push(rel);
            } else if kind.is_file() {
                files.push(rel);
            }
        }
    }
    files.sort();
    Ok(files)
}
