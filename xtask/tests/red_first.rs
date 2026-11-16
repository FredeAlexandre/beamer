//! `cargo xtask red-first`: every test a branch adds to a crate's `tests/` must panic on a
//! `todo!()` of the blind export, not pass and not fail to compile (#32, #15, ADR 0002).

mod fixture;

use fixture::{Fixture, assert_ok, stdout};

const LIB: &str = "pub fn add(a: u32, b: u32) -> u32 {\n    a + b\n}\n";
const TESTS: &str = "crates/beamer-demo/tests";

/// The fixture with `old` committed on `main`, then a `task` branch checked out.
fn branch(old: &[(&str, &str)]) -> Fixture {
    let mut files = vec![("crates/beamer-demo/src/lib.rs", LIB)];
    files.extend_from_slice(old);
    let ws = Fixture::new(&files);
    ws.git(&["switch", "--quiet", "--create", "task"]);
    ws
}

/// The `red`/`FAIL` line red-first printed for `test`.
fn verdict<'a>(out: &'a str, test: &str) -> &'a str {
    out.lines()
        .find(|line| line.ends_with(&format!(" {test}")) || line.contains(&format!(" {test}:")))
        .unwrap_or_else(|| panic!("no verdict for `{test}` in:\n{out}"))
}

#[test]
fn new_tests_that_panic_on_todo_pass_the_check_and_old_tests_are_not_run() {
    let old = "#[test]\nfn old_passes_even_when_blind() {\n    assert_eq!(1 + 1, 2);\n}\n\n\
               #[test]\nfn edited() {\n    assert_eq!(1, 1);\n}\n";
    let ws = branch(&[("crates/beamer-demo/tests/old.rs", old)]);
    let edited = old.replace("assert_eq!(1, 1)", "assert_eq!(beamer_demo::add(1, 1), 2)");
    ws.write(&format!("{TESTS}/old.rs"), &edited);
    ws.write(
        &format!("{TESTS}/add.rs"),
        "use beamer_demo::add;\n\n\
         macro_rules! cases { ($($item:item)*) => { $($item)* }; }\n\n\
         #[test]\nfn adds() {\n    assert_eq!(add(2, 3), 5);\n}\n\n\
         mod nested {\n    #[test]\n    fn adds_zero() {\n        assert_eq!(beamer_demo::add(0, 3), 3);\n    }\n}\n\n\
         cases! {\n    #[test]\n    fn from_a_macro() {\n        assert_eq!(beamer_demo::add(1, 2), 3);\n    }\n}\n",
    );
    ws.commit("tests for add");

    let out = ws.xtask(&["red-first", "--base", "main"]);

    assert_ok(&out);
    let out = stdout(&out);
    for test in ["adds", "nested::adds_zero", "from_a_macro", "edited"] {
        assert!(
            verdict(&out, test).starts_with("red"),
            "{test} is not red:\n{out}"
        );
    }
    assert!(
        !out.contains("old_passes_even_when_blind"),
        "an old test was run:\n{out}"
    );
}

#[test]
fn a_new_test_that_passes_fails_to_compile_or_fails_without_reaching_todo_fails_the_check() {
    let ws = branch(&[]);
    ws.write(
        &format!("{TESTS}/weak.rs"),
        "#[test]\nfn passes_blind() {\n    assert!(true);\n}\n\n\
         #[test]\n#[should_panic]\nfn any_panic_will_do() {\n    beamer_demo::add(1, 2);\n}\n\n\
         #[test]\nfn wrong() {\n    assert_eq!(1, 2, \"a wrong expectation\");\n}\n\n\
         #[test]\nfn red() {\n    beamer_demo::add(1, 2);\n}\n",
    );
    ws.write(
        &format!("{TESTS}/broken.rs"),
        "#[test]\nfn calls_what_does_not_exist() {\n    beamer_demo::subtract(3, 2);\n}\n",
    );

    let out = ws.xtask(&["red-first", "--base", "main"]);

    assert!(!out.status.success(), "red-first passed:\n{}", stdout(&out));
    let text = stdout(&out);
    assert!(verdict(&text, "passes_blind").contains("passed"), "{text}");
    assert!(
        verdict(&text, "any_panic_will_do").contains("passed"),
        "{text}"
    );
    assert!(
        verdict(&text, "wrong").contains("without reaching a todo!()"),
        "{text}"
    );
    assert!(
        verdict(&text, "calls_what_does_not_exist").contains("does not compile"),
        "{text}"
    );
    assert!(verdict(&text, "red").starts_with("red"), "{text}");
    assert!(
        fixture::stderr(&out).contains("subtract"),
        "no compiler error shown"
    );
}

#[test]
fn a_branch_without_new_tests_passes() {
    let ws = branch(&[]);
    ws.write(
        "crates/beamer-demo/src/lib.rs",
        "pub fn add(a: u32, b: u32) -> u32 {\n    b + a\n}\n",
    );

    let out = ws.xtask(&["red-first", "--base", "main"]);

    assert_ok(&out);
    assert!(stdout(&out).contains("No new tests"), "{}", stdout(&out));
}
