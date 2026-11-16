//! `cargo xtask mutants`: cargo-mutants as the merge gate (`--in-diff`) and the nightly run it,
//! on the crates a runner mutates (#33, ADR 0002).
//!
//! It runs cargo-mutants with `.cargo/mutants.toml` plus two things the tree decides, written to a
//! derived config under the target dir:
//!
//! - **Red spec tests are left out of the test filter.** A spec test merged red panics at a
//!   `todo!()` of the skeleton (#23). It would fail the baseline, and fail on every mutant too,
//!   killing mutants it never tested.
//! - **`todo!()` bodies are not mutated.** A skeleton's bodies have no tests to kill their
//!   mutants yet; once a body is written, its mutants count.
//! - **The tests of every crate that reaches a mutant run against it** (#135). A rule's spec tests
//!   sit behind the highest seam that reaches it, often `beamer-sim`'s or `beamer-core`'s `tests/`,
//!   not the mutated crate's own; cargo-mutants alone runs only the latter. See [`test_packages`].

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use syn::spanned::Spanned;
use syn::visit::Visit;

use super::crates::{self, Job, Runner, Selection};
use super::nextest;
use crate::deps::workspace::{DepKind, Workspace};
use crate::{Result, workspace};

/// What to mutate, from the command line.
pub struct Options<'a> {
    pub runner: Runner,
    /// Only the code changed since the merge base of HEAD and this rev: the merge gate.
    pub in_diff: Option<&'a str>,
    /// `k/n`: one shard of the mutants, for the nightly.
    pub shard: Option<&'a str>,
    /// More arguments for the test tool, after cargo-mutants' `--`.
    pub args: &'a [String],
}

