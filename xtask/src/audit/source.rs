//! The crates' Rust source, as the audit reads it: spec tests in `tests/` with the `r[verify]`
//! lines above them and the assertions in them, `r[verify]` lines anywhere else, and
//! `mutants::skip` attributes with their justification.
//!
//! Files are read as token streams, not parsed as items, so tests written inside a macro such as
//! `proptest!` are found too. Comments aren't tokens: `r[verify]` lines and justifications are read
//! from the text, at the lines the tokens give.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use proc_macro2::{Delimiter, TokenStream, TokenTree};

use super::Problem;

/// A rule a test cites: its id and version, as in `r[verify id+n]`.
pub struct Cite {
    pub id: String,
    pub version: u32,
}

/// One spec test in a crate's `tests/`.
pub struct SpecTest {
    /// The file, relative to the repo root.
    pub file: String,
    /// The line of its `fn`.
    pub line: usize,
    pub name: String,
    pub cites: Vec<Cite>,
    /// It asserts, itself or through a helper in the crate's `tests/`, or it is `#[should_panic]`.
    pub asserts: bool,
    /// It is `#[ignore]`d without a reason.
    pub ignored_without_reason: bool,
}

/// Every spec test in `crates/*/tests/`. A file that isn't valid Rust tokens is a problem.
pub fn spec_tests(root: &Path, problems: &mut Vec<Problem>) -> Vec<SpecTest> {
    let mut tests = Vec::new();
    for krate in crates(root) {
        let mut files = Vec::new();
        rust_files(&krate.join("tests"), &mut files);
        let mut functions = Vec::new();
        for path in &files {
            let file = relative(root, path);
            let Ok(text) = fs::read_to_string(path) else {
                problems.push(Problem::at(&file, 0, "can't be read".into()));
                continue;
            };
            match text.parse::<TokenStream>() {
                Ok(tokens) => {
                    let lines: Vec<&str> = text.lines().collect();
                    let mut found = Vec::new();
                    functions_in(tokens, &mut Vec::new(), &mut found);
                    for function in found {
                        functions.push((file.clone(), cites(&lines, &function), function));
                    }
                }
                Err(error) => problems.push(Problem::at(
                    &file,
                    error.span().start().line,
                    format!("isn't valid Rust: {error}"),
                )),
            }
        }
        let asserting = asserting(functions.iter().map(|(_, _, f)| f));
        for (file, cites, function) in functions {
            if !function.is_test {
                continue;
            }
            let asserts = function.should_panic
                || function.asserts
                || function.calls.iter().any(|c| asserting.contains(c));
            tests.push(SpecTest {
                file,
                line: function.line,
                name: function.name,
                cites,
                asserts,
                ignored_without_reason: function.ignored_without_reason,
            });
        }
    }
    tests
}

/// Each `r[verify]` line in a crate outside its `tests/`, as its file and line.
pub fn verify_lines_outside_tests(root: &Path) -> Vec<(String, usize)> {
    let mut found = Vec::new();
    for krate in crates(root) {
        let mut files = Vec::new();
        rust_files(&krate, &mut files);
        let tests = krate.join("tests");
        for path in files.iter().filter(|p| !p.starts_with(&tests)) {
            let text = fs::read_to_string(path).unwrap_or_default();
            for (n, line) in text.lines().enumerate() {
                if line.contains("r[verify ") {
                    found.push((relative(root, path), n + 1));
                }
            }
        }
    }
    found
}

/// Each `mutants::skip` attribute in the crates with no `mutants-justified: <reason>` comment on
/// its line or in the comments and attributes directly above it.
pub fn unjustified_skips(root: &Path) -> Vec<(String, usize)> {
    let mut found = Vec::new();
    for krate in crates(root) {
        let mut files = Vec::new();
        rust_files(&krate, &mut files);
        for path in &files {
            let text = fs::read_to_string(path).unwrap_or_default();
            let lines: Vec<&str> = text.lines().collect();
            for (n, line) in lines.iter().enumerate() {
                let trimmed = line.trim_start();
                if !(trimmed.starts_with('#') && trimmed.contains("mutants::skip")) {
                    continue;
                }
                let above = lines[..n].iter().rev().take_while(|l| {
                    let l = l.trim_start();
                    l.starts_with("//") || l.starts_with('#')
                });
                let justified = std::iter::once(line)
                    .chain(above)
                    .any(|l| justification(l).is_some_and(|reason| !reason.is_empty()));
                if !justified {
                    found.push((relative(root, path), n + 1));
                }
            }
        }
    }
    found
}

