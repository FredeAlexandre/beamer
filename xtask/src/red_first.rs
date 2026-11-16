//! `cargo xtask red-first`: every test a branch adds to a crate's `tests/` must fail against the
//! blind export by panicking on a `todo!()` (#32, #15, ADR 0002).
//!
//! A test that passes there tests nothing the implementation does, a test that doesn't compile
//! there needs an API the skeleton lacks, and a test that fails without reaching a `todo!()`
//! fails on code the export shares with the real crate, so it fails on the branch too.
//!
//! New tests are the `#[test]` functions in `crates/*/tests/` that the checkout (committed or
//! not) adds or changes since its merge base with `--base`. Changed counts as new: an edited test
//! must be red again.

use std::collections::BTreeMap;
use std::fs;
use std::ops::Range;
use std::path::{Path, PathBuf};
use std::process::Command;

use proc_macro2::{Delimiter, TokenStream, TokenTree};

use crate::{Result, blind, workspace};

/// The message `todo!()` panics with.
const TODO_PANIC: &str = "not yet implemented";

pub fn run(base: &str) -> Result {
    let root = workspace::root()?;
    let merge_base = workspace::git(&root, &["merge-base", base, "HEAD"])?;
    let new = new_tests(&root, merge_base.trim())?;
    if new.is_empty() {
        println!("No new tests in crates/*/tests since {base}.");
        return Ok(());
    }

    let scratch = target_dir(&root).join("red-first");
    let export = scratch.join("export");
    if export.exists() {
        fs::remove_dir_all(&export)
            .map_err(|e| format!("cannot clear {}: {e}", export.display()))?;
    }
    blind::export(&root, &export)?;

    let mut failed = 0;
    for (target, tests) in group_by_target(new) {
        for (test, verdict) in target.run(&export, &scratch.join("target"), &tests)? {
            match verdict {
                Verdict::Red => println!("red   {}  {}", test.file, test.name),
                Verdict::Fail(why) => {
                    failed += 1;
                    println!("FAIL  {}  {}: {why}", test.file, test.name);
                }
            }
        }
    }
    match failed {
        0 => Ok(()),
        n => Err(format!(
            "{n} new test(s) are not red against the blind export. Each must panic on a todo!()."
        )),
    }
}

/// The build output directory: `CARGO_TARGET_DIR`, or the workspace's `target/`.
fn target_dir(root: &Path) -> PathBuf {
    std::env::var_os("CARGO_TARGET_DIR").map_or_else(|| root.join("target"), |dir| root.join(dir))
}

/// One new test: the file it is in and its path inside its test binary.
#[derive(Debug)]
struct Test {
    file: String,
    name: String,
}

enum Verdict {
    Red,
    Fail(String),
}

/// A test binary: a crate's `tests/<name>.rs` or `tests/<name>/main.rs`.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
struct Target {
    /// The crate's directory, like `crates/beamer-link`.
    package: String,
    /// The binary's name; `None` for a file under `tests/` that no binary is built from.
    binary: Option<String>,
}

fn group_by_target(tests: Vec<(Target, Test)>) -> BTreeMap<Target, Vec<Test>> {
    let mut groups: BTreeMap<Target, Vec<Test>> = BTreeMap::new();
    for (target, test) in tests {
        groups.entry(target).or_default().push(test);
    }
    groups
}

