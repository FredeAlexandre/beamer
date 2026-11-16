//! `cargo xtask spec`: the spec format of `spec/README.md`, checked, and tracey run on it.
//!
//! tracey owns rule ids, references and versions. This module checks what tracey doesn't know
//! about: the id's shape and the markers at the end of a rule (tier, parity, model, fuzz).

use std::fmt;
use std::fs;
use std::path::Path;

use crate::tracey;

/// The eight state machines that get a model test (#15), by their `model:` names.
pub const MODELS: [&str; 8] = [
    "driving",
    "link",
    "pairing",
    "handover",
    "store",
    "clipboard",
    "audio",
    "camera",
];

const MARKER_KEYS: [&str; 4] = ["tier", "parity", "model", "fuzz"];

/// Checks the format, then runs `tracey query validate`. Fails if either finds a problem.
pub fn validate(root: &Path) -> bool {
    let (format_ok, has_rules) = match read_spec(root) {
        Ok(rules) => (true, !rules.is_empty()),
        Err(problems) => {
            report(&problems);
            (false, true)
        }
    };
    // tracey runs even after a format problem, so one run shows every problem.
    let tracey_ok = tracey_query(root, "validate", has_rules, |_| true);
    format_ok && tracey_ok
}

/// Runs `tracey query stale`, which exits zero even when it lists stale references, so its answer
/// is read instead: anything but "no stale references" fails.
pub fn stale(root: &Path) -> bool {
    let has_rules = read_spec(root).map_or(true, |rules| !rules.is_empty());
    tracey_query(root, "stale", has_rules, |text| {
        text.contains("no stale references")
    })
}

/// Prints every parity change as a Markdown list: the rule, what Python did, why it changed and
/// what the rule says now.
pub fn parity(root: &Path) -> bool {
    let rules = match read_spec(root) {
        Ok(rules) => rules,
        Err(problems) => {
            report(&problems);
            return false;
        }
    };
    let changes: Vec<&Rule> = rules.iter().filter(|r| r.parity.is_some()).collect();
    if changes.is_empty() {
        println!("No parity changes.");
    }
    for rule in changes {
        let parity = rule.parity.as_ref().unwrap();
        println!("- `{}` ({}:{})", rule.cited_id(), rule.file, rule.line);
        println!("  - Python: {}", parity.python);
        println!("  - Why: {}", parity.why);
        println!("  - Rule: {}", rule.text);
    }
    true
}

/// Runs `tracey query <query>` and prints its answer. `clean` judges the answer's text, on top
/// of tracey's exit code.
///
/// tracey refuses a spec with no rules as a config error, since it reads the `r` prefix from the
/// rules. Until stage 0 writes the first one there is nothing to check, so tracey isn't run.
fn tracey_query(root: &Path, query: &str, has_rules: bool, clean: impl Fn(&str) -> bool) -> bool {
    if !has_rules {
        println!("spec/ holds no rules yet: tracey has nothing to check");
        return true;
    }
    match tracey::query(root, query) {
        Ok(answer) => {
            print!("{}", answer.text);
            // tracey exits zero on a config error, having checked nothing.
            let configured = !answer.text.contains("CONFIG ERROR")
                && !answer.text.contains("No spec/impl combinations configured");
            answer.ok && configured && clean(&answer.text)
        }
        Err(message) => {
            eprintln!("error: {message}");
            false
        }
    }
}

fn report(problems: &[Problem]) {
    for problem in problems {
        eprintln!("{problem}");
    }
    eprintln!(
        "{} spec format problem(s); the format is in spec/README.md",
        problems.len()
    );
}

/// One rule of the spec.
pub struct Rule {
    pub id: String,
    pub version: u32,
    pub file: String,
    pub line: usize,
    /// The rule's text on one line, markers left out.
    pub text: String,
    pub parity: Option<Parity>,
}

impl Rule {
    /// The id as a test cites it: with `+n` from version 2 on.
    pub fn cited_id(&self) -> String {
        match self.version {
            1 => self.id.clone(),
            n => format!("{}+{n}", self.id),
        }
    }
}

pub struct Parity {
    pub python: String,
    pub why: String,
}

struct Problem {
    file: String,
    line: usize,
    rule: Option<String>,
    message: String,
}