fn justification(line: &str) -> Option<&str> {
    let (_, rest) = line.split_once("mutants-justified:")?;
    Some(rest.trim())
}

/// Every crate directory: `crates/*/`.
fn crates(root: &Path) -> Vec<PathBuf> {
    let mut crates: Vec<PathBuf> = fs::read_dir(root.join("crates"))
        .map(|entries| {
            entries
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir())
                .collect()
        })
        .unwrap_or_default();
    crates.sort();
    crates
}

/// Every `.rs` file under `dir`, in name order.
fn rust_files(dir: &Path, files: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    let mut paths: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
    paths.sort();
    for path in paths {
        let name = path.file_name().unwrap_or_default().to_string_lossy();
        if path.is_dir() {
            rust_files(&path, files);
        } else if name.ends_with(".rs") {
            files.push(path);
        }
    }
}

fn relative(root: &Path, path: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/")
}

/// A function found in a file's tokens.
struct Function {
    name: String,
    /// The line of its `fn`.
    line: usize,
    /// The line of its first attribute (a doc comment is one), or of its `fn`.
    first_line: usize,
    is_test: bool,
    should_panic: bool,
    ignored_without_reason: bool,
    /// Its body calls an assertion macro.
    asserts: bool,
    /// The names its body calls, as functions or methods.
    calls: HashSet<String>,
}

/// An outer attribute: the last segment of its path, what follows the path, and its line.
struct Attribute {
    name: String,
    rest: Vec<TokenTree>,
    line: usize,
}

/// Finds every function with a body in `tokens`, nested ones and those inside macro bodies
/// included. `attributes` holds the outer attributes met since the last item.
fn functions_in(tokens: TokenStream, attributes: &mut Vec<Attribute>, found: &mut Vec<Function>) {
    let mut trees = tokens.into_iter();
    while let Some(tree) = trees.next() {
        match tree {
            // An outer attribute. An inner one, `#![…]`, has a `!` first and so falls through to
            // the group arm, as part of the enclosing item.
            TokenTree::Punct(hash) if hash.as_char() == '#' => {
                let mut ahead = trees.clone();
                if let Some(TokenTree::Group(g)) = ahead.next() {
                    attributes.push(attribute(g.stream(), hash.span().start().line));
                    trees = ahead;
                }
            }
            TokenTree::Ident(keyword) if keyword == "fn" => {
                let attrs = std::mem::take(attributes);
                let Some(TokenTree::Ident(name)) = trees.next() else {
                    continue;
                };
                // The body is the first brace group, unless a `;` ends a declaration first.
                let body = trees.by_ref().find_map(|t| match t {
                    TokenTree::Group(g) if g.delimiter() == Delimiter::Brace => {
                        Some(Some(g.stream()))
                    }
                    TokenTree::Punct(p) if p.as_char() == ';' => Some(None),
                    _ => None,
                });
                let Some(Some(body)) = body else {
                    continue;
                };
                let line = keyword.span().start().line;
                let mut asserts = false;
                let mut calls = HashSet::new();
                scan_body(body.clone(), &mut asserts, &mut calls);
                found.push(Function {
                    name: name.to_string(),
                    line,
                    first_line: attrs.first().map_or(line, |a| a.line),
                    is_test: attrs.iter().any(|a| a.name == "test"),
                    should_panic: attrs.iter().any(|a| a.name == "should_panic"),
                    ignored_without_reason: attrs
                        .iter()
                        .any(|a| a.name == "ignore" && !has_reason(&a.rest)),
                    asserts,
                    calls,
                });
                functions_in(body, &mut Vec::new(), found);
            }
            TokenTree::Group(g) => {
                attributes.clear();
                functions_in(g.stream(), &mut Vec::new(), found);
            }
            // Any other punctuation ends the item the attributes were for, as `;` does.
            TokenTree::Punct(_) => attributes.clear(),
            _ => {}
        }
    }
}

