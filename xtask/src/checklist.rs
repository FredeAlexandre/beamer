//! `cargo xtask checklist <milestone>`: the issue Alex checks the `human`-tier rules from at the
//! end of a milestone (#22, ADR 0005, `.claude/skills/build-coordinator/milestone.md`).
//!
//! The issue has a box per `human`-tier rule: its id, what to do and what to see, from the rule's
//! `do:` and `see:` markers (spec/README.md). It links the latest machine-tier report, which
//! `cargo xtask machine` writes to `dev/.reports/` (#104).
//!
//! A hidden line in the body names the milestone, so a second run finds the issue and updates it.
//! The update keeps every box Alex ticked whose rule kept its version: a bumped rule changed
//! meaning, so its box is unticked for him to check again.

use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::Result;
use crate::spec::{self, Rule};
use crate::workspace::git;

pub struct Options<'a> {
    pub root: &'a Path,
    /// `M1` to `M4`, or any `M<n>`.
    pub milestone: &'a str,
    /// The GitHub repository, as owner/name, the issue is opened in.
    pub repo: &'a str,
    /// The machine-tier report to link: a URL, or a file to upload. Defaults to the newest report
    /// in `dev/.reports/`.
    pub report: Option<&'a str>,
    /// Rule-id prefixes, such as `crossing.zone`: when given, only their rules get a box.
    pub only: &'a [String],
    /// Print the body and say what would change, without opening, editing or uploading.
    pub dry_run: bool,
}

/// GitHub refuses an issue body over 65,536 characters. This leaves room for the hidden lines.
const BUDGET: usize = 65_000;

/// Where `cargo xtask machine` writes one JSON report per run (#104).
const REPORTS: &str = "dev/.reports";

pub fn run(options: &Options) -> Result {
    let milestone = options.milestone;
    if !milestone
        .strip_prefix('M')
        .is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
    {
        return Err(format!(
            "`{milestone}` is not a milestone: name it M1, M2, M3 or M4"
        ));
    }
    let rules = spec::rules(options.root)?;
    let human: Vec<&Rule> = rules
        .iter()
        .filter(|r| r.check.is_some() && in_scope(&r.id, options.only))
        .collect();
    let commit = git(options.root, &["rev-parse", "HEAD"])?;
    let report = find_report(options.root, options.report)?;
    let gh = Gh { repo: options.repo };
    let existing = match gh.find(&tag(milestone))? {
        Some(number) => Some((number, gh.body(number)?)),
        None => None,
    };
    let old = existing.as_ref().map(|(_, body)| body.as_str());
    let linked = old.and_then(linked_report);
    let ticked = old.map(ticked).unwrap_or_default();
    let checklist = Checklist {
        milestone,
        repo: options.repo,
        commit: commit.trim(),
        rules: &human,
        ticked: &ticked,
    };

    // Measured before the report is uploaded, so a checklist that can't be written uploads
    // nothing either.
    let length = checklist
        .render(&report.local(linked.as_ref()))
        .chars()
        .count();
    if length > BUDGET {
        return Err(format!(
            "the checklist for {milestone} is {length} characters, more than a GitHub issue holds: \
             scope it to the milestone's rule groups with --only <rule-id prefix>"
        ));
    }
    if options.dry_run {
        let body = checklist.render(&report.local(linked.as_ref()));
        print!("{body}");
        match existing {
            Some((number, _)) => eprintln!("dry run: would update #{number}"),
            None => eprintln!("dry run: would open an issue titled {:?}", title(milestone)),
        }
        return Ok(());
    }
    let body = checklist.render(&report.link(linked)?);
    let url = match existing {
        Some((number, _)) => gh.edit(number, &body)?,
        None => gh.create(&title(milestone), &body)?,
    };
    println!("{url}");
    Ok(())
}

/// Whether `id` is under one of the `only` prefixes, a whole part at a time. With none, every id is.
fn in_scope(id: &str, only: &[String]) -> bool {
    only.is_empty()
        || only.iter().any(|prefix| {
            id == prefix
                || id
                    .strip_prefix(prefix.as_str())
                    .is_some_and(|rest| rest.starts_with('.'))
        })
}