impl fmt::Display for Problem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}: ", self.file, self.line)?;
        if let Some(rule) = &self.rule {
            write!(f, "{rule}: ")?;
        }
        f.write_str(&self.message)
    }
}

/// Reads every domain file, `spec/*.md` apart from the README, in name order.
fn read_spec(root: &Path) -> Result<Vec<Rule>, Vec<Problem>> {
    let dir = root.join("spec");
    let mut files: Vec<_> = match fs::read_dir(&dir) {
        Ok(entries) => entries
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().is_some_and(|e| e == "md"))
            .filter(|p| p.file_name().is_some_and(|n| n != "README.md"))
            .collect(),
        Err(_) => Vec::new(),
    };
    files.sort();
    let mut rules = Vec::new();
    let mut problems = Vec::new();
    for path in files {
        let domain = path.file_stem().unwrap().to_string_lossy().into_owned();
        let file = format!("spec/{domain}.md");
        let text = fs::read_to_string(&path).unwrap_or_default();
        let mut parser = Parser {
            file,
            domain,
            rules: &mut rules,
            problems: &mut problems,
        };
        parser.parse(&text);
    }
    if problems.is_empty() {
        Ok(rules)
    } else {
        Err(problems)
    }
}

struct Parser<'a> {
    file: String,
    domain: String,
    rules: &'a mut Vec<Rule>,
    problems: &'a mut Vec<Problem>,
}

/// A line of a rule, with its 1-based number. In a blockquote, its `>` is taken off.
struct Line<'t> {
    number: usize,
    text: &'t str,
}

