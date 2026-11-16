//! `cargo xtask spec digest`: every rule added, bumped or removed between two commits, written as
//! the comment the daily spec digest posts on the pinned 'Spec digest' issue (#45).
//!
//! Parity and human-tier changes come first: those are the ones that need Alex's approval
//! (spec/README.md). The digest's last line names the commit it ran through, and the routine
//! (`dev/routines/spec-digest.md`) starts the next digest there. When no rule changed, nothing is
//! printed and the routine posts nothing, so the next digest still starts where the last one ended.

use std::collections::BTreeMap;
use std::path::Path;
use std::process::Command;

use crate::Result;
use crate::spec::{self, Rule};
use crate::workspace::git;

pub fn run(root: &Path, since: &str, until: &str, repo: Option<&str>) -> Result {
    let since = commit(root, since)?;
    let until = commit(root, until)?;
    if git(root, &["merge-base", "--is-ancestor", &since, &until]).is_err() {
        return Err(format!(
            "{since} is not an ancestor of {until}: the branch was rewritten after the last \
             digest, so there is no history between them to read. Rerun with a --since on it."
        ));
    }
    let before = rules_at(root, &since)?;
    let after = rules_at(root, &until)?;
    let commits = commits_by_rule(root, &since, &until)?;
    let changes = changes(&before, &after, &commits);
    if changes.is_empty() {
        eprintln!("No rule added, bumped or removed in {since}..{until}.");
        return Ok(());
    }
    let mut prs = Prs::new(repo);
    print!("{}", render(&changes, &since, &until, &mut prs)?);
    Ok(())
}

/// The full hash of `rev`'s commit.
fn commit(root: &Path, rev: &str) -> Result<String> {
    git(
        root,
        &[
            "rev-parse",
            "--verify",
            "--quiet",
            &format!("{rev}^{{commit}}"),
        ],
    )
    .map(|out| out.trim().to_owned())
    .map_err(|_| format!("`{rev}` is not a commit here"))
}

/// The rules of every domain file in `spec/` at `rev`, by id.
fn rules_at(root: &Path, rev: &str) -> Result<BTreeMap<String, Rule>> {
    let listed = git(root, &["ls-tree", "-z", "--name-only", rev, "--", "spec/"])?;
    let mut rules = BTreeMap::new();
    for path in listed.split('\0') {
        let Some(domain) = path
            .strip_prefix("spec/")
            .and_then(|name| name.strip_suffix(".md"))
            .filter(|domain| *domain != "README" && !domain.contains('/'))
        else {
            continue;
        };
        let text = git(root, &["show", &format!("{rev}:{path}")])?;
        for rule in spec::rules_in(domain, &text) {
            rules.insert(rule.id.clone(), rule);
        }
    }
    Ok(rules)
}

/// For each rule whose version changed, appeared or disappeared in a commit after `since`, those
/// commits, oldest first.
fn commits_by_rule(root: &Path, since: &str, until: &str) -> Result<BTreeMap<String, Vec<String>>> {
    let range = format!("{since}..{until}");
    let listed = git(
        root,
        &[
            "rev-list",
            "--reverse",
            "--first-parent",
            &range,
            "--",
            "spec/",
        ],
    )?;
    let mut by_rule: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for commit in listed.lines() {
        let old = rules_at(root, &format!("{commit}^"))?;
        let new = rules_at(root, commit)?;
        for id in old.keys().chain(new.keys()) {
            let version = |rules: &BTreeMap<String, Rule>| rules.get(id).map(|r| r.version);
            let commits = by_rule.entry(id.clone()).or_default();
            if version(&old) != version(&new) && commits.last() != Some(&commit.to_owned()) {
                commits.push(commit.to_owned());
            }
        }
    }
    Ok(by_rule)
}

enum Kind {
    Added,
    Bumped { from: u32 },
    Removed,
}

struct Change<'r> {
    kind: Kind,
    /// The rule as it is at `until`, or as it was at `since` when removed.
    rule: &'r Rule,
    /// A parity or human-tier rule, before or after: Alex approves those.
    first: bool,
    commits: &'r [String],
}

fn changes<'r>(
    before: &'r BTreeMap<String, Rule>,
    after: &'r BTreeMap<String, Rule>,
    commits: &'r BTreeMap<String, Vec<String>>,
) -> Vec<Change<'r>> {
    let mut changes: Vec<Change> = commits
        .iter()
        .filter_map(|(id, commits)| {
            let (was, now) = (before.get(id), after.get(id));
            let kind = match (was, now) {
                (None, Some(_)) => Kind::Added,
                (Some(was), Some(now)) if now.version > was.version => {
                    Kind::Bumped { from: was.version }
                }
                (Some(_), None) => Kind::Removed,
                _ => return None,
            };
            Some(Change {
                kind,
                rule: now.or(was)?,
                first: was.into_iter().chain(now).any(needs_approval),
                commits,
            })
        })
        .collect();
    changes.sort_by(|a, b| (!a.first, &a.rule.id).cmp(&(!b.first, &b.rule.id)));
    changes
}

fn needs_approval(rule: &Rule) -> bool {
    rule.parity.is_some() || rule.tier.as_ref().is_some_and(|t| t.name == "human")
}

/// A GitHub comment holds at most 65,536 characters. This leaves room for the header and footer.
const BUDGET: usize = 60_000;

