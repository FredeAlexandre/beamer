//! The Python tests at a git ref: every `def test_*` (or `async def test_*`) in a `.py` file, at
//! module level or in a class, as `<path>::<Class>::…::<name>`, the way pytest names them.
//!
//! The files are read from git, so no checkout of the ref is needed. They are scanned by
//! indentation, line by line: strings, comments and bracketed continuation lines are skipped, so
//! a `def` inside a docstring or a closing bracket at column 0 changes nothing. A `def` nested in a
//! function isn't a test.

use std::collections::BTreeSet;
use std::io::{Read, Write};
use std::path::Path;
use std::process::{Command, Stdio};

/// Every Python test id at `reference`, or why they can't be read.
pub fn tests(root: &Path, reference: &str) -> Result<BTreeSet<String>, String> {
    let git = |args: &[&str]| {
        Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .output()
            .map_err(|e| format!("git doesn't run: {e}"))
    };
    let resolved = git(&[
        "rev-parse",
        "--verify",
        "--quiet",
        &format!("{reference}^{{commit}}"),
    ])?;
    if !resolved.status.success() {
        return Err(format!(
            "`{reference}` is not in this clone, so the ledger can't be checked against the \
             Python tests: fetch it with `git fetch origin tag {reference}`, or name another ref \
             with --python-ref"
        ));
    }
    let listing = git(&["ls-tree", "-r", "-z", "--name-only", reference])?;
    let paths: Vec<String> = String::from_utf8_lossy(&listing.stdout)
        .split('\0')
        .filter(|p| p.ends_with(".py"))
        .map(str::to_owned)
        .collect();

    // One `git cat-file --batch` reads every file.
    let mut child = Command::new("git")
        .arg("-C")
        .arg(root)
        .args(["cat-file", "--batch"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("git doesn't run: {e}"))?;
    let mut stdin = child.stdin.take().unwrap();
    let request: String = paths.iter().map(|p| format!("{reference}:{p}\n")).collect();
    let writer = std::thread::spawn(move || stdin.write_all(request.as_bytes()));
    let mut output = Vec::new();
    child
        .stdout
        .take()
        .unwrap()
        .read_to_end(&mut output)
        .map_err(|e| e.to_string())?;
    let _ = writer.join();
    let _ = child.wait();

    let mut tests = BTreeSet::new();
    let mut rest = &output[..];
    for path in &paths {
        // Each object comes as `<sha> blob <size>\n<content>\n`.
        let header_end = rest
            .iter()
            .position(|&b| b == b'\n')
            .ok_or("git cat-file stopped early")?;
        let (header, after) = rest.split_at(header_end);
        let header = String::from_utf8_lossy(header);
        let size: usize = header
            .split_whitespace()
            .nth(2)
            .and_then(|s| s.parse().ok())
            .ok_or_else(|| format!("git cat-file can't read {path}: {header}"))?;
        let (content, after) = after[1..].split_at(size);
        rest = &after[1..];
        tests.extend(test_ids(path, &String::from_utf8_lossy(content)));
    }
    Ok(tests)
}

/// The test ids in one file.
fn test_ids(path: &str, source: &str) -> Vec<String> {
    let mut ids = Vec::new();
    // The open `def`s and `class`es: indentation, whether a class, name.
    let mut scopes: Vec<(usize, bool, String)> = Vec::new();
    let mut lexer = Lexer::default();
    for line in source.lines() {
        if lexer.at_statement_start() {
            let code = line.trim_start();
            if !code.is_empty() && !code.starts_with('#') {
                let indent = line.len() - code.len();
                while scopes.last().is_some_and(|s| s.0 >= indent) {
                    scopes.pop();
                }
                if let Some((is_class, name)) = definition(code) {
                    if !is_class && name.starts_with("test_") && scopes.iter().all(|s| s.1) {
                        let mut id = path.to_owned();
                        for (_, _, class) in &scopes {
                            id += "::";
                            id += class;
                        }
                        id += "::";
                        id += name;
                        ids.push(id);
                    }
                    scopes.push((indent, is_class, name.to_owned()));
                }
            }
        }
        lexer.line(line);
    }
    ids
}

/// `code` after the keyword `word` and the blanks that follow it.
fn word<'c>(code: &'c str, word: &str) -> Option<&'c str> {
    code.strip_prefix(word)
        .filter(|r| r.starts_with([' ', '\t']))
        .map(str::trim_start)
}

/// `def name`, `async def name` or `class name`: whether a class, and the name.
fn definition(code: &str) -> Option<(bool, &str)> {
    let (is_class, rest) = if let Some(rest) = word(code, "class") {
        (true, rest)
    } else {
        let rest = word(code, "async").unwrap_or(code);
        (false, word(rest, "def")?)
    };
    let end = rest
        .find(|c: char| !(c.is_alphanumeric() || c == '_'))
        .unwrap_or(rest.len());
    Some((is_class, &rest[..end]))
}

/// Where a line leaves the lexer: in a triple-quoted string, in brackets, or continued by `\`.
#[derive(Default)]
struct Lexer {
    triple: Option<char>,
    depth: usize,
    continued: bool,
}

impl Lexer {
    fn at_statement_start(&self) -> bool {
        self.triple.is_none() && self.depth == 0 && !self.continued
    }

    fn line(&mut self, line: &str) {
        let chars: Vec<char> = line.chars().collect();
        let mut i = 0;
        self.continued = false;
        while i < chars.len() {
            let c = chars[i];
            if let Some(quote) = self.triple {
                if c == '\\' {
                    i += 2;
                    continue;
                }
                if c == quote
                    && chars.get(i + 1) == Some(&quote)
                    && chars.get(i + 2) == Some(&quote)
                {
                    self.triple = None;
                    i += 3;
                    continue;
                }
                i += 1;
                continue;
            }
            match c {
                '#' => return,
                // Outside a string, a backslash only ever continues the line.
                '\\' => {
                    self.continued = true;
                    return;
                }
                '"' | '\'' => {
                    if chars.get(i + 1) == Some(&c) && chars.get(i + 2) == Some(&c) {
                        self.triple = Some(c);
                        i += 3;
                        continue;
                    }
                    // A one-line string: up to its closing quote or the end of the line.
                    i += 1;
                    while i < chars.len() && chars[i] != c {
                        i += if chars[i] == '\\' { 2 } else { 1 };
                    }
                }
                '(' | '[' | '{' => self.depth += 1,
                ')' | ']' | '}' => self.depth = self.depth.saturating_sub(1),
                _ => {}
            }
            i += 1;
        }
    }
}
