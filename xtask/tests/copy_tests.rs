//! `cargo xtask copy-tests <export>`: a test agent's tests, from the blind export back onto the
//! real task branch, in one command (#32).

mod fixture;

use fixture::{Fixture, assert_ok, read, write};

const LIB: &str = "pub fn add(a: u32, b: u32) -> u32 {\n    a + b\n}\n";
const MANIFEST: &str = "crates/beamer-demo/Cargo.toml";
const TEST: &str = "#[test]\nfn adds() {\n    assert_eq!(beamer_demo::add(2, 3), 5);\n}\n";

/// The fixture and its fresh blind export.
fn exported() -> (Fixture, std::path::PathBuf) {
    let ws = Fixture::new(&[("crates/beamer-demo/src/lib.rs", LIB)]);
    let export = ws.path("export");
    assert_ok(&ws.xtask(&["blind", export.to_str().unwrap()]));
    (ws, export)
}

#[test]
fn new_tests_and_manifest_changes_come_back_and_nothing_else_does() {
    let (ws, export) = exported();
    let manifest = format!("{}\n[dev-dependencies]\n", read(&export.join(MANIFEST)));
    write(&export.join("crates/beamer-demo/tests/add.rs"), TEST);
    write(
        &export.join("crates/beamer-demo/tests/common/mod.rs"),
        "pub fn helper() {}\n",
    );
    write(&export.join(MANIFEST), &manifest);
    write(
        &export.join("crates/beamer-demo/src/lib.rs"),
        "pub fn add() {}\n",
    );
    write(
        &export.join("spec/demo.md"),
        "rewritten by the test agent\n",
    );

    assert_ok(&ws.xtask(&["copy-tests", export.to_str().unwrap()]));

    assert_eq!(ws.read("crates/beamer-demo/tests/add.rs"), TEST);
    assert_eq!(
        ws.read("crates/beamer-demo/tests/common/mod.rs"),
        "pub fn helper() {}\n"
    );
    assert_eq!(ws.read(MANIFEST), manifest);
    assert_eq!(
        ws.read("crates/beamer-demo/src/lib.rs"),
        LIB,
        "the blind source came back"
    );
    assert_eq!(ws.read("spec/demo.md"), fixture::SPEC, "the spec came back");
}

#[test]
fn a_file_changed_on_both_sides_is_refused_and_nothing_is_copied() {
    let (ws, export) = exported();
    write(&export.join("crates/beamer-demo/tests/add.rs"), TEST);
    write(
        &export.join(MANIFEST),
        &format!("{}\n# the agent's\n", read(&export.join(MANIFEST))),
    );
    ws.write(
        MANIFEST,
        &format!("{}\n# the branch's\n", ws.read(MANIFEST)),
    );

    let out = ws.xtask(&["copy-tests", export.to_str().unwrap()]);

    assert!(
        !out.status.success(),
        "copy-tests overwrote a change made on the branch"
    );
    assert!(
        fixture::stderr(&out).contains(MANIFEST),
        "{}",
        fixture::stderr(&out)
    );
    assert!(ws.read(MANIFEST).contains("# the branch's"));
    assert!(
        !ws.path("crates/beamer-demo/tests/add.rs").exists(),
        "copied despite the conflict"
    );
}
