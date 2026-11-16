//! The Python ledger, `spec/ledger/<domain>.txt`: every Python test at `archive/python`, owned by
//! the domain whose file holds it, with its fate. The format is in `spec/README.md`.

use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::path::PathBuf;

use super::{Options, Problem, python};
use crate::spec::is_kebab;

/// The four fates, as the ledger writes them.
const FATES: [&str; 4] = ["mined", "dropped", "python-detail", "wire-v6"];

/// Checks the ledger. Per PR it may be partial; `--complete` requires every Python test once, each
/// with a fate.
pub fn check(options: &Options, rules: &HashMap<String, u32>, problems: &mut Vec<Problem>) {
    let reference = options.python_ref;
    let files = ledger_files(options);
    if files.is_empty() && !options.complete {
        println!(
            "test-audit: no ledger yet in spec/ledger/, so nothing to check against {reference}"
        );
        return;
    }
    let python = match python::tests(options.root, reference) {
        Ok(tests) => tests,
        Err(message) => {
            problems.push(Problem::general(message));
            return;
        }
    };

    // Each test met so far, and where.
    let mut seen: HashMap<String, (String, usize)> = HashMap::new();
    let mut fated = 0;
    for path in files {
        let domain = path
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        let file = format!("spec/ledger/{domain}.txt");
        if !is_kebab(&domain) {
            problems.push(Problem::general(format!(
                "{file}: a ledger file is named for the domain that owns its tests, in \
                 kebab-case: spec/ledger/<domain>.txt"
            )));
        }
        let text = fs::read_to_string(&path).unwrap_or_default();
        for (n, line) in text.lines().enumerate() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let at = |message: String| Problem::at(&file, n + 1, message);
            let (id, fate) = match line.split_once(char::is_whitespace) {
                Some((id, fate)) => (id, fate.trim()),
                None => (line, ""),
            };
            if !python.contains(id) {
                problems.push(at(format!(
                    "`{id}` is not a Python test at {reference}: a test is \
                     `<path>::<Class>::<test_name>`, or `<path>::<test_name>` at module level"
                )));
            }
            match seen.get(id) {
                Some((first_file, first_line)) => problems.push(at(format!(
                    "`{id}` is already in the ledger at {first_file}:{first_line}: each Python \
                     test has one owner domain and one fate"
                ))),
                None => {
                    seen.insert(id.to_owned(), (file.clone(), n + 1));
                }
            }
            if fate.is_empty() {
                if options.complete {
                    problems.push(at(format!(
                        "`{id}` has no fate yet: give it one of {}",
                        FATES.join(", ")
                    )));
                }
                continue;
            }
            fated += 1;
            if let Err(message) = check_fate(fate, rules) {
                problems.push(at(format!("`{id}`: {message}")));
            }
        }
    }

    let missing: BTreeSet<&String> = python.iter().filter(|t| !seen.contains_key(*t)).collect();
    if options.complete && !missing.is_empty() {
        let list: Vec<String> = missing.iter().map(|t| format!("  {t}")).collect();
        problems.push(Problem::general(format!(
            "{} Python test{} at {reference} missing from the ledger:\n{}",
            missing.len(),
            if missing.len() == 1 { " is" } else { "s are" },
            list.join("\n")
        )));
    }
    println!(
        "test-audit: the ledger holds {} of the {} Python tests at {reference}, {fated} with a fate",
        seen.len(),
        python.len()
    );
}

/// The ledger's files, `spec/ledger/*.txt`, in name order.
fn ledger_files(options: &Options) -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = fs::read_dir(options.root.join("spec/ledger"))
        .map(|entries| {
            entries
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|e| e == "txt"))
                .collect()
        })
        .unwrap_or_default();
    files.sort();
    files
}

/// `mined: <rule id>, …`, or `dropped:`, `python-detail:` or `wire-v6:` with a reason.
fn check_fate(fate: &str, rules: &HashMap<String, u32>) -> Result<(), String> {
    let (key, value) = fate.split_once(':').unwrap_or((fate, ""));
    let (key, value) = (key.trim(), value.trim());
    match key {
        "mined" => {
            let ids: Vec<&str> = value
                .split(',')
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .collect();
            if ids.is_empty() {
                return Err("a mined test names the rules it was mined into: \
                            `mined: <rule id>, <rule id>`"
                    .into());
            }
            let mut wrong = Vec::new();
            for id in ids {
                if let Some((base, _)) = id.split_once('+') {
                    wrong.push(format!(
                        "`{id}`: the ledger names a rule without its version, `{base}`"
                    ));
                } else if !rules.contains_key(id) {
                    wrong.push(format!("`{id}` is not a rule in spec/"));
                }
            }
            if wrong.is_empty() {
                Ok(())
            } else {
                Err(wrong.join("; "))
            }
        }
        "dropped" | "python-detail" | "wire-v6" if value.is_empty() => {
            Err(format!("`{key}` gives its reason: `{key}: <reason>`"))
        }
        "dropped" | "python-detail" | "wire-v6" => Ok(()),
        other => Err(format!(
            "`{other}` is not a fate: a test is {}",
            FATES.join(", ")
        )),
    }
}
