//! Fixture workspaces for the `deps` tests: a throwaway Cargo workspace under the test's temporary
//! directory, built crate by crate, that the real `cargo xtask deps` (or `cargo clippy`) then runs on.

#![allow(dead_code)] // Each test crate uses a different part of this module.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

/// The lint list every sans-I/O crate carries as its `clippy.toml`.
pub const SANS_IO_CLIPPY: &str = include_str!("../../sans-io.clippy.toml");

pub struct Fixture {
    pub root: PathBuf,
}

impl Fixture {
    /// An empty workspace named after the test, wiped first so a rerun starts clean.
    pub fn new(name: &str) -> Self {
        let root = Path::new(env!("CARGO_TARGET_TMPDIR"))
            .join("deps")
            .join(name);
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("crates")).unwrap();
        fs::write(
            root.join("Cargo.toml"),
            "[workspace]\nresolver = \"3\"\nmembers = [\"crates/*\"]\n",
        )
        .unwrap();
        Fixture { root }
    }

    /// A crate whose manifest ends with `manifest_tail` (dependency tables, raw TOML), with an
    /// empty `src/lib.rs`.
    pub fn krate(&self, name: &str, manifest_tail: &str) -> &Self {
        let manifest = format!(
            "[package]\nname = \"{name}\"\nversion = \"0.0.0\"\nedition = \"2024\"\n\n{manifest_tail}\n"
        );
        self.file(&format!("crates/{name}/Cargo.toml"), &manifest);
        self.file(&format!("crates/{name}/src/lib.rs"), "");
        self
    }

    /// A sans-I/O crate: [`Fixture::krate`] plus the canonical `clippy.toml`.
    pub fn sans_io(&self, name: &str, manifest_tail: &str) -> &Self {
        self.krate(name, manifest_tail);
        self.file(&format!("crates/{name}/clippy.toml"), SANS_IO_CLIPPY);
        self
    }

    /// Writes `content` to `path`, relative to the workspace root.
    pub fn file(&self, path: &str, content: &str) -> &Self {
        let path = self.root.join(path);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, content).unwrap();
        self
    }

    /// `cargo xtask deps` on this workspace.
    pub fn deps(&self) -> Run {
        Run::of(
            Command::new(env!("CARGO_BIN_EXE_xtask"))
                .arg("deps")
                .arg("--manifest-path")
                .arg(self.root.join("Cargo.toml")),
        )
    }

    /// `cargo clippy -- -D warnings` on this workspace.
    pub fn clippy(&self) -> Run {
        Run::of(
            Command::new(std::env::var("CARGO").unwrap_or_else(|_| "cargo".into()))
                .args(["clippy", "--quiet", "--manifest-path"])
                .arg(self.root.join("Cargo.toml"))
                .arg("--target-dir")
                .arg(self.root.join("target"))
                .args(["--", "-D", "warnings"]),
        )
    }
}

/// A finished run, with stdout and stderr together, as a developer reads them.
pub struct Run {
    pub ok: bool,
    pub text: String,
}

impl Run {
    pub fn of(command: &mut Command) -> Self {
        let Output {
            status,
            stdout,
            stderr,
        } = command.output().expect("the command runs");
        let text =
            String::from_utf8_lossy(&stdout).into_owned() + &String::from_utf8_lossy(&stderr);
        Run {
            ok: status.success(),
            text,
        }
    }

    /// Asserts the run passed.
    pub fn passes(&self) {
        assert!(self.ok, "expected a pass, got:\n{}", self.text);
    }

    /// Asserts the run failed and its output names every one of `needles`.
    pub fn fails_naming(&self, needles: &[&str]) {
        assert!(!self.ok, "expected a failure, got a pass:\n{}", self.text);
        for needle in needles {
            assert!(
                self.text.contains(needle),
                "output doesn't name `{needle}`:\n{}",
                self.text
            );
        }
    }
}
