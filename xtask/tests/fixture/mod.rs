//! A throwaway workspace in a temp dir, committed to its own git repo, and `cargo xtask` run in it
//! from the outside, as a developer runs it.

#![allow(dead_code)] // each test binary uses its own part of this module

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

pub struct Fixture {
    dir: tempfile::TempDir,
}

pub const GLOSSARY: &str = "# Glossary\n\n**Crossing**: the pointer leaving one screen.\n";
pub const SPEC: &str = "# Demo\n\nr[demo.add] Adding two numbers gives their sum.\n";

/// The smallest workspace that looks like Beamer's: crates in `crates/`, a spec, a glossary.
pub const WORKSPACE: &[(&str, &str)] = &[
    (
        "Cargo.toml",
        "[workspace]\nresolver = \"3\"\nmembers = [\"crates/*\"]\n",
    ),
    (".gitignore", "/target/\n"),
    (".cargo/config.toml", "[alias]\nhello = \"version\"\n"),
    ("GLOSSARY.md", GLOSSARY),
    ("spec/demo.md", SPEC),
    (
        "crates/beamer-demo/Cargo.toml",
        "[package]\nname = \"beamer-demo\"\nversion = \"0.0.0\"\nedition = \"2024\"\n",
    ),
];

impl Fixture {
    /// [`WORKSPACE`] plus `files`, committed.
    pub fn new(files: &[(&str, &str)]) -> Self {
        let fixture = Fixture {
            dir: tempfile::tempdir().expect("a temp dir"),
        };
        fixture.git(&["init", "--quiet", "--initial-branch=main"]);
        for (path, text) in WORKSPACE.iter().chain(files) {
            fixture.write(path, text);
        }
        fixture.commit("the skeleton");
        fixture
    }

    pub fn root(&self) -> &Path {
        self.dir.path()
    }

    pub fn path(&self, rel: &str) -> PathBuf {
        self.root().join(rel)
    }

    pub fn write(&self, rel: &str, text: &str) {
        write(&self.path(rel), text);
    }

    pub fn read(&self, rel: &str) -> String {
        read(&self.path(rel))
    }

    pub fn commit(&self, message: &str) {
        self.git(&["add", "--all"]);
        self.git(&["commit", "--quiet", "--allow-empty", "-m", message]);
    }

    pub fn git(&self, args: &[&str]) -> String {
        let out = Command::new("git")
            .args([
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@example.com",
            ])
            .args(["-c", "core.autocrlf=false", "-c", "commit.gpgsign=false"])
            .args(args)
            .current_dir(self.root())
            .output()
            .expect("git runs");
        assert!(out.status.success(), "git {args:?} failed: {out:?}");
        String::from_utf8(out.stdout).unwrap()
    }

    /// `cargo xtask <args>`, run from the fixture's root.
    pub fn xtask(&self, args: &[&str]) -> Output {
        xtask_in(self.root(), args)
    }
}

pub fn xtask_in(dir: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_xtask"))
        .args(args)
        .current_dir(dir)
        .output()
        .expect("the xtask binary runs")
}

/// `cargo <args>` in `dir`, with its own target dir so it never waits on the outer build's lock.
pub fn cargo(dir: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO"))
        .args(args)
        .current_dir(dir)
        .env("CARGO_TARGET_DIR", dir.join("target"))
        .output()
        .expect("cargo runs")
}

pub fn write(path: &Path, text: &str) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, text).unwrap();
}

pub fn read(path: &Path) -> String {
    fs::read_to_string(path).unwrap_or_else(|e| panic!("reading {}: {e}", path.display()))
}

#[track_caller]
pub fn assert_ok(out: &Output) {
    assert!(
        out.status.success(),
        "failed: {}\nstdout:\n{}\nstderr:\n{}",
        out.status,
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
}

pub fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

pub fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}
