//! `cargo xtask blind <dir>`: the workspace as a test agent sees it (#32, ADR 0002).

mod fixture;

use fixture::{Fixture, assert_ok, cargo, read};

const LIB: &str = r#"//! A demo crate.

/// Adds two numbers.
pub fn add(a: u32, b: u32) -> u32 {
    // the secret sauce
    a + b
}

pub struct Counter {
    count: u32,
}

impl Counter {
    pub fn new() -> Self {
        Counter { count: 7 }
    }
}

pub trait Shape {
    fn area(&self) -> f64 {
        42.0
    }
}
"#;

#[test]
fn every_function_body_becomes_todo_and_the_export_compiles() {
    let ws = Fixture::new(&[("crates/beamer-demo/src/lib.rs", LIB)]);
    let export = ws.path("export");

    assert_ok(&ws.xtask(&["blind", export.to_str().unwrap()]));

    let lib = read(&export.join("crates/beamer-demo/src/lib.rs"));
    for secret in ["a + b", "secret sauce", "count: 7", "42.0"] {
        assert!(
            !lib.contains(secret),
            "`{secret}` leaked into the export:\n{lib}"
        );
    }
    assert_eq!(
        lib.matches("todo!()").count(),
        3,
        "three bodies, three todo!():\n{lib}"
    );
    for kept in [
        "/// Adds two numbers.",
        "pub fn add(a: u32, b: u32) -> u32",
        "count: u32",
    ] {
        assert!(
            lib.contains(kept),
            "`{kept}` is missing from the export:\n{lib}"
        );
    }
    assert_ok(&cargo(&export, &["check", "--workspace", "--all-targets"]));
}

#[test]
fn the_export_holds_the_spec_and_glossary_but_no_git_and_no_other_files() {
    let ws = Fixture::new(&[
        ("crates/beamer-demo/src/lib.rs", LIB),
        ("native/driver.c", "int secret(void) { return 1; }\n"),
        ("docs/notes.md", "How add works inside.\n"),
        ("README.md", "# Beamer\n"),
    ]);
    ws.write("spec/new.md", "# Not committed yet, still exported\n");
    ws.write("target/debug/junk", "build output");
    let export = ws.path("export");

    assert_ok(&ws.xtask(&["blind", export.to_str().unwrap()]));

    assert!(!export.join(".git").exists(), "the export has a .git");
    assert_eq!(read(&export.join("GLOSSARY.md")), fixture::GLOSSARY);
    assert_eq!(read(&export.join("spec/demo.md")), fixture::SPEC);
    assert!(
        export.join("spec/new.md").is_file(),
        "untracked spec files are exported"
    );
    for absent in ["native", "docs", "README.md", "target"] {
        assert!(
            !export.join(absent).exists(),
            "{absent} should not be exported"
        );
    }
}

#[test]
fn tests_and_build_scripts_are_copied_as_they_are_and_unit_tests_are_dropped() {
    let test = "use beamer_demo::add;\n\n#[test]\nfn adds() {\n    assert_eq!(add(2, 3), 5);\n}\n";
    let build = "fn main() {\n    println!(\"cargo::rerun-if-changed=build.rs\");\n}\n";
    let lib = "pub fn add(a: u32, b: u32) -> u32 {\n    a + b\n}\n\n\
               #[cfg(test)]\nmod tests {\n    #[test]\n    fn unit_secret() {}\n}\n";
    let ws = Fixture::new(&[
        ("crates/beamer-demo/src/lib.rs", lib),
        ("crates/beamer-demo/build.rs", build),
        ("crates/beamer-demo/tests/add.rs", test),
    ]);
    let export = ws.path("export");

    assert_ok(&ws.xtask(&["blind", export.to_str().unwrap()]));

    let demo = export.join("crates/beamer-demo");
    assert_eq!(read(&demo.join("tests/add.rs")), test);
    assert_eq!(read(&demo.join("build.rs")), build);
    let lib = read(&demo.join("src/lib.rs"));
    assert!(
        !lib.contains("unit_secret"),
        "a #[cfg(test)] item leaked:\n{lib}"
    );
    assert!(!lib.contains("a + b"), "a body leaked:\n{lib}");
}

#[test]
fn a_destination_that_is_not_empty_is_refused() {
    let ws = Fixture::new(&[("crates/beamer-demo/src/lib.rs", LIB)]);
    ws.write("export/keep.txt", "mine");

    let out = ws.xtask(&["blind", ws.path("export").to_str().unwrap()]);

    assert!(
        !out.status.success(),
        "blind wrote into a non-empty directory"
    );
    assert!(
        fixture::stderr(&out).contains("not empty"),
        "{}",
        fixture::stderr(&out)
    );
    assert_eq!(ws.read("export/keep.txt"), "mine");
}

#[test]
fn bodies_todo_cannot_replace_are_kept_with_a_warning_and_blinding_adds_no_warnings() {
    let lib = r#"use std::collections::HashMap;

pub const LIMIT: u32 = double(21);

pub const fn double(n: u32) -> u32 {
    n * 2
}

pub fn evens(n: u32) -> impl Iterator<Item = u32> {
    (0..n).filter(|x| x % 2 == 0)
}

fn helper(map: HashMap<u8, u8>) -> usize {
    map.len()
}

pub fn count(mut n: u32) -> usize {
    n += 1;
    helper(HashMap::from([(n as u8, 0)]))
}
"#;
    let ws = Fixture::new(&[("crates/beamer-demo/src/lib.rs", lib)]);
    let export = ws.path("export");

    let out = ws.xtask(&["blind", export.to_str().unwrap()]);

    assert_ok(&out);
    let warnings = fixture::stderr(&out);
    for name in ["fn double", "fn evens"] {
        assert!(
            warnings.contains(name),
            "no warning for the kept `{name}`:\n{warnings}"
        );
    }
    let check = cargo(&export, &["check", "--workspace", "--all-targets"]);
    assert_ok(&check);
    let compiler = fixture::stderr(&check);
    assert!(
        !compiler.contains("warning"),
        "blinding caused warnings:\n{compiler}"
    );
    let config = read(&export.join(".cargo/config.toml"));
    assert!(
        config.contains("hello"),
        "the workspace's own config is lost:\n{config}"
    );
}