fn render(changes: &[Change], since: &str, until: &str, prs: &mut Prs) -> Result<String> {
    let count = match changes.len() {
        1 => "1 rule".to_owned(),
        n => format!("{n} rules"),
    };
    let range = match prs.repo {
        Some(repo) => format!(
            "[{}..{}](https://github.com/{repo}/compare/{since}...{until})",
            &since[..7],
            &until[..7]
        ),
        None => format!("{}..{}", &since[..7], &until[..7]),
    };
    let mut entries = Vec::new();
    for change in changes {
        let at = match change.kind {
            Kind::Removed => since,
            _ => until,
        };
        entries.push(entry(change, at, prs)?);
    }
    // Too long for one comment, and stage 0 can add hundreds of rules in a day: other changes
    // lose their text first, then the list stops where the budget does.
    let mut body = list(&entries, |_| true, usize::MAX);
    if body.chars().count() > BUDGET {
        body = list(&entries, |e| e.first, BUDGET);
    }
    Ok(format!(
        "## Spec digest: {count} changed\n\nCommits {range}.\n{body}\n\
         <!-- spec-digest through {until} -->\n"
    ))
}

/// The entries under their two headings, each with its details when `details` says so, up to
/// `budget` characters. Entries past the budget are counted in a last line instead.
fn list(entries: &[Entry], details: impl Fn(&Entry) -> bool, budget: usize) -> String {
    let mut out = String::new();
    let mut size = 0;
    let mut heading = None;
    for (i, entry) in entries.iter().enumerate() {
        let title = if entry.first {
            "Parity and human tier"
        } else {
            "Other changes"
        };
        let mut text = String::new();
        if heading != Some(title) {
            text += &format!("\n### {title}\n\n");
        }
        text += &entry.bullet;
        if details(entry) {
            text += &entry.details;
        }
        let length = text.chars().count();
        if size + length > budget {
            let left = entries.len() - i;
            out += &format!(
                "- …and {left} more changed rule(s), left out to fit GitHub's comment size: \
                 see the commits above.\n"
            );
            break;
        }
        out += &text;
        size += length;
        heading = Some(title);
    }
    out
}

struct Entry {
    first: bool,
    /// What happened, the rule, its tier and parity, and where it came from: one line.
    bullet: String,
    /// The rule's text and markers, indented under the bullet.
    details: String,
}

/// One change, rendered.
fn entry(change: &Change, at: &str, prs: &mut Prs) -> Result<Entry> {
    let rule = change.rule;
    let cited = format!("`{}`", rule.cited_id());
    let id = match prs.repo {
        Some(repo) => format!(
            "[{cited}](https://github.com/{repo}/blob/{at}/{}#L{})",
            rule.file, rule.line
        ),
        None => format!("{cited} ({})", rule.file),
    };
    let what = match change.kind {
        Kind::Added => "**added**",
        Kind::Bumped { .. } => "**bumped**",
        Kind::Removed => "**removed**",
    };
    let mut tags = Vec::new();
    if rule.parity.is_some() {
        tags.push("parity".to_owned());
    }
    if let Some(tier) = &rule.tier {
        tags.push(format!("{} tier", tier.name));
    }
    let mut sources = Vec::new();
    for commit in change.commits {
        let source = prs.source(commit)?;
        if !sources.contains(&source) {
            sources.push(source);
        }
    }
    let mut parts = vec![id];
    match change.kind {
        Kind::Bumped { from: 1 } => parts.push("from version 1".to_owned()),
        Kind::Bumped { from } => parts.push(format!("from +{from}")),
        _ => {}
    }
    parts.extend(tags);
    parts.push(sources.join(", "));
    let mut details = format!("  > {}\n", rule.text);
    if let Some(parity) = &rule.parity {
        details += &format!("  - parity: {} ({})\n", parity.python, parity.why);
    }
    if let Some(tier) = &rule.tier {
        details += &format!("  - tier: {} ({})\n", tier.name, tier.reason);
    }
    Ok(Entry {
        first: change.first,
        bullet: format!("- {what} {}\n", parts.join(" · ")),
        details,
    })
}

/// Each commit's pull request, looked up with `gh` when there is a repository to look in.
struct Prs<'a> {
    repo: Option<&'a str>,
    known: BTreeMap<String, String>,
}

impl<'a> Prs<'a> {
    fn new(repo: Option<&'a str>) -> Self {
        Prs {
            repo,
            known: BTreeMap::new(),
        }
    }

    /// `#n` for the merged pull requests that brought `commit` in, or the commit's short hash.
    fn source(&mut self, commit: &str) -> Result<String> {
        if let Some(known) = self.known.get(commit) {
            return Ok(known.clone());
        }
        let short = &commit[..7];
        let source = match self.repo {
            None => short.to_owned(),
            Some(repo) => {
                let numbers = gh_pulls(repo, commit)?;
                if numbers.is_empty() {
                    format!("{short} (no PR)")
                } else {
                    numbers
                        .iter()
                        .map(|n| format!("#{n}"))
                        .collect::<Vec<_>>()
                        .join(", ")
                }
            }
        };
        self.known.insert(commit.to_owned(), source.clone());
        Ok(source)
    }
}

/// The merged pull requests GitHub associates with `commit`. Rebase merges count: GitHub
/// associates the rebased commits with their pull request, whatever the base branch.
fn gh_pulls(repo: &str, commit: &str) -> Result<Vec<String>> {
    let endpoint = format!("repos/{repo}/commits/{commit}/pulls");
    let out = Command::new("gh")
        .args([
            "api",
            &endpoint,
            "--jq",
            ".[] | select(.merged_at) | .number",
        ])
        .output()
        .map_err(|e| format!("cannot run gh, needed for --repo: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "`gh api {endpoint}` failed:\n{}",
            String::from_utf8_lossy(&out.stderr).trim_end()
        ));
    }
    Ok(String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .map(str::to_owned)
        .collect())
}
