//! The fuzz corpus entries a nightly run found, for the routine to propose as a pull request.
//!
//! The `fuzz-corpus` artifact holds every harness's corpus after the run at its path in the
//! checkout, `<…>/__fuzz__/<harness>/corpus/<entry>` (the upload's pattern starts with `**`, so
//! upload-artifact keeps the whole path). libFuzzer names an entry by its content's hash, so an
//! entry is new exactly when the checkout has no file at its path.

use std::collections::BTreeSet;
use std::fs;
use std::path::Path;

use super::failures::{files, slash_path};
use crate::Result;

/// Prints each new entry's path, one per line, and copies it into `root` with `write`.
pub fn run(artifact: &Path, root: &Path, write: bool) -> Result {
    if !artifact.is_dir() {
        return Err(format!(
            "{} is not a directory: download the artifact with `gh run download <run> -n fuzz-corpus -D <dir>`",
            artifact.display()
        ));
    }
    let mut new = 0;
    let mut gone = BTreeSet::new();
    for file in files(artifact)? {
        let rel = slash_path(file.strip_prefix(artifact).expect("under the artifact"));
        let Some((owner, rest)) = rel.split_once("/__fuzz__/") else {
            continue;
        };
        let mut parts = rest.split('/');
        let (Some(harness), Some("corpus"), Some(_), None) =
            (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            continue; // Not a corpus entry.
        };
        if !root.join(owner).is_dir() {
            gone.insert(format!("{owner}/__fuzz__/{harness}"));
            continue;
        }
        let target = root.join(&rel);
        if target.exists() {
            continue;
        }
        println!("{rel}");
        new += 1;
        if write {
            let dir = target.parent().expect("an entry is in a corpus directory");
            fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
            fs::copy(&file, &target)
                .map_err(|e| format!("cannot copy {rel} into the checkout: {e}"))?;
        }
    }
    for harness in &gone {
        eprintln!("corpus: skipped {harness}: the checkout no longer has its directory");
    }
    eprintln!(
        "corpus: {new} new entr{} {}",
        if new == 1 { "y" } else { "ies" },
        if write {
            "copied into the checkout"
        } else {
            "(run with --write to copy them)"
        }
    );
    Ok(())
}
