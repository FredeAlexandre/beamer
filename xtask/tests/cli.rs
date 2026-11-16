//! `cargo xtask` as a developer runs it: the built binary, from the outside.

use std::process::{Command, Output};

fn xtask(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_xtask"))
        .args(args)
        .output()
        .expect("the xtask binary runs")
}

#[test]
fn help_names_the_tool_and_succeeds() {
    let out = xtask(&["--help"]);
    assert!(out.status.success(), "--help failed: {out:?}");
    let help = String::from_utf8(out.stdout).unwrap();
    assert!(help.contains("Usage: xtask"), "no usage line in:\n{help}");
}

#[test]
fn no_command_prints_usage_and_fails() {
    let out = xtask(&[]);
    assert!(!out.status.success(), "a bare `cargo xtask` should fail");
    let err = String::from_utf8(out.stderr).unwrap();
    assert!(err.contains("Usage: xtask"), "no usage line in:\n{err}");
}

#[test]
fn an_unknown_command_fails() {
    let out = xtask(&["no-such-command"]);
    assert!(!out.status.success());
}
