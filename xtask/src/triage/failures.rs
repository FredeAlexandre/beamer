//! The fuzz crashes, the model failures and the failed jobs of a nightly run.
//!
//! - `fuzz-crashes` holds each crashing input at its path in the checkout,
//!   `<…>/__fuzz__/<harness>/crashes/<input>`: the upload's pattern starts with `**`, so
//!   upload-artifact keeps the whole path. One group per harness.
//! - `model-failures` holds one `<package>.log` per crate the model job tested, `cargo test`'s
//!   output. One group per failing test, or one per package whose tests did not run at all.
//! - A job that failed with neither of those to show for it is a broken nightly: one `needs:alex`
//!   group lists them all.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use super::{BUDGET, Group, Run, read_json};
use crate::Result;

pub fn groups(dir: &Path, run: &Run, jobs: Option<&Path>) -> Result<Vec<Group>> {
    let jobs = match jobs {
        Some(path) => read_jobs(path)?,
        None => Vec::new(),
    };
    let job_url = |prefix: &str| {
        jobs.iter()
            .find(|j| j.name.starts_with(prefix))
            .map(|j| j.url.clone())
    };
    let mut groups = fuzz(&dir.join("fuzz-crashes"), run, job_url("Fuzz").as_deref())?;
    groups.extend(model(
        &dir.join("model-failures"),
        run,
        job_url("Model").as_deref(),
    )?);
    groups.extend(broken(dir, run, &jobs));
    Ok(groups)
}

struct Job {
    name: String,
    conclusion: String,
    url: String,
}

/// The jobs of `gh run view <run> --json jobs`.
fn read_jobs(path: &Path) -> Result<Vec<Job>> {
    let json = read_json(path)?;
    let list = json["jobs"]
        .as_array()
        .ok_or_else(|| format!("{}: no `jobs` list", path.display()))?;
    Ok(list
        .iter()
        .map(|j| {
            let text = |key: &str| j[key].as_str().unwrap_or_default().to_owned();
            Job {
                name: text("name"),
                conclusion: text("conclusion"),
                url: text("url"),
            }
        })
        .collect())
}

/// One group per fuzz harness that crashed.
fn fuzz(dir: &Path, run: &Run, job: Option<&str>) -> Result<Vec<Group>> {
    // The harness directory, as a path in the checkout → its crashing inputs.
    let mut harnesses: BTreeMap<String, Vec<PathBuf>> = BTreeMap::new();
    for file in files(dir)? {
        let rel = slash_path(file.strip_prefix(dir).expect("under dir"));
        if let Some((harness, _input)) = rel.rsplit_once("/crashes/") {
            harnesses.entry(harness.to_owned()).or_default().push(file);
        }
    }
    let mut groups = Vec::new();
    for (harness, inputs) in harnesses {
        let name = harness.rsplit('/').next().unwrap_or(&harness);
        let package = crate_of(&harness);
        let n = inputs.len();
        let mut body = format!(
            "{}\n\n`cargo bolero test` crashed the fuzz harness `{name}` of `{package}` on {}, kept in \
             the {} artifact under `{harness}/crashes/`.{}\n\n\
             Work it with the `build-mutant-triage` skill (failures.md). The input is the crash \
             file restored into the harness's `corpus/`, where `cargo test -p {package}` replays \
             it. Artifacts are kept 30 days, so each input is copied here; run this from the \
             checkout's root to restore it:\n\n",
            run.provenance(),
            if n == 1 {
                "one input".to_owned()
            } else {
                format!("{n} inputs")
            },
            run.artifact("fuzz-crashes"),
            job.map(|url| format!(" [The fuzz job's log]({url}) has the panic."))
                .unwrap_or_default(),
        );
        for input in &inputs {
            let file_name = input.file_name().unwrap_or_default().to_string_lossy();
            let bytes =
                fs::read(input).map_err(|e| format!("cannot read {}: {e}", input.display()))?;
            let encoded = wrap(&base64(&bytes), 76);
            let entry = format!(
                "`{file_name}`, {} bytes:\n\n```sh\nmkdir -p {harness}/corpus\nbase64 -d > {harness}/corpus/{file_name} <<'EOF'\n{encoded}\nEOF\n```\n\n",
                bytes.len()
            );
            if body.len() + entry.len() > BUDGET {
                let _ = writeln!(
                    body,
                    "`{file_name}`, {} bytes, is too large to copy here: take it from the artifact.\n",
                    bytes.len()
                );
            } else {
                body += &entry;
            }
        }
        groups.push(Group {
            key: format!("fuzz {harness}"),
            label: "triage:fuzz",
            title: format!("Fuzz crash: {package} {name}"),
            body,
            again: None,
        });
    }
    Ok(groups)
}