impl Parser<'_> {
    fn parse(&mut self, text: &str) {
        let lines: Vec<&str> = text.lines().collect();
        let mut fenced = false;
        let mut i = 0;
        while i < lines.len() {
            let line = lines[i];
            if is_fence(unquote(line).unwrap_or(line)) {
                fenced = !fenced;
                i += 1;
                continue;
            }
            if fenced {
                i += 1;
                continue;
            }
            if line.starts_with('>') {
                // A blockquote: a rule if its first line is a marker.
                let mut quote = Vec::new();
                while i < lines.len() && lines[i].starts_with('>') {
                    quote.push(Line {
                        number: i + 1,
                        text: unquote(lines[i]).unwrap(),
                    });
                    i += 1;
                }
                self.quote(&quote);
                continue;
            }
            if line.starts_with("r[") {
                let start = i;
                let mut body = Vec::new();
                i += 1;
                while i < lines.len() && !ends_paragraph(lines[i]) {
                    body.push(Line {
                        number: i + 1,
                        text: lines[i],
                    });
                    i += 1;
                }
                self.plain_rule(start + 1, line, &body);
                continue;
            }
            if let Some(key) = marker_key(line) {
                self.problem(i + 1, None, format!(
                    "a `{key}:` marker outside a rule; markers end a rule written as a blockquote"
                ));
            }
            i += 1;
        }
    }

    fn plain_rule(&mut self, number: usize, marker: &str, body: &[Line]) {
        let Some((id, version)) = self.rule_marker(number, marker) else {
            return;
        };
        for line in body {
            if marker_key(line.text).is_some() {
                self.problem(
                    line.number,
                    Some(&id),
                    "a rule with markers is written as a \
                    blockquote, so its markers are part of its versioned text"
                        .into(),
                );
            }
        }
        let text = join(body.iter().map(|l| l.text));
        self.push(number, id, version, text, Markers::default());
    }

    fn quote(&mut self, quote: &[Line]) {
        let Some(first) = quote.iter().position(|l| !l.text.trim().is_empty()) else {
            return;
        };
        if !quote[first].text.starts_with("r[") {
            for line in quote {
                if let Some(key) = marker_key(line.text) {
                    self.problem(
                        line.number,
                        None,
                        format!("a `{key}:` marker outside a rule"),
                    );
                }
            }
            return;
        }
        let number = quote[first].number;
        let Some((id, version)) = self.rule_marker(number, quote[first].text) else {
            return;
        };
        let body = &quote[first + 1..];

        // The markers are the list at the end of the quote. Items may wrap onto lines indented
        // under them.
        let mut end = body.len();
        while end > 0 && body[end - 1].text.trim().is_empty() {
            end -= 1;
        }
        let mut list_start = end;
        while list_start > 0 {
            let text = body[list_start - 1].text;
            if text.starts_with("- ") || (text.starts_with("  ") && !text.trim().is_empty()) {
                list_start -= 1;
            } else {
                break;
            }
        }
        let mut items: Vec<(usize, String)> = Vec::new();
        for line in &body[list_start..end] {
            if let Some(item) = line.text.strip_prefix("- ") {
                items.push((line.number, item.trim().to_owned()));
            } else if let Some((_, item)) = items.last_mut() {
                item.push(' ');
                item.push_str(line.text.trim());
            }
        }
        // Rule text may end in a list of its own; the markers are the items from the first one
        // that looks like `- key: …` on.
        let first_marker = items
            .iter()
            .position(|(_, item)| looks_like_marker(item))
            .unwrap_or(items.len());
        let text_end = if first_marker < items.len() {
            body.iter()
                .position(|l| l.number == items[first_marker].0)
                .unwrap()
        } else {
            end
        };
        for line in &body[..text_end] {
            if let Some(key) = marker_key(line.text) {
                self.problem(line.number, Some(&id), format!(
                    "a `{key}:` marker in the middle of the rule; markers are the list that ends it"
                ));
            }
        }
        let mut markers = Markers::default();
        for (number, item) in &items[first_marker..] {
            self.marker(*number, &id, item, &mut markers);
        }
        let text = join(body[..text_end].iter().map(|l| l.text));
        self.push(number, id, version, text, markers);
    }

    /// Parses `r[id]` or `r[id+n]`, alone on its line, and checks the id's shape.
    fn rule_marker(&mut self, number: usize, line: &str) -> Option<(String, u32)> {
        let Some((inner, rest)) = line[2..].split_once(']') else {
            self.problem(number, None, "a rule marker with no closing `]`".into());
            return None;
        };
        let (id, version) =
            match inner.split_once('+') {
                Some((id, n)) => match n.parse::<u32>() {
                    Ok(n) if n >= 2 => (id, n),
                    _ => {
                        self.problem(number, Some(id), format!(
                        "`+{n}` is not a version: versions count up from `+2`, and version 1 \
                         has no suffix"
                    ));
                        return None;
                    }
                },
                None => (inner, 1),
            };
        if !rest.trim().is_empty() {
            self.problem(
                number,
                Some(id),
                "the marker stands alone on its line, with the \
                rule's text from the next line"
                    .into(),
            );
        }
        let segments: Vec<&str> = id.split('.').collect();
        if segments.len() < 3 {
            self.problem(
                number,
                Some(id),
                "an id has at least three parts: domain.topic.rule".into(),
            );
        } else if segments[0] != self.domain {
            self.problem(
                number,
                Some(id),
                format!(
                    "the id starts with `{}`, but rules in {} start with `{}.`",
                    segments[0], self.file, self.domain
                ),
            );
        }
        if !segments.iter().all(|s| is_kebab(s)) {
            self.problem(
                number,
                Some(id),
                "each part of an id is lowercase kebab-case: \
                a-z, 0-9 and single hyphens"
                    .into(),
            );
        }
        Some((id.to_owned(), version))
    }

    fn marker(&mut self, number: usize, id: &str, item: &str, markers: &mut Markers) {
        let (key, value) = item.split_once(':').unwrap();
        let value = value.trim();
        match key {
            "tier" => {
                if markers.tier {
                    self.problem(number, Some(id), "a second `tier:` marker".into());
                    return;
                }
                let (tier, reason) = match split_reason(value) {
                    Some((tier, reason)) => (tier, Some(reason)),
                    None => (value, None),
                };
                match (tier, reason) {
                    ("ci", _) => {
                        self.problem(
                            number,
                            Some(id),
                            "`ci` is the default tier and is never \
                            written: drop the line"
                                .into(),
                        );
                    }
                    ("machine" | "human", None) => {
                        self.problem(number, Some(id), format!(
                            "a `{tier}` tier needs a one-line reason: `- tier: {tier} (<reason>)`"
                        ));
                    }
                    ("machine" | "human", Some(_)) => markers.tier = true,
                    (other, _) => {
                        self.problem(
                            number,
                            Some(id),
                            format!(
                                "`{other}` is not a tier: a rule is `ci` (by default), `machine` \
                             or `human`"
                            ),
                        );
                    }
                }
            }
            "parity" => {
                if markers.parity.is_some() {
                    self.problem(number, Some(id), "a second `parity:` marker".into());
                    return;
                }
                match split_reason(value) {
                    Some((python, why)) if !python.is_empty() => {
                        markers.parity = Some(Parity {
                            python: python.into(),
                            why: why.into(),
                        });
                    }
                    _ => self.problem(
                        number,
                        Some(id),
                        "a parity change says what Python did \
                        and why the rule differs: `- parity: <what Python did> (<why>)`"
                            .into(),
                    ),
                }
            }
            "model" => {
                if !MODELS.contains(&value) {
                    self.problem(
                        number,
                        Some(id),
                        format!(
                            "`{value}` is not one of the eight models: {}",
                            MODELS.join(", ")
                        ),
                    );
                }
            }
            "fuzz" => {
                if !is_kebab(value) {
                    self.problem(
                        number,
                        Some(id),
                        format!(
                            "`{value}` is not a fuzz harness name: one kebab-case word, such as \
                         `link-frame`"
                        ),
                    );
                }
            }
            other => self.problem(
                number,
                Some(id),
                format!(
                    "`{other}:` is not a marker: markers are {}",
                    MARKER_KEYS.join(", ")
                ),
            ),
        }
    }

    fn push(&mut self, line: usize, id: String, version: u32, text: String, markers: Markers) {
        self.rules.push(Rule {
            id,
            version,
            file: self.file.clone(),
            line,
            text,
            parity: markers.parity,
        });
    }

    fn problem(&mut self, line: usize, rule: Option<&str>, message: String) {
        self.problems.push(Problem {
            file: self.file.clone(),
            line,
            rule: rule.map(str::to_owned),
            message,
        });
    }
}