/// Runs cargo-mutants and returns its exit code: 0 when every mutant is caught, 2 when one is
/// missed, 3 when one timed out (cargo-mutants' own codes).
pub fn run(root: &Path, options: &Options) -> Result<u8> {
    let Selection::Packages(selected) = crates::select(root, options.runner, Job::Mutants)? else {
        unreachable!("mutants are selected crate by crate");
    };
    let workspace = Workspace::load(&root.join("Cargo.toml"))?;
    let dirs: Vec<(String, PathBuf)> = workspace
        .crates
        .iter()
        .filter(|c| selected.contains(&c.name))
        .map(|c| (c.name.clone(), c.dir.clone()))
        .collect();

    let scratch = workspace::target_dir(root).join("xtask-mutants");
    fs::create_dir_all(&scratch)
        .map_err(|e| format!("cannot create {}: {e}", scratch.display()))?;

    let mut diff_file = None;
    let packages: Vec<(String, PathBuf)> = match options.in_diff {
        None => dirs,
        Some(base) => {
            let merge_base = workspace::git(root, &["merge-base", base, "HEAD"])?;
            let merge_base = merge_base.trim();
            let changed = workspace::git(root, &["diff", "--name-only", merge_base])?;
            let changed: Vec<PathBuf> = changed.lines().map(|f| root.join(f)).collect();
            let touched: Vec<_> = dirs
                .into_iter()
                .filter(|(_, dir)| changed.iter().any(|f| f.starts_with(dir)))
                .collect();
            let path = scratch.join("in-diff.diff");
            let diff = workspace::git(root, &["diff", merge_base])?;
            fs::write(&path, diff).map_err(|e| format!("cannot write {}: {e}", path.display()))?;
            diff_file = Some(path);
            touched
        }
    };
    if packages.is_empty() {
        println!(
            "mutants: nothing to mutate; no crate the {:?} runner mutates is changed",
            options.runner
        );
        return Ok(0);
    }
    let package_args: Vec<String> = packages
        .iter()
        .flat_map(|(name, _)| ["-p".to_owned(), name.clone()])
        .collect();
    let mutated: Vec<&str> = packages.iter().map(|(name, _)| name.as_str()).collect();
    let tested = test_packages(
        &workspace,
        &mutated,
        &crates::select(root, options.runner, Job::Test)?,
    );
    println!("mutants: running the spec tests of {}", tested.join(", "));

    let config_path = root.join(".cargo/mutants.toml");
    let mut config: toml::Table = fs::read_to_string(&config_path)
        .map_err(|e| format!("cannot read {}: {e}", config_path.display()))?
        .parse()
        .map_err(|e| format!("{} doesn't parse: {e}", config_path.display()))?;
    let filter = test_filter(&config)?;

    let baseline = baseline(root, &tested, &filter)?;
    for (binary, name) in &baseline.red {
        println!("mutants: leaving out {binary} {name}, red at a todo!()");
    }
    set_test_filter(&mut config, &filter, &baseline.red);
    config.insert(
        "test_package".into(),
        toml::Value::Array(tested.into_iter().map(toml::Value::String).collect()),
    );

    let mut todo_bodies = Vec::new();
    for (_, dir) in &packages {
        for file in rust_files(&dir.join("src")) {
            let rel = file.strip_prefix(root).unwrap_or(&file);
            for (line, column) in todo_bodies_in(&file)? {
                todo_bodies.push(exclude_re(rel, line, column));
            }
        }
    }
    if !todo_bodies.is_empty() {
        println!("mutants: leaving out {} todo!() bodies", todo_bodies.len());
        let excluded = config
            .entry("exclude_re")
            .or_insert_with(|| toml::Value::Array(Vec::new()));
        let toml::Value::Array(excluded) = excluded else {
            return Err("exclude_re in .cargo/mutants.toml is not a list".into());
        };
        excluded.extend(todo_bodies.into_iter().map(toml::Value::String));
    }
    let derived = scratch.join("mutants.toml");
    fs::write(
        &derived,
        toml::to_string(&config).map_err(|e| e.to_string())?,
    )
    .map_err(|e| format!("cannot write {}: {e}", derived.display()))?;

    let mut command = Command::new(cargo());
    command.arg("mutants").arg("--config").arg(&derived);
    // cargo-mutants' own baseline runs only the mutated crates' tests, whatever `test_package`
    // says (27.1.0): the baseline above, over every crate that reaches them, stands in for it.
    command.args([
        "--baseline=skip".to_owned(),
        format!("--timeout={}", test_timeout(baseline.seconds)),
    ]);
    if let Some(diff) = &diff_file {
        command.arg("--in-diff").arg(diff);
    }
    if let Some(shard) = options.shard {
        command.args(["--shard", shard]);
    }
    command.args(&package_args);
    if !options.args.is_empty() {
        command.arg("--").args(options.args);
    }
    let status = command
        .current_dir(root)
        .status()
        .map_err(|e| format!("cannot run cargo mutants: {e}"))?;
    let code = status.code().unwrap_or(1);
    match code {
        0 => println!("mutants: every mutant is caught by tests/"),
        2 => println!("mutants: missed mutants, listed in mutants.out/missed.txt"),
        3 => println!("mutants: timed-out mutants, listed in mutants.out/timeout.txt"),
        _ => {}
    }
    Ok(u8::try_from(code).unwrap_or(1))
}