/// One group per failing model test, or per package whose tests never ran.
fn model(dir: &Path, run: &Run, job: Option<&str>) -> Result<Vec<Group>> {
    let mut groups = Vec::new();
    for file in files(dir)? {
        let Some(package) = file
            .file_name()
            .and_then(|n| n.to_str())
            .and_then(|n| n.strip_suffix(".log"))
        else {
            continue;
        };
        let log = fs::read_to_string(&file)
            .map_err(|e| format!("cannot read {}: {e}", file.display()))?;
        let job_line = job
            .map(|url| format!(" [The model job's log]({url}) has the whole run."))
            .unwrap_or_default();
        let failed = failed_tests(&log);
        if failed.is_empty() {
            if !log.contains("error: could not compile") && !log.contains("test result: FAILED") {
                continue; // Its tests passed: another package failed the job.
            }
            let tail: Vec<&str> = log.lines().rev().take(80).collect();
            let tail: Vec<&str> = tail.into_iter().rev().collect();
            groups.push(Group {
                key: format!("model {package}"),
                label: "triage:model",
                title: format!("Model failure: {package} did not run"),
                body: format!(
                    "{}\n\nThe nightly's model job could not run the tests of `{package}`.{job_line} \
                     The end of its output, from `{package}.log` in the {} artifact:\n\n```text\n{}\n```\n\n\
                     Work it with the `build-mutant-triage` skill (failures.md).\n",
                    run.provenance(),
                    run.artifact("model-failures"),
                    cap(&tail.join("\n")),
                ),
                again: None,
            });
            continue;
        }
        for test in failed {
            let replay = match test.target.as_deref() {
                Some(t) if t.starts_with("tests/") => format!(
                    " --test {}",
                    t.trim_start_matches("tests/").trim_end_matches(".rs")
                ),
                Some(t) if t.starts_with("unittests ") => " --lib".to_owned(),
                _ => String::new(),
            };
            let target = test
                .target
                .as_deref()
                .map(|t| format!(" in `{t}`"))
                .unwrap_or_default();
            groups.push(Group {
                key: format!("model {package} {} {}", test.target.as_deref().unwrap_or("?"), test.name),
                label: "triage:model",
                title: format!("Model failure: {package} {}", test.name),
                body: format!(
                    "{}\n\nThe model test `{name}`{target} of `{package}` failed in the nightly's \
                     model job.{job_line} Its output, with the counterexample or the failing \
                     input, from `{package}.log` in the {artifact} artifact:\n\n```text\n{output}\n```\n\n\
                     Work it with the `build-mutant-triage` skill (failures.md). Replay it with:\n\n\
                     ```sh\ncargo test --release -p {package}{replay} -- --include-ignored --exact {name}\n```\n",
                    run.provenance(),
                    name = test.name,
                    artifact = run.artifact("model-failures"),
                    output = cap(test.output.trim_end()),
                ),
                again: None,
            });
        }
    }
    Ok(groups)
}

struct FailedTest {
    /// The test binary, as cargo's `Running` line names it: `tests/handover.rs`.
    target: Option<String>,
    name: String,
    output: String,
}

/// The tests `cargo test`'s output reports `FAILED`, each with its captured output.
fn failed_tests(log: &str) -> Vec<FailedTest> {
    let mut failed = Vec::new();
    let mut target = None;
    let mut lines = log.lines().peekable();
    let mut outputs: BTreeMap<String, String> = BTreeMap::new();
    while let Some(line) = lines.next() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("Running ") {
            target = Some(rest.split(" (").next().unwrap_or(rest).to_owned());
        } else if let Some(name) = trimmed
            .strip_prefix("test ")
            .and_then(|r| r.strip_suffix(" ... FAILED"))
        {
            failed.push((target.clone(), name.to_owned()));
        } else if let Some(name) = trimmed
            .strip_prefix("---- ")
            .and_then(|r| r.strip_suffix(" stdout ----"))
        {
            let mut output = String::new();
            while let Some(next) = lines.peek() {
                if next.starts_with("---- ") || next.trim() == "failures:" {
                    break;
                }
                output += lines.next().expect("peeked");
                output.push('\n');
            }
            outputs.insert(name.to_owned(), output.trim_matches('\n').to_owned() + "\n");
        }
    }
    failed
        .into_iter()
        .map(|(target, name)| FailedTest {
            output: outputs.remove(&name).unwrap_or_default(),
            target,
            name,
        })
        .collect()
}

