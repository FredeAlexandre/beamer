//! The workspace as `cargo metadata` sees it: its crates and their direct dependencies.

use serde_json::Value;
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct Workspace {
    pub crates: Vec<Crate>,
}

pub struct Crate {
    pub name: String,
    /// The directory holding the crate's `Cargo.toml`.
    pub dir: PathBuf,
    pub deps: Vec<Dep>,
}

pub struct Dep {
    /// The package name, even when the manifest renames it.
    pub name: String,
    pub kind: DepKind,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DepKind {
    Normal,
    Build,
    Dev,
}

impl Workspace {
    /// Reads the workspace whose root manifest is `manifest`. `--no-deps` keeps it offline.
    pub fn load(manifest: &Path) -> Result<Self, String> {
        let cargo = std::env::var("CARGO").unwrap_or_else(|_| "cargo".into());
        let out = Command::new(cargo)
            .args([
                "metadata",
                "--format-version",
                "1",
                "--no-deps",
                "--manifest-path",
            ])
            .arg(manifest)
            .output()
            .map_err(|e| format!("cargo metadata didn't run: {e}"))?;
        if !out.status.success() {
            return Err(format!(
                "cargo metadata failed:\n{}",
                String::from_utf8_lossy(&out.stderr)
            ));
        }
        let metadata: Value = serde_json::from_slice(&out.stdout)
            .map_err(|e| format!("cargo metadata printed bad JSON: {e}"))?;
        let packages = metadata["packages"]
            .as_array()
            .ok_or("cargo metadata lists no packages")?;
        let crates = packages
            .iter()
            .map(Crate::from_metadata)
            .collect::<Result<_, _>>()?;
        Ok(Workspace { crates })
    }

    pub fn has(&self, name: &str) -> bool {
        self.crates.iter().any(|c| c.name == name)
    }
}

impl Crate {
    fn from_metadata(package: &Value) -> Result<Self, String> {
        let name = package["name"]
            .as_str()
            .ok_or("a package without a name")?
            .to_owned();
        let manifest = package["manifest_path"]
            .as_str()
            .ok_or("a package without a manifest")?;
        let dir = Path::new(manifest).parent().unwrap().to_owned();
        let deps = package["dependencies"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|dep| Dep {
                name: dep["name"].as_str().unwrap_or_default().to_owned(),
                kind: match dep["kind"].as_str() {
                    Some("dev") => DepKind::Dev,
                    Some("build") => DepKind::Build,
                    _ => DepKind::Normal,
                },
            })
            .collect();
        Ok(Crate { name, dir, deps })
    }
}