fn title(milestone: &str) -> String {
    format!("{milestone} checklist: the human-tier rules")
}

/// The hidden line that marks the milestone's issue.
fn tag(milestone: &str) -> String {
    format!("<!-- xtask-checklist {milestone} -->")
}

/// The hidden line that records which report the issue links, so an update can keep the link.
const REPORT_TAG: &str = "<!-- machine-report ";

/// A machine-tier report, as `--report` names it or as found in `dev/.reports/`.
enum Report {
    Url(String),
    File(PathBuf),
}

/// A report the issue can link: its name and its URL.
struct Link {
    name: String,
    url: String,
}

fn find_report(root: &Path, given: Option<&str>) -> Result<Report> {
    if let Some(given) = given {
        if given.starts_with("https://") || given.starts_with("http://") {
            return Ok(Report::Url(given.to_owned()));
        }
        let path = PathBuf::from(given);
        if !path.is_file() {
            return Err(format!("--report {given}: no such file"));
        }
        return Ok(Report::File(path));
    }
    let newest = fs::read_dir(root.join(REPORTS))
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|e| e == "json"))
        .filter_map(|path| Some((fs::metadata(&path).ok()?.modified().ok()?, path)))
        .max();
    match newest {
        Some((_, path)) => Ok(Report::File(path)),
        None => Err(format!(
            "no machine-tier report in {REPORTS}/: run `cargo xtask machine` first, or name the \
             report with --report <file or URL>"
        )),
    }
}

impl Report {
    /// The link to show without uploading anything: the one the issue has for this report, or
    /// the report's local path.
    fn local(&self, linked: Option<&Link>) -> Link {
        match self {
            Report::Url(url) => Link {
                name: url.clone(),
                url: url.clone(),
            },
            Report::File(path) => {
                let name = path.file_name().unwrap().to_string_lossy().into_owned();
                match linked.filter(|l| l.name == name) {
                    Some(linked) => Link {
                        name,
                        url: linked.url.clone(),
                    },
                    None => Link {
                        name,
                        url: path.display().to_string(),
                    },
                }
            }
        }
    }

    /// The report's link: a URL as it is, or a file uploaded as a secret gist. A file the issue
    /// already links, by name, keeps its gist.
    fn link(&self, linked: Option<Link>) -> Result<Link> {
        match self {
            Report::Url(url) => Ok(Link {
                name: url.clone(),
                url: url.clone(),
            }),
            Report::File(path) => {
                let name = path.file_name().unwrap().to_string_lossy().into_owned();
                if let Some(linked) = linked.filter(|l| l.name == name) {
                    return Ok(linked);
                }
                let description = format!("Beamer machine-tier report {name}");
                let path = path.to_str().ok_or("the report's path is not UTF-8")?;
                let out = gh(&["gist", "create", "--desc", &description, path], None)?;
                let url = out.lines().last().unwrap_or_default().trim().to_owned();
                Ok(Link { name, url })
            }
        }
    }
}

/// The report an issue's body links, from its hidden line.
fn linked_report(body: &str) -> Option<Link> {
    let line = body.lines().find_map(|l| l.strip_prefix(REPORT_TAG))?;
    let (url, name) = line.strip_suffix(" -->")?.split_once(' ')?;
    Some(Link {
        name: name.to_owned(),
        url: url.to_owned(),
    })
}

/// The rules whose boxes are ticked in an issue's body, as cited, with their version.
fn ticked(body: &str) -> BTreeSet<String> {
    body.lines()
        .filter_map(|l| {
            l.strip_prefix("- [x] ")
                .or_else(|| l.strip_prefix("- [X] "))
        })
        .map(|l| l.strip_prefix('[').unwrap_or(l))
        .filter_map(|l| l.strip_prefix('`')?.split_once('`'))
        .map(|(id, _)| id.to_owned())
        .collect()
}

/// What the issue lists, apart from the report it links.
struct Checklist<'a> {
    milestone: &'a str,
    repo: &'a str,
    /// The commit the rules are read at, which their links point into.
    commit: &'a str,
    rules: &'a [&'a Rule],
    /// The cited ids whose boxes Alex ticked.
    ticked: &'a BTreeSet<String>,
}