/// The jobs that failed without leaving their findings: a fuzz job without `fuzz-crashes`, a
/// model job without `model-failures`, and every other job that failed.
fn broken(dir: &Path, run: &Run, jobs: &[Job]) -> Option<Group> {
    let covered = |job: &Job| {
        (job.name.starts_with("Fuzz") && dir.join("fuzz-crashes").is_dir())
            || (job.name.starts_with("Model") && dir.join("model-failures").is_dir())
    };
    let failed: Vec<&Job> = jobs
        .iter()
        .filter(|j| {
            !matches!(
                j.conclusion.as_str(),
                "success" | "skipped" | "neutral" | ""
            )
        })
        .filter(|j| !covered(j))
        .collect();
    if failed.is_empty() {
        return None;
    }
    let list: String = failed
        .iter()
        .map(|j| format!("- [{}]({}): {}\n", j.name, j.url, j.conclusion))
        .collect();
    Some(Group {
        key: "nightly jobs failed".to_owned(),
        label: "needs:alex",
        title: "Nightly: jobs failed with nothing to triage".to_owned(),
        body: format!(
            "Alex: these jobs of nightly run [{id}]({url}) failed and left nothing the triage \
             routine can file, so the night's findings are incomplete. Read their logs, then fix \
             what broke: the nightly (`.github/workflows/nightly.yml` on `main`) or the code it \
             runs on `rust`.\n\n{list}\nThe nightly triage routine (`dev/routines/nightly-triage.md`) \
             comments here each night they fail again. Close this issue once the nightly is \
             whole.\n",
            id = run.id,
            url = run.url(),
        ),
        again: Some(format!(
            "Nightly run [{id}]({url}): these jobs failed again, leaving nothing to triage:\n\n{list}",
            id = run.id,
            url = run.url(),
        )),
    })
}

/// Every file under `dir`, sorted; none when `dir` doesn't exist. Symbolic links are skipped.
pub(super) fn files(dir: &Path) -> Result<Vec<PathBuf>> {
    let mut out = Vec::new();
    if !dir.is_dir() {
        return Ok(out);
    }
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in fs::read_dir(&d).map_err(|e| format!("cannot read {}: {e}", d.display()))? {
            let entry = entry.map_err(|e| format!("cannot read {}: {e}", d.display()))?;
            let kind = entry
                .file_type()
                .map_err(|e| format!("{}: {e}", entry.path().display()))?;
            if kind.is_dir() {
                stack.push(entry.path());
            } else if kind.is_file() {
                out.push(entry.path());
            }
        }
    }
    out.sort();
    Ok(out)
}

/// `path` with `/` between its components, as GitHub and the issue bodies write it.
pub(super) fn slash_path(path: &Path) -> String {
    path.components()
        .map(|c| c.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/")
}

/// The crate a path in the checkout is in: the directory under `crates/`.
fn crate_of(path: &str) -> &str {
    path.strip_prefix("crates/")
        .and_then(|rest| rest.split('/').next())
        .unwrap_or(path)
}

/// `text`, cut to fit an issue body.
fn cap(text: &str) -> String {
    const MAX: usize = 40_000;
    if text.len() <= MAX {
        return text.to_owned();
    }
    let mut start = text.len() - MAX;
    while !text.is_char_boundary(start) {
        start += 1;
    }
    format!(
        "[… cut: the first {start} bytes are in the artifact]\n{}",
        &text[start..]
    )
}

/// Standard base64, padded, as `base64 -d` reads it.
fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let b = [
            chunk[0],
            *chunk.get(1).unwrap_or(&0),
            *chunk.get(2).unwrap_or(&0),
        ];
        let n = (u32::from(b[0]) << 16) | (u32::from(b[1]) << 8) | u32::from(b[2]);
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

fn wrap(text: &str, width: usize) -> String {
    text.as_bytes()
        .chunks(width)
        .map(|c| std::str::from_utf8(c).expect("base64 is ASCII"))
        .collect::<Vec<_>>()
        .join("\n")
}