impl Target {
    /// Runs `tests` in this binary of the export, and judges each.
    fn run<'t>(
        &self,
        export: &Path,
        target_dir: &Path,
        tests: &'t [Test],
    ) -> Result<Vec<(&'t Test, Verdict)>> {
        let Some(binary) = &self.binary else {
            let why = "its file is in tests/ but in no test binary (tests/<name>.rs or tests/<name>/main.rs)";
            return Ok(tests
                .iter()
                .map(|test| (test, Verdict::Fail(why.into())))
                .collect());
        };
        let manifest = export.join(&self.package).join("Cargo.toml");
        let out = Command::new(std::env::var_os("CARGO").unwrap_or("cargo".into()))
            .arg("test")
            .arg("--manifest-path")
            .arg(&manifest)
            .args(["--test", binary, "--", "--include-ignored", "--exact"])
            .args(tests.iter().map(|test| &test.name))
            .env("CARGO_TARGET_DIR", target_dir)
            // The export allows the lints its todo!() bodies set off in its own config, which
            // these would override.
            .env_remove("RUSTFLAGS")
            .env_remove("CARGO_ENCODED_RUSTFLAGS")
            .env_remove("CARGO_BUILD_RUSTFLAGS")
            .output()
            .map_err(|e| format!("cannot run cargo test: {e}"))?;
        let stdout = String::from_utf8_lossy(&out.stdout);
        let results = parse_libtest(&stdout);
        if results.is_empty() && !out.status.success() {
            // Nothing ran: the binary didn't build. Show why.
            eprint!("{}", String::from_utf8_lossy(&out.stderr));
            let why = format!("does not compile against the blind export (tests/{binary})");
            return Ok(tests
                .iter()
                .map(|test| (test, Verdict::Fail(why.clone())))
                .collect());
        }
        Ok(tests
            .iter()
            .map(|test| {
                let verdict = match results.get(test.name.as_str()) {
                    None => Verdict::Fail("did not run".into()),
                    Some(None) => Verdict::Fail("passed against the blind export".into()),
                    Some(Some(output)) if output.contains(TODO_PANIC) => Verdict::Red,
                    Some(Some(_)) => Verdict::Fail(
                        "failed without reaching a todo!(), so it fails on the real code too"
                            .into(),
                    ),
                };
                (test, verdict)
            })
            .collect())
    }
}

/// Each test libtest reported: `None` if it passed, else `Some` of its captured output.
fn parse_libtest(stdout: &str) -> BTreeMap<String, Option<String>> {
    let mut results = BTreeMap::new();
    for line in stdout.lines() {
        let Some(rest) = line.strip_prefix("test ") else {
            continue;
        };
        // libtest marks #[should_panic] tests: `test name - should panic ... ok`.
        let rest = rest.replacen(" - should panic ... ", " ... ", 1);
        if let Some(name) = rest.strip_suffix(" ... ok") {
            results.insert(name.to_owned(), None);
        } else if let Some(name) = rest.strip_suffix(" ... FAILED") {
            let header = format!("---- {name} stdout ----");
            let output = stdout
                .split_once(&header)
                .map(|(_, after)| {
                    after
                        .lines()
                        .take_while(|line| !line.starts_with("---- ") && *line != "failures:")
                        .collect::<Vec<_>>()
                        .join("\n")
                })
                .unwrap_or_default();
            results.insert(name.to_owned(), Some(output));
        }
    }
    results
}

/// The tests the checkout adds or changes in `crates/*/tests/` since `merge_base`.
fn new_tests(root: &Path, merge_base: &str) -> Result<Vec<(Target, Test)>> {
    let changed = workspace::git(
        root,
        &[
            "diff",
            "-z",
            "--name-only",
            "--no-renames",
            "--diff-filter=AM",
            merge_base,
        ],
    )?;
    let untracked = workspace::git(root, &["ls-files", "-z", "--others", "--exclude-standard"])?;
    let mut files: Vec<&str> = changed.split('\0').chain(untracked.split('\0')).collect();
    files.sort();
    files.dedup();

    let mut new = Vec::new();
    for file in files {
        let Some(target) = target_of(root, file) else {
            continue;
        };
        let source =
            fs::read_to_string(root.join(file)).map_err(|e| format!("cannot read {file}: {e}"))?;
        let old =
            workspace::git(root, &["show", &format!("{merge_base}:{file}")]).unwrap_or_default();
        let old = tests_in(&old).unwrap_or_default();
        for (name, text) in tests_in(&source).map_err(|e| format!("{file}: {e}"))? {
            if old.get(&name) != Some(&text) {
                let name = format!("{}{name}", target_prefix(file));
                new.push((
                    target.clone(),
                    Test {
                        file: file.to_owned(),
                        name,
                    },
                ));
            }
        }
    }
    Ok(new)
}

