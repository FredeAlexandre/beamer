//! `cargo xtask triage`: the deterministic half of the nightly triage routine (#44,
//! `dev/routines/nightly-triage.md`). The routine downloads a nightly run's artifacts with `gh`;
//! this reads them and decides what to file, so the routine only carries the plan out.
//!
//! - `triage issues` groups the run's findings, one issue per group: the surviving mutants of one
//!   function (`triage:mutant`), the crashes of one fuzz harness (`triage:fuzz`), one failing model
//!   test (`triage:model`), and the nightly jobs that failed leaving nothing to triage
//!   (`needs:alex`). Each body ends with a hidden line naming its group, so a group whose issue is
//!   still open is not filed twice.
//! - `triage corpus` lists the fuzz corpus entries the run found that the checkout lacks, and
//!   copies them in, for the routine to propose as a pull request.

mod corpus;
mod failures;
mod mutants;

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use serde_json::{Value, json};

use crate::Result;

pub use corpus::run as corpus;

/// GitHub refuses an issue body over 65,536 characters. Bodies stop adding detail past this.
pub(crate) const BUDGET: usize = 60_000;

/// What `triage issues` reads, from the command line.
pub struct Options<'a> {
    /// Where `gh run download <run>` put the run's artifacts, one directory per artifact.
    pub dir: &'a Path,
    /// The nightly run's id.
    pub run: &'a str,
    /// The repository, as owner/name, the run and the issues are in.
    pub repo: &'a str,
    /// The `rust` commit the run tested.
    pub sha: Option<&'a str>,
    /// `gh api repos/<repo>/actions/runs/<run>/artifacts`, to link each artifact.
    pub artifacts: Option<&'a Path>,
    /// `gh run view <run> --json jobs`, to find the jobs that failed.
    pub jobs: Option<&'a Path>,
    /// `gh issue list --state open --json number,body`: the issues already filed.
    pub open: Option<&'a Path>,
}

/// The run the findings come from, as the issue bodies link it.
pub(crate) struct Run<'a> {
    pub id: &'a str,
    pub repo: &'a str,
    pub sha: Option<&'a str>,
    artifact_ids: BTreeMap<String, u64>,
}

impl Run<'_> {
    pub fn url(&self) -> String {
        format!("https://github.com/{}/actions/runs/{}", self.repo, self.id)
    }

    /// `From nightly run [<id>](<url>), which tested `rust` at [`<sha>`](<url>).`
    pub fn provenance(&self) -> String {
        let run = format!("From nightly run [{}]({})", self.id, self.url());
        match self.sha {
            Some(sha) => format!(
                "{run}, which tested `rust` at [`{}`](https://github.com/{}/commit/{sha}).",
                &sha[..sha.len().min(9)],
                self.repo
            ),
            None => format!("{run}."),
        }
    }

    /// The artifact as a link to its download, or its bare name when its id is unknown.
    pub fn artifact(&self, name: &str) -> String {
        match self.artifact_ids.get(name) {
            Some(id) => format!("[`{name}`]({}/artifacts/{id})", self.url()),
            None => format!("`{name}`"),
        }
    }

    /// A link to `path` (lines `lines`, when known) at the commit the run tested.
    pub fn source(&self, path: &str, lines: Option<(u64, u64)>) -> String {
        let Some(sha) = self.sha else {
            return format!("`{path}`");
        };
        let url = format!("https://github.com/{}/blob/{sha}/{path}", self.repo);
        match lines {
            Some((a, b)) => format!("[`{path}`, lines {a}–{b}]({url}#L{a}-L{b})"),
            None => format!("[`{path}`]({url})"),
        }
    }
}

/// One issue's worth of findings.
pub(crate) struct Group {
    /// Names the group in the body's hidden line: the same group on another night has the same key.
    pub key: String,
    pub label: &'static str,
    pub title: String,
    pub body: String,
    /// What to say on the group's issue when it is already open; nothing when `None`.
    pub again: Option<String>,
}

/// The hidden last line of a group's issue body.
fn marker(key: &str) -> String {
    format!("<!-- nightly-triage: {key} -->")
}

/// Prints the plan: the issues to open, the comments to post on issues already open, and the
/// groups whose issue is open and needs nothing.
pub fn issues(options: &Options) -> Result {
    if !options.dir.is_dir() {
        return Err(format!(
            "{} is not a directory of nightly artifacts: download them with `gh run download`",
            options.dir.display()
        ));
    }
    let run = Run {
        id: options.run,
        repo: options.repo,
        sha: options.sha,
        artifact_ids: options
            .artifacts
            .map(artifact_ids)
            .transpose()?
            .unwrap_or_default(),
    };
    let mut groups = mutants::groups(options.dir, &run)?;
    groups.extend(failures::groups(options.dir, &run, options.jobs)?);
    let open = options
        .open
        .map(open_issues)
        .transpose()?
        .unwrap_or_default();

    let (mut to_open, mut comment, mut skip) = (Vec::new(), Vec::new(), Vec::new());
    for group in groups {
        let marker = marker(&group.key);
        match open
            .iter()
            .find(|(_, body)| body.lines().any(|l| l.trim() == marker))
        {
            None => to_open.push(json!({
                "key": group.key,
                "label": group.label,
                "title": group.title,
                "body": format!("{}\n\n{marker}\n", group.body.trim_end()),
            })),
            Some((number, _)) => match group.again {
                Some(body) => {
                    comment.push(json!({ "key": group.key, "number": number, "body": body }))
                }
                None => skip.push(json!({ "key": group.key, "number": number })),
            },
        }
    }
    eprintln!(
        "triage: {} issue(s) to open, {} to comment on, {} already open",
        to_open.len(),
        comment.len(),
        skip.len()
    );
    let plan = json!({ "open": to_open, "comment": comment, "skip": skip });
    println!(
        "{}",
        serde_json::to_string_pretty(&plan).expect("a plan serializes")
    );
    Ok(())
}

pub(crate) fn read_json(path: &Path) -> Result<Value> {
    let text =
        fs::read_to_string(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))
}

/// Each artifact's id by its name, from the API's `{"artifacts": [{"id", "name"}, …]}`.
fn artifact_ids(path: &Path) -> Result<BTreeMap<String, u64>> {
    let json = read_json(path)?;
    let list = json["artifacts"]
        .as_array()
        .ok_or_else(|| format!("{}: no `artifacts` list", path.display()))?;
    Ok(list
        .iter()
        .filter_map(|a| Some((a["name"].as_str()?.to_owned(), a["id"].as_u64()?)))
        .collect())
}

/// Each open issue's number and body, from `gh issue list --json number,body`.
fn open_issues(path: &Path) -> Result<Vec<(u64, String)>> {
    let json = read_json(path)?;
    let list = json
        .as_array()
        .ok_or_else(|| format!("{}: not a list of issues", path.display()))?;
    Ok(list
        .iter()
        .filter_map(|i| Some((i["number"].as_u64()?, i["body"].as_str()?.to_owned())))
        .collect())
}

/// The directories directly in `dir` whose name starts with `prefix`, sorted.
pub(crate) fn artifact_dirs(dir: &Path, prefix: &str) -> Result<Vec<(String, std::path::PathBuf)>> {
    let entries = fs::read_dir(dir).map_err(|e| format!("cannot read {}: {e}", dir.display()))?;
    let mut dirs: Vec<_> = entries
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .filter_map(|e| {
            let name = e.file_name().into_string().ok()?;
            name.starts_with(prefix).then(|| (name, e.path()))
        })
        .collect();
    dirs.sort();
    Ok(dirs)
}
