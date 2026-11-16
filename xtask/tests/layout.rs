//! The workspace layout from #16 and ADR 0004: every crate lives flat in `crates/`, its directory
//! named exactly as the crate, and every crate is `beamer` or `beamer-*`.

use std::fs;
use std::path::Path;

#[test]
fn every_crate_lives_flat_in_crates_under_its_own_name() {
    let crates = Path::new(env!("CARGO_MANIFEST_DIR")).join("../crates");
    let mut problems = Vec::new();
    for entry in fs::read_dir(&crates).expect("crates/ exists") {
        let path = entry.unwrap().path();
        if !path.is_dir() {
            continue;
        }
        let dir = path.file_name().unwrap().to_str().unwrap().to_owned();
        let Ok(manifest) = fs::read_to_string(path.join("Cargo.toml")) else {
            problems.push(format!("crates/{dir} has no Cargo.toml"));
            continue;
        };
        let manifest: toml::Table = manifest.parse().expect("Cargo.toml parses");
        let name = manifest
            .get("package")
            .and_then(|p| p.get("name"))
            .and_then(|n| n.as_str());
        match name {
            Some(name) if name != dir => {
                problems.push(format!("crates/{dir} holds crate `{name}`"));
            }
            Some(_) if dir != "beamer" && !dir.starts_with("beamer-") => {
                problems.push(format!("crates/{dir} is not `beamer` or `beamer-*`"));
            }
            Some(_) => {}
            None => problems.push(format!("crates/{dir}/Cargo.toml has no package name")),
        }
    }
    assert!(
        problems.is_empty(),
        "layout broken:\n{}",
        problems.join("\n")
    );
}