/// The test binary a Rust file under `crates/<crate>/tests/` is compiled into.
fn target_of(root: &Path, file: &str) -> Option<Target> {
    let parts: Vec<&str> = file.split('/').collect();
    let ["crates", krate, "tests", rest @ ..] = parts.as_slice() else {
        return None;
    };
    if !file.ends_with(".rs") {
        return None;
    }
    let package = format!("crates/{krate}");
    let binary = match rest {
        [single] => Some(single.trim_end_matches(".rs").to_owned()),
        [dir, ..]
            if root
                .join(&package)
                .join("tests")
                .join(dir)
                .join("main.rs")
                .is_file() =>
        {
            Some((*dir).to_owned())
        }
        _ => None,
    };
    Some(Target { package, binary })
}

/// The module path a file under `tests/<binary>/` adds in front of its tests: `tests/x/a/b.rs`
/// and `tests/x/a/b/mod.rs` hold `a::b::`, assuming the usual `mod` declarations.
fn target_prefix(file: &str) -> String {
    let parts: Vec<&str> = file.split('/').collect();
    let rest = &parts[3..];
    if rest.len() < 2 {
        return String::new();
    }
    rest[1..]
        .iter()
        .map(|part| part.trim_end_matches(".rs"))
        .filter(|part| *part != "main" && *part != "mod")
        .map(|part| format!("{part}::"))
        .collect()
}

/// Every test function in a file, by its path in the file (`module::name`), with its source text
/// so a changed test can be told from an unchanged one. Finds `#[test]` (and `#[tokio::test]`
/// and the like) on functions, in inline modules, and inside item macros such as `proptest!`.
fn tests_in(source: &str) -> Result<BTreeMap<String, String>> {
    let file: syn::File = syn::parse_str(source).map_err(|e| format!("does not parse: {e}"))?;
    let mut tests = BTreeMap::new();
    collect(source, &file.items, "", &mut tests);
    Ok(tests)
}

fn collect(source: &str, items: &[syn::Item], prefix: &str, tests: &mut BTreeMap<String, String>) {
    use syn::spanned::Spanned;
    for item in items {
        match item {
            syn::Item::Fn(f) if f.attrs.iter().any(|attr| is_test_attr(attr.path())) => {
                tests.insert(
                    format!("{prefix}{}", f.sig.ident),
                    source[item.span().byte_range()].to_owned(),
                );
            }
            syn::Item::Mod(m) => {
                if let Some((_, items)) = &m.content {
                    collect(source, items, &format!("{prefix}{}::", m.ident), tests);
                }
            }
            syn::Item::Macro(m) => {
                for (name, range) in tests_in_tokens(m.mac.tokens.clone()) {
                    tests.insert(format!("{prefix}{name}"), source[range].to_owned());
                }
            }
            _ => {}
        }
    }
}

fn is_test_attr(path: &syn::Path) -> bool {
    path.segments
        .last()
        .is_some_and(|segment| segment.ident == "test")
}

/// `#[test] fn name … { … }` sequences in a macro's tokens: each name with the byte range from
/// the `#` to the end of the body.
fn tests_in_tokens(tokens: TokenStream) -> Vec<(String, Range<usize>)> {
    let tokens: Vec<TokenTree> = tokens.into_iter().collect();
    let mut found = Vec::new();
    let mut start = None; // where the pending #[test] began
    let mut i = 0;
    while i < tokens.len() {
        match (&tokens[i], tokens.get(i + 1)) {
            (TokenTree::Punct(hash), Some(TokenTree::Group(attr)))
                if hash.as_char() == '#' && attr.delimiter() == Delimiter::Bracket =>
            {
                if syn::parse2::<syn::Path>(attr.stream()).is_ok_and(|path| is_test_attr(&path)) {
                    start.get_or_insert(hash.span().byte_range().start);
                }
                i += 2;
                continue;
            }
            (TokenTree::Ident(kw), Some(TokenTree::Ident(name))) if kw == "fn" => {
                if let Some(begin) = start.take() {
                    let body = tokens[i + 2..].iter().find_map(|token| match token {
                        TokenTree::Group(g) if g.delimiter() == Delimiter::Brace => Some(g.span()),
                        _ => None,
                    });
                    let end =
                        body.map_or(name.span().byte_range().end, |span| span.byte_range().end);
                    found.push((name.to_string(), begin..end));
                }
            }
            (TokenTree::Group(group), _) if group.delimiter() == Delimiter::Brace => {
                start = None; // a body ended without a fn: the attribute wasn't ours
            }
            _ => {}
        }
        i += 1;
    }
    found
}