/// The markers met so far in one rule. Only parity is kept: nothing reads the others yet.
#[derive(Default)]
struct Markers {
    tier: bool,
    parity: Option<Parity>,
}

/// The line without its blockquote `>` and the one space after it.
fn unquote(line: &str) -> Option<&str> {
    let rest = line.strip_prefix('>')?;
    Some(rest.strip_prefix(' ').unwrap_or(rest))
}

fn is_fence(line: &str) -> bool {
    let line = line.trim_start();
    line.starts_with("```") || line.starts_with("~~~")
}

/// What ends a plain rule's paragraph, as tracey reads it.
fn ends_paragraph(line: &str) -> bool {
    line.trim().is_empty()
        || line.starts_with('#')
        || line.starts_with("r[")
        || line.starts_with('>')
}

/// `Some(key)` when the line is a list item for a known marker key.
fn marker_key(line: &str) -> Option<&'static str> {
    let item = line.strip_prefix("- ")?;
    let (key, _) = item.split_once(':')?;
    MARKER_KEYS.iter().find(|k| **k == key.trim()).copied()
}

/// A list item of the form `key: value`, with a one-word lowercase key.
fn looks_like_marker(item: &str) -> bool {
    item.split_once(':').is_some_and(|(key, _)| {
        !key.is_empty() && key.chars().all(|c| c.is_ascii_lowercase() || c == '-')
    })
}

/// Splits `text (reason)` at its last parenthesised group, which may hold parentheses itself.
fn split_reason(value: &str) -> Option<(&str, &str)> {
    let body = value.strip_suffix(')')?;
    let mut depth = 1;
    for (i, c) in body.char_indices().rev() {
        match c {
            ')' => depth += 1,
            '(' => {
                depth -= 1;
                if depth == 0 {
                    let reason = body[i + 1..].trim();
                    return (!reason.is_empty()).then(|| (body[..i].trim(), reason));
                }
            }
            _ => {}
        }
    }
    None
}

fn is_kebab(s: &str) -> bool {
    !s.is_empty()
        && !s.starts_with('-')
        && !s.ends_with('-')
        && !s.contains("--")
        && s.chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
}

fn join<'t>(lines: impl Iterator<Item = &'t str>) -> String {
    lines
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .collect::<Vec<_>>()
        .join(" ")
}
