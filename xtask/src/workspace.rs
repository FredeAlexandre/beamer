//! The workspace `cargo xtask` runs in, and the git checkout that holds it.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::Result;

/// The root of the workspace around the current directory: the nearest `Cargo.toml` with a
/// `[workspace]` table.
pub fn root() -> Result<PathBuf> {
    let cwd = std::env::current_dir().map_err(|e| format!("no current directory: {e}"))?;
    cwd.ancestors()
        .find(|dir| {
            fs::read_to_string(dir.join("Cargo.toml"))
                .is_ok_and(|toml| toml.lines().any(|line| line.trim() == "[workspace]"))
        })
        .map(Path::to_path_buf)
        .ok_or_else(|| format!("{} is not inside a Cargo workspace", cwd.display()))
}

/// `git <args>` in `dir`, its stdout on success.
pub fn git(dir: &Path, args: &[&str]) -> Result<String> {
    let out = Command::new("git")
        .args(args)
        .current_dir(dir)
        .output()
        .map_err(|e| format!("cannot run git: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "`git {}` failed in {}:\n{}",
            args.join(" "),
            dir.display(),
            String::from_utf8_lossy(&out.stderr).trim_end()
        ));
    }
    String::from_utf8(out.stdout)
        .map_err(|e| format!("`git {}` printed non-UTF-8: {e}", args.join(" ")))
}

/// Every file of the checkout that git would commit: tracked or untracked, never ignored, and
/// still on disk. Paths are relative, with `/` separators.
pub fn files(root: &Path) -> Result<Vec<String>> {
    let listed = git(
        root,
        &[
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
    )?;
    let mut files: Vec<String> = listed
        .split('\0')
        .filter(|path| !path.is_empty() && root.join(path).is_file())
        .map(str::to_owned)
        .collect();
    files.sort();
    files.dedup();
    Ok(files)
}