fn attribute(tokens: TokenStream, line: usize) -> Attribute {
    let mut trees = tokens.into_iter().peekable();
    let mut name = String::new();
    // The path: idents joined by `::`.
    while let Some(tree) = trees.peek() {
        match tree {
            TokenTree::Ident(ident) => name = ident.to_string(),
            TokenTree::Punct(p) if p.as_char() == ':' => {}
            _ => break,
        }
        trees.next();
    }
    Attribute {
        name,
        rest: trees.collect(),
        line,
    }
}

/// `= "<reason>"`, with a reason that isn't blank.
fn has_reason(rest: &[TokenTree]) -> bool {
    match rest {
        // `= "…"`, or a raw `= r#"…"#`.
        [TokenTree::Punct(_), TokenTree::Literal(reason)] => !reason
            .to_string()
            .trim_start_matches('r')
            .trim_matches(|c| c == '#' || c == '"')
            .trim()
            .is_empty(),
        _ => false,
    }
}

/// Records whether `body` calls an assertion macro, and the names it calls.
fn scan_body(body: TokenStream, asserts: &mut bool, calls: &mut HashSet<String>) {
    let trees: Vec<TokenTree> = body.into_iter().collect();
    for (i, tree) in trees.iter().enumerate() {
        match tree {
            TokenTree::Ident(ident) => match trees.get(i + 1) {
                Some(TokenTree::Punct(bang)) if bang.as_char() == '!' => {
                    if is_assertion(&ident.to_string()) {
                        *asserts = true;
                    }
                }
                Some(TokenTree::Group(g)) if g.delimiter() == Delimiter::Parenthesis => {
                    calls.insert(ident.to_string());
                }
                _ => {}
            },
            TokenTree::Group(g) => scan_body(g.stream(), asserts, calls),
            _ => {}
        }
    }
}

/// `assert!`, `assert_eq!` and the rest of the family, proptest's `prop_assert*!`, and `panic!`.
/// `debug_assert*!` vanishes from release builds, so it doesn't count.
fn is_assertion(name: &str) -> bool {
    name.starts_with("assert") || name.starts_with("prop_assert") || name == "panic"
}

/// The names of the functions that assert, themselves or through a function they call.
fn asserting<'f>(functions: impl Iterator<Item = &'f Function>) -> HashSet<String> {
    let mut calls: HashMap<&str, Vec<&HashSet<String>>> = HashMap::new();
    let mut asserting = HashSet::new();
    for function in functions {
        if function.asserts {
            asserting.insert(function.name.clone());
        }
        calls
            .entry(&function.name)
            .or_default()
            .push(&function.calls);
    }
    loop {
        let more: Vec<String> = calls
            .iter()
            .filter(|(name, _)| !asserting.contains(**name))
            .filter(|(_, sets)| {
                sets.iter()
                    .flat_map(|s| s.iter())
                    .any(|c| asserting.contains(c))
            })
            .map(|(name, _)| (*name).to_owned())
            .collect();
        if more.is_empty() {
            return asserting;
        }
        asserting.extend(more);
    }
}

/// The rules a function's `r[verify]` lines cite: the comment lines directly above its first
/// attribute and those among its attributes.
fn cites(lines: &[&str], function: &Function) -> Vec<Cite> {
    fn is_comment(line: &&&str) -> bool {
        line.trim_start().starts_with("//")
    }
    let first = function.first_line.saturating_sub(1);
    let above = lines[..first.min(lines.len())]
        .iter()
        .rev()
        .take_while(is_comment);
    let among = lines[first.min(lines.len())..function.line.saturating_sub(1).min(lines.len())]
        .iter()
        .filter(is_comment);
    let mut cites = Vec::new();
    for line in above.chain(among) {
        let mut rest = *line;
        while let Some(start) = rest.find("r[verify ") {
            rest = &rest[start + "r[verify ".len()..];
            let Some(end) = rest.find(']') else { break };
            let cited = rest[..end].trim();
            let (id, version) = match cited.split_once('+') {
                Some((id, n)) => (id, n.parse().unwrap_or(0)),
                None => (cited, 1),
            };
            cites.push(Cite {
                id: id.to_owned(),
                version,
            });
            rest = &rest[end..];
        }
    }
    cites
}