/// The packages whose tests run against every mutant of the `mutated` crates: each crate, among
/// those the runner `tested_here` tests, whose `tests/` can reach one of them. Sorted.
///
/// A crate's tests reach the crates its dependencies reach, transitively, and its
/// dev-dependencies; a dev-dependency of a dependency is not linked into them. One cargo-mutants
/// run takes one test list, so a mutant of one crate in the run also meets the tests reaching the
/// others: harmless, since a test that can't reach a mutant can't kill it.
///
/// A crate the runner doesn't test isn't run: Linux tests only the OS-independent crates, so a
/// rule of a Linux-mutated crate is caught only by sans-I/O seams (`beamer-sim`, `beamer-core`).
pub fn test_packages(
    workspace: &Workspace,
    mutated: &[&str],
    tested_here: &Selection,
) -> Vec<String> {
    let reaches = |from: &str| -> BTreeSet<String> {
        let mut reached = BTreeSet::new();
        let mut frontier: Vec<String> = vec![from.to_owned()];
        if let Some(krate) = workspace.crates.iter().find(|c| c.name == from) {
            frontier.extend(
                krate
                    .deps
                    .iter()
                    .filter(|d| d.kind == DepKind::Dev)
                    .map(|d| d.name.clone()),
            );
        }
        while let Some(name) = frontier.pop() {
            if !reached.insert(name.clone()) {
                continue;
            }
            if let Some(krate) = workspace.crates.iter().find(|c| c.name == name) {
                frontier.extend(
                    krate
                        .deps
                        .iter()
                        .filter(|d| d.kind == DepKind::Normal)
                        .map(|d| d.name.clone()),
                );
            }
        }
        reached
    };
    let mut tested: Vec<String> = workspace
        .crates
        .iter()
        .filter(|c| match tested_here {
            Selection::Workspace => true,
            Selection::Packages(names) => names.contains(&c.name),
        })
        .filter(|c| {
            let reached = reaches(&c.name);
            mutated.iter().any(|m| reached.contains(*m))
        })
        .map(|c| c.name.clone())
        .collect();
    tested.sort();
    tested
}

fn cargo() -> std::ffi::OsString {
    std::env::var_os("CARGO").unwrap_or("cargo".into())
}

/// The nextest filterset in the config's `additional_cargo_test_args`.
fn test_filter(config: &toml::Table) -> Result<String> {
    let args = config
        .get("additional_cargo_test_args")
        .and_then(toml::Value::as_array)
        .ok_or(".cargo/mutants.toml has no additional_cargo_test_args with the test filter")?;
    let args: Vec<&str> = args.iter().filter_map(toml::Value::as_str).collect();
    args.iter()
        .position(|a| *a == "-E" || *a == "--filterset")
        .and_then(|i| args.get(i + 1))
        .map(|f| (*f).to_owned())
        .ok_or_else(|| {
            ".cargo/mutants.toml's additional_cargo_test_args have no `-E <filterset>`".into()
        })
}

/// Narrows the config's filterset to leave out the `red` tests.
fn set_test_filter(config: &mut toml::Table, filter: &str, red: &BTreeSet<(String, String)>) {
    if red.is_empty() {
        return;
    }
    let left_out = red
        .iter()
        .map(|(binary, name)| format!("(binary_id(={binary}) & test(={name}))"))
        .collect::<Vec<_>>()
        .join(" | ");
    let narrowed = format!("({filter}) & not ({left_out})");
    if let Some(toml::Value::Array(args)) = config.get_mut("additional_cargo_test_args")
        && let Some(i) = args
            .iter()
            .position(|a| matches!(a.as_str(), Some("-E" | "--filterset")))
    {
        args[i + 1] = toml::Value::String(narrowed);
    }
}

/// What the unmutated tree's tests say, run as cargo-mutants would run them.
struct Baseline {
    /// The tests that panic at a `todo!()`, as (binary, name).
    red: BTreeSet<(String, String)>,
    /// How long the tests took to run, in seconds.
    seconds: f64,
}

/// Runs the tests under `filter` in the `tested` packages on the unmutated tree: the baseline.
/// Fails when a test fails other than at a `todo!()`: every mutant would look caught.
fn baseline(root: &Path, tested: &[String], filter: &str) -> Result<Baseline> {
    let report = workspace::target_dir(root).join("nextest/ci/junit.xml");
    let _ = fs::remove_file(&report);
    let status = Command::new(cargo())
        .args(["nextest", "run", "--profile", "ci", "--no-tests=pass"])
        .args(tested.iter().flat_map(|name| ["-p", name]))
        .args(["-E", filter])
        .current_dir(root)
        .status()
        .map_err(|e| format!("cannot run cargo nextest: {e}"))?;
    let xml = match fs::read_to_string(&report) {
        Ok(xml) => xml,
        // No test binary under the filter: nothing ran, and nothing can be red.
        Err(_) if status.success() => {
            return Ok(Baseline {
                red: BTreeSet::new(),
                seconds: 0.0,
            });
        }
        Err(_) => return Err(format!("nextest failed ({status}) before it ran the tests")),
    };
    let mut red = BTreeSet::new();
    let mut failed = Vec::new();
    for test in nextest::failures(&xml)? {
        if test.pending {
            red.insert((test.binary, test.name));
        } else {
            failed.push(format!("{} {}", test.binary, test.name));
        }
    }
    if !failed.is_empty() {
        return Err(format!(
            "tests fail on the unmutated tree, not at a todo!(): {}",
            failed.join(", ")
        ));
    }
    Ok(Baseline {
        red,
        seconds: nextest::run_time(&xml)?,
    })
}