impl Checklist<'_> {
    fn render(&self, report: &Link) -> String {
        let Checklist {
            milestone,
            repo,
            commit,
            ..
        } = self;
        let mut body = format!(
            "Alex's check of the `human`-tier rules at the end of {milestone} (spec/README.md). \
             For each box, do what it says, and tick it once you have seen what it says.\n\n\
             **A box that fails** gets a comment saying what you saw instead, and becomes a \
             `build:triage` issue under #24. Leave it unticked.\n\n\
             **Machine tier**: the latest report, [`{}`]({}).\n\n",
            report.name, report.url
        );
        if self.rules.is_empty() {
            body += "No `human`-tier rule is in this checklist.\n";
        }
        for rule in self.rules {
            let check = rule.check.as_ref().unwrap();
            let id = rule.cited_id();
            let tick = if self.ticked.contains(&id) { 'x' } else { ' ' };
            let _ = write!(
                body,
                "- [{tick}] [`{id}`](https://github.com/{repo}/blob/{commit}/{}#L{})\n  \
                 - **Do**: {}\n  - **See**: {}\n",
                rule.file, rule.line, check.act, check.see
            );
        }
        let _ = write!(
            body,
            "\n{}\n{REPORT_TAG}{} {} -->\n",
            tag(milestone),
            report.url,
            report.name
        );
        body
    }
}

/// `gh` on the issues of one repository: every call carries `-R <repo>`.
struct Gh<'a> {
    repo: &'a str,
}

impl Gh<'_> {
    /// The issue, open or closed, whose body holds `tag`. Listing reads the repository's issues
    /// directly, unlike a search, which lags behind an issue just opened.
    fn find(&self, tag: &str) -> Result<Option<u64>> {
        let filter = format!(".[] | select(.body | contains(\"{tag}\")) | .number");
        let out = self.run(
            &[
                "issue",
                "list",
                "--state",
                "all",
                "--limit",
                "10000",
                "--json",
                "number,body",
                "--jq",
                &filter,
            ],
            None,
        )?;
        let mut numbers: Vec<u64> = out
            .split_whitespace()
            .map(|n| {
                n.parse()
                    .map_err(|_| format!("gh listed {n:?} as an issue"))
            })
            .collect::<Result<_>>()?;
        numbers.sort();
        if numbers.len() > 1 {
            eprintln!(
                "warning: issues {numbers:?} all hold {tag}; updating the first, #{}",
                numbers[0]
            );
        }
        Ok(numbers.first().copied())
    }

    fn body(&self, number: u64) -> Result<String> {
        self.run(
            &[
                "issue",
                "view",
                &number.to_string(),
                "--json",
                "body",
                "--jq",
                ".body",
            ],
            None,
        )
    }

    /// Opens an issue for Alex and returns its URL.
    fn create(&self, title: &str, body: &str) -> Result<String> {
        self.run(
            &[
                "issue",
                "create",
                "--title",
                title,
                "--label",
                "needs:alex",
                "--body-file",
                "-",
            ],
            Some(body),
        )
    }

    /// Replaces an issue's body and returns its URL.
    fn edit(&self, number: u64, body: &str) -> Result<String> {
        self.run(
            &["issue", "edit", &number.to_string(), "--body-file", "-"],
            Some(body),
        )
    }

    fn run(&self, args: &[&str], stdin: Option<&str>) -> Result<String> {
        let mut args = args.to_vec();
        args.extend(["-R", self.repo]);
        gh(&args, stdin)
    }
}

/// Runs `gh <args>` with `stdin` as its input, and returns what it printed.
fn gh(args: &[&str], stdin: Option<&str>) -> Result<String> {
    let mut child = Command::new("gh")
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot run gh: {e}"))?;
    let mut input = child.stdin.take().unwrap();
    input
        .write_all(stdin.unwrap_or_default().as_bytes())
        .map_err(|e| format!("cannot write to gh: {e}"))?;
    drop(input);
    let out = child.wait_with_output().map_err(|e| format!("gh: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "`gh {}` failed:\n{}",
            args.join(" "),
            String::from_utf8_lossy(&out.stderr).trim_end()
        ));
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim_end().to_owned())
}
