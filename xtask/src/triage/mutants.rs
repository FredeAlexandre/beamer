//! The surviving mutants of a nightly run, one group per function.
//!
//! Each `mutants-<runner>-shard-<k>` artifact is one shard's `mutants.out`. A **survivor** is a
//! mutant cargo-mutants reports missed or timed out: `cargo xtask mutants` runs every spec test
//! that reaches it (#135), and only spec tests (#33), so none of them told it from the code. The
//! shards split one function's mutants between them; its group gathers them all.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;

use super::{BUDGET, Group, Run, artifact_dirs, read_json};
use crate::Result;

struct Survivor {
    name: String,
    package: String,
    outcome: &'static str,
    line: u64,
    column: u64,
    /// The function's first and last line.
    lines: Option<(u64, u64)>,
    runner: String,
    artifact: String,
    diff_path: Option<String>,
    diff: Option<String>,
}

pub fn groups(dir: &Path, run: &Run) -> Result<Vec<Group>> {
    // (file, function) → its survivors, from every shard.
    let mut by_function: BTreeMap<(String, String), Vec<Survivor>> = BTreeMap::new();
    for (artifact, shard) in artifact_dirs(dir, "mutants-")? {
        let outcomes = shard.join("outcomes.json");
        if !outcomes.exists() {
            continue; // A shard with no mutant to test writes no outcomes.
        }
        let runner = artifact
            .strip_prefix("mutants-")
            .and_then(|rest| rest.split_once("-shard-"))
            .map_or("?", |(runner, _)| runner)
            .to_owned();
        let json = read_json(&outcomes)?;
        let list = json["outcomes"]
            .as_array()
            .ok_or_else(|| format!("{}: no outcomes", outcomes.display()))?;
        for outcome in list {
            let kind = match outcome["summary"].as_str() {
                Some("MissedMutant") => "missed",
                Some("Timeout") => "timed out",
                _ => continue,
            };
            let mutant = &outcome["scenario"]["Mutant"];
            let text = |v: &serde_json::Value| v.as_str().unwrap_or_default().to_owned();
            let function = &mutant["function"];
            let span = |end: &str| function["span"][end]["line"].as_u64();
            let diff_path = outcome["diff_path"].as_str().map(str::to_owned);
            let survivor = Survivor {
                name: text(&mutant["name"]),
                package: text(&mutant["package"]),
                outcome: kind,
                line: mutant["span"]["start"]["line"].as_u64().unwrap_or(0),
                column: mutant["span"]["start"]["column"].as_u64().unwrap_or(0),
                lines: span("start").zip(span("end")),
                runner: runner.clone(),
                artifact: artifact.clone(),
                diff: diff_path
                    .as_ref()
                    .and_then(|p| fs::read_to_string(shard.join(p)).ok()),
                diff_path,
            };
            let function = function["function_name"]
                .as_str()
                .unwrap_or("(outside any function)")
                .to_owned();
            by_function
                .entry((text(&mutant["file"]), function))
                .or_default()
                .push(survivor);
        }
    }

    Ok(by_function
        .into_iter()
        .map(|((file, function), mut survivors)| {
            survivors.sort_by(|a, b| (a.line, a.column, &a.name).cmp(&(b.line, b.column, &b.name)));
            group(run, &file, &function, &survivors)
        })
        .collect())
}

fn group(run: &Run, file: &str, function: &str, survivors: &[Survivor]) -> Group {
    let first = &survivors[0];
    let mut runners: Vec<&str> = survivors.iter().map(|s| s.runner.as_str()).collect();
    runners.dedup();
    let n = survivors.len();
    let mut body = format!(
        "{}\n\nNo spec test kills {} of `{function}` in {}, mutated on the {} runner:\n\n\
         | Mutant | Outcome | Diff |\n|---|---|---|\n",
        run.provenance(),
        if n == 1 {
            "this mutant".to_owned()
        } else {
            format!("these {n} mutants")
        },
        run.source(file, first.lines),
        runners
            .iter()
            .map(|r| format!("`{r}`"))
            .collect::<Vec<_>>()
            .join(", "),
    );
    for s in survivors {
        let diff = match &s.diff_path {
            Some(path) => format!("`{path}` in {}", run.artifact(&s.artifact)),
            None => format!("in {}", run.artifact(&s.artifact)),
        };
        let _ = writeln!(body, "| `{}` | {} | {diff} |", s.name, s.outcome);
    }
    let _ = write!(
        body,
        "\nWork it with the `build-mutant-triage` skill: each mutant is one case. \
         `gh run download {id} -R {repo} -n <artifact>` gives that shard's `mutants.out`, with the \
         mutant's diff at the path above. Artifacts are kept 30 days, so the diffs are copied \
         below.\n\n<details><summary>The diffs</summary>\n\n",
        id = run.id,
        repo = run.repo,
    );
    let mut left_out = 0;
    for s in survivors {
        let Some(diff) = &s.diff else {
            let _ = writeln!(body, "`{}`: its diff is not in the artifact.\n", s.name);
            continue;
        };
        let entry = format!(
            "`{}` ({}):\n\n```diff\n{}\n```\n\n",
            s.name,
            s.outcome,
            diff.trim_end()
        );
        if body.len() + entry.len() > BUDGET {
            left_out += 1;
        } else {
            body += &entry;
        }
    }
    if left_out > 0 {
        let _ = writeln!(
            body,
            "{left_out} more diffs are left out here: read them in the artifacts.\n"
        );
    }
    body += "</details>\n";

    Group {
        key: format!("mutant {file} {function}"),
        label: "triage:mutant",
        title: format!("Survivors: {} {function}", first.package),
        body,
        again: None,
    }
}