/// The test timeout of each mutant, by cargo-mutants' own rule for a baseline it ran: five
/// times the baseline's test time, and at least 20 seconds.
fn test_timeout(baseline_seconds: f64) -> u64 {
    let scaled = (baseline_seconds * 5.0).ceil();
    if scaled > 20.0 { scaled as u64 } else { 20 }
}

/// Every `.rs` file under `dir`, sorted.
fn rust_files(dir: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    let Ok(entries) = fs::read_dir(dir) else {
        return files;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            files.extend(rust_files(&path));
        } else if path.extension().is_some_and(|e| e == "rs") {
            files.push(path);
        }
    }
    files.sort();
    files
}

/// Where each function whose whole body is one `todo!()` has it: 1-based line and column, as
/// cargo-mutants names the mutants of a body.
fn todo_bodies_in(file: &Path) -> Result<Vec<(usize, usize)>> {
    let text =
        fs::read_to_string(file).map_err(|e| format!("cannot read {}: {e}", file.display()))?;
    let Ok(ast) = syn::parse_file(&text) else {
        // A file that doesn't parse doesn't build either; cargo-mutants will say so.
        return Ok(Vec::new());
    };
    let mut finder = TodoBodies(Vec::new());
    finder.visit_file(&ast);
    Ok(finder.0)
}

struct TodoBodies(Vec<(usize, usize)>);

impl TodoBodies {
    fn body(&mut self, block: &syn::Block) {
        let [stmt] = block.stmts.as_slice() else {
            return;
        };
        let mac = match stmt {
            syn::Stmt::Macro(m) => &m.mac,
            syn::Stmt::Expr(syn::Expr::Macro(m), _) => &m.mac,
            _ => return,
        };
        if mac.path.is_ident("todo") {
            let start = mac.span().start();
            self.0.push((start.line, start.column + 1));
        }
    }
}

impl<'ast> Visit<'ast> for TodoBodies {
    fn visit_item_fn(&mut self, f: &'ast syn::ItemFn) {
        self.body(&f.block);
        syn::visit::visit_item_fn(self, f);
    }

    fn visit_impl_item_fn(&mut self, f: &'ast syn::ImplItemFn) {
        self.body(&f.block);
        syn::visit::visit_impl_item_fn(self, f);
    }

    fn visit_trait_item_fn(&mut self, f: &'ast syn::TraitItemFn) {
        if let Some(block) = &f.default {
            self.body(block);
        }
        syn::visit::visit_trait_item_fn(self, f);
    }
}

/// The `exclude_re` that matches every mutant cargo-mutants names at `file:line:column`, with
/// either path separator.
fn exclude_re(file: &Path, line: usize, column: usize) -> String {
    let parts: Vec<String> = file
        .components()
        .map(|c| regex_escape(&c.as_os_str().to_string_lossy()))
        .collect();
    format!("^{}:{line}:{column}: ", parts.join(r"[/\\]"))
}

fn regex_escape(text: &str) -> String {
    text.chars()
        .flat_map(|c| {
            let special = r"\.+*?()|[]{}^$".contains(c);
            special.then_some('\\').into_iter().chain([c])
        })
        .collect()
}
