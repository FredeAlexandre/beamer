//! `cargo xtask checklist`: the milestone's issue for Alex, with a box per `human`-tier rule.
//!
//! Each test runs the command on a scratch workspace holding the example spec in
//! `tests/fixtures/spec-example/`, with a fake `gh` first on `PATH`. The fake keeps its issues as
//! files in a temp dir and logs every call, so no test reaches GitHub.

#![cfg(unix)]

mod fixture;

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::Output;
use std::time::{Duration, SystemTime};

use fixture::{Fixture, assert_ok, stderr, stdout};

const EXAMPLE: &str = include_str!("fixtures/spec-example/spec/example.md");
const SPEC: &str = "spec/example.md";
const REPO: &str = "owner/repo";

/// The example's one `human`-tier rule, with what its `do:` and `see:` markers say.
const HUMAN: &str = "example.driving.landing-point";
const DO: &str = "Move the pointer across the edge to the other machine";
const SEE: &str = "The pointer appears on the other screen at the same height it left";

/// A fake `gh` that answers the calls `checklist` makes, as GitHub would:
///
/// - `issue list … --jq <filter containing("<text>")>`: the numbers of the issues whose body
///   holds `<text>`, one per line;
/// - `issue view <n> … --json body --jq .body`: the issue's body;
/// - `issue create … --body-file -` and `issue edit <n> … --body-file -`: store the body read from
///   stdin, print the issue's URL;
/// - `gist create … <file>`: print a gist's URL.
///
/// Anything else fails, so a call the fake doesn't know shows up as a failed run.
const FAKE_GH: &str = r#"#!/bin/sh
state="$(dirname "$0")/state"
mkdir -p "$state/issues"
for arg in "$@"; do printf '%s\n' "$arg"; done > "$state/last"
printf '%s\n' "$*" >> "$state/calls"
case "$1 $2" in
  "issue list")
    text=$(printf '%s' "$*" | sed -n 's/.*contains("\([^"]*\)").*/\1/p')
    for f in "$state"/issues/*.md; do
      [ -e "$f" ] || continue
      if grep -qF -- "$text" "$f"; then basename "$f" .md; fi
    done ;;
  "issue view")
    cat "$state/issues/$3.md" ;;
  "issue create")
    n=$(( $(ls "$state/issues" | wc -l) + 1 ))
    cat > "$state/issues/$n.md"
    echo "https://github.com/owner/repo/issues/$n" ;;
  "issue edit")
    [ -e "$state/issues/$3.md" ] || { echo "no issue $3" >&2; exit 1; }
    cat > "$state/issues/$3.md"
    echo "https://github.com/owner/repo/issues/$3" ;;
  "gist create")
    n=$(grep -c '^gist create' "$state/calls")
    echo "https://gist.github.com/alex/report$n" ;;
  *)
    echo "fake gh: unknown call: $*" >&2
    exit 1 ;;
esac
"#;

/// A workspace with the example spec, and the fake `gh` it runs `checklist` with.
struct Desk {
    ws: Fixture,
    bin: tempfile::TempDir,
}

impl Desk {
    fn new() -> Self {
        let ws = Fixture::new(&[(SPEC, EXAMPLE)]);
        fs::remove_file(ws.path("spec/demo.md")).unwrap();
        ws.commit("only the example spec");
        let bin = tempfile::tempdir().unwrap();
        let gh = bin.path().join("gh");
        fs::write(&gh, FAKE_GH).unwrap();
        fs::set_permissions(&gh, fs::Permissions::from_mode(0o755)).unwrap();
        Desk { ws, bin }
    }

    /// A desk whose latest machine-tier report is `run-1.json`.
    fn with_report() -> Self {
        let desk = Desk::new();
        desk.report("run-1.json", 60);
        desk
    }

    /// Writes a machine-tier report to `dev/.reports/`, last modified `age` seconds ago.
    fn report(&self, name: &str, age: u64) -> PathBuf {
        let path = self.ws.path(&format!("dev/.reports/{name}"));
        fixture::write(&path, "{\"example.zone.held-at-edge\": {\"pass\": true}}\n");
        let file = fs::File::options().write(true).open(&path).unwrap();
        file.set_modified(SystemTime::now() - Duration::from_secs(age))
            .unwrap();
        path
    }

    /// `cargo xtask checklist <args> --root <ws> --repo owner/repo`.
    fn run(&self, args: &[&str]) -> Output {
        let path = format!(
            "{}:{}",
            self.bin.path().display(),
            std::env::var("PATH").unwrap()
        );
        std::process::Command::new(env!("CARGO_BIN_EXE_xtask"))
            .arg("checklist")
            .args(args)
            .arg("--root")
            .arg(self.ws.root())
            .args(["--repo", REPO])
            .env("PATH", path)
            .current_dir(self.ws.root())
            .output()
            .expect("the xtask binary runs")
    }

    /// Runs `checklist` and asserts it passed.
    fn checklist(&self, args: &[&str]) -> Output {
        let out = self.run(args);
        assert_ok(&out);
        out
    }

    /// The bodies of the fake's issues, by number.
    fn issues(&self) -> Vec<(u32, String)> {
        let dir = self.bin.path().join("state/issues");
        let mut issues: Vec<(u32, String)> = match fs::read_dir(&dir) {
            Ok(entries) => entries
                .map(|e| e.unwrap().path())
                .map(|p| {
                    let n = p.file_stem().unwrap().to_str().unwrap().parse().unwrap();
                    (n, fs::read_to_string(p).unwrap())
                })
                .collect(),
            Err(_) => Vec::new(),
        };
        issues.sort();
        issues
    }

    /// The one issue the fake holds.
    fn issue(&self) -> String {
        let issues = self.issues();
        assert_eq!(issues.len(), 1, "expected one issue, got {issues:#?}");
        issues.into_iter().next().unwrap().1
    }

    /// Every call made to `gh`, one per line.
    fn calls(&self) -> String {
        fs::read_to_string(self.bin.path().join("state/calls")).unwrap_or_default()
    }

    /// The arguments of the last call made to `gh`, one per line.
    fn last_call(&self) -> String {
        fs::read_to_string(self.bin.path().join("state/last")).unwrap_or_default()
    }

    fn edit_spec(&self, from: &str, to: &str) {
        let text = self.ws.read(SPEC);
        assert!(text.contains(from), "{SPEC} has no {from:?}");
        self.ws.write(SPEC, &text.replacen(from, to, 1));
        self.ws.commit("edit the spec");
    }

    fn add_rule(&self, rule: &str) {
        let text = self.ws.read(SPEC);
        self.ws.write(SPEC, &format!("{text}\n{rule}\n"));
        self.ws.commit("add a rule");
    }
}

/// The issue's line for `id`: the box that names it.
fn line<'a>(body: &'a str, id: &str) -> &'a str {
    body.lines()
        .find(|l| l.starts_with("- [") && l.contains(&format!("`{id}`")))
        .unwrap_or_else(|| panic!("no box for `{id}` in:\n{body}"))
}

/// A `human`-tier rule, in the example's `driving` topic.
fn human_rule(name: &str) -> String {
    format!(
        "> r[example.driving.{name}]\n> The {name} rule.\n>\n\
         > - tier: human (seen)\n> - do: Do {name}\n> - see: See {name}"
    )
}

#[test]
fn a_first_run_opens_one_issue_with_a_box_per_human_tier_rule() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    let body = desk.issue();
    let found = line(&body, HUMAN);
    assert!(found.starts_with("- [ ] "), "an unticked box:\n{body}");
    let item = &body[body.find(found).unwrap()..];
    let item = &item[..item.find("\n- [").unwrap_or(item.len())];
    assert!(item.contains(DO), "says what to do:\n{body}");
    assert!(item.contains(SEE), "says what to see:\n{body}");
    assert_eq!(
        body.matches("- [ ]").count(),
        1,
        "no box for a ci or machine rule:\n{body}"
    );
    assert!(!body.contains("example.zone.held-at-edge"), "{body}");
}

#[test]
fn the_issue_is_for_alex_and_named_for_its_milestone() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    let create = desk
        .calls()
        .lines()
        .find(|c| c.starts_with("issue create"))
        .map(str::to_owned)
        .expect("an issue is opened");
    assert!(create.contains("--label needs:alex"), "{create}");
    let call = desk.last_call();
    let title: Vec<&str> = call.lines().skip_while(|a| *a != "--title").collect();
    assert!(title[1].starts_with("M1 "), "{call}");
    for call in desk.calls().lines().filter(|c| c.starts_with("issue")) {
        assert!(call.contains(&format!("-R {REPO}")), "{call}");
    }
}

#[test]
fn the_issue_says_a_failed_box_gets_a_comment_and_becomes_a_triage_issue() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    let body = desk.issue();
    let said = body
        .lines()
        .find(|l| l.contains("fail"))
        .unwrap_or_else(|| panic!("nothing on a failed box:\n{body}"));
    assert!(said.contains("comment"), "{said}");
    assert!(said.contains("`build:triage`"), "{said}");
}

#[test]
fn it_links_the_latest_machine_tier_report_in_dev_reports() {
    let desk = Desk::new();
    desk.report("b-older.json", 600);
    let newest = desk.report("a-newest.json", 60);
    desk.checklist(&["M1"]);
    let gists: Vec<String> = desk
        .calls()
        .lines()
        .filter(|c| c.starts_with("gist create"))
        .map(str::to_owned)
        .collect();
    assert_eq!(gists.len(), 1, "{gists:?}");
    assert!(
        gists[0].ends_with(newest.to_str().unwrap()),
        "uploads the newest report: {gists:?}"
    );
    let body = desk.issue();
    let link = body
        .lines()
        .find(|l| l.contains("https://gist.github.com/alex/report1"))
        .unwrap_or_else(|| panic!("no link to the report:\n{body}"));
    assert!(link.contains("a-newest.json"), "names the report: {link}");
    assert!(!body.contains("b-older.json"), "{body}");
}

#[test]
fn a_report_given_by_url_is_linked_as_it_is() {
    let desk = Desk::new();
    let url = "https://github.com/owner/repo/actions/runs/1";
    desk.checklist(&["M1", "--report", url]);
    assert!(desk.issue().contains(url), "{}", desk.issue());
    assert!(!desk.calls().contains("gist"), "{}", desk.calls());
}

#[test]
fn a_report_given_by_path_is_the_one_uploaded() {
    let desk = Desk::with_report();
    let older = desk.report("chosen.json", 6000);
    desk.checklist(&["M1", "--report", older.to_str().unwrap()]);
    let calls = desk.calls();
    let gist = calls
        .lines()
        .find(|c| c.starts_with("gist create"))
        .unwrap();
    assert!(gist.ends_with(older.to_str().unwrap()), "{calls}");
    assert!(desk.issue().contains("chosen.json"));
}

#[test]
fn without_a_machine_tier_report_no_issue_is_opened() {
    let desk = Desk::new();
    let out = desk.run(&["M1"]);
    assert!(!out.status.success(), "{}", stdout(&out));
    let err = stderr(&out);
    assert!(
        err.contains("cargo xtask machine"),
        "says how to get one: {err}"
    );
    assert!(err.contains("--report"), "{err}");
    assert!(desk.issues().is_empty());
}

impl Desk {
    /// Ticks the box for `id` in issue `n`, as Alex does on GitHub.
    fn tick(&self, n: u32, id: &str) {
        let path = self.bin.path().join(format!("state/issues/{n}.md"));
        let body = fs::read_to_string(&path).unwrap();
        let found = line(&body, id).to_owned();
        let ticked = found.replacen("- [ ]", "- [x]", 1);
        fs::write(&path, body.replacen(&found, &ticked, 1)).unwrap();
    }
}

#[test]
fn running_twice_for_a_milestone_updates_its_issue_instead_of_opening_another() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    desk.add_rule(&human_rule("new-check"));
    desk.checklist(&["M1"]);
    let body = desk.issue();
    line(&body, HUMAN);
    line(&body, "example.driving.new-check");
    assert!(desk.calls().contains("issue edit 1 "), "{}", desk.calls());
}

#[test]
fn each_milestone_has_its_own_issue() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    desk.checklist(&["M2"]);
    desk.checklist(&["M1"]);
    assert_eq!(desk.issues().len(), 2, "{:#?}", desk.issues());
}

#[test]
fn an_update_keeps_alex_s_ticks() {
    let desk = Desk::with_report();
    desk.add_rule(&human_rule("other"));
    desk.checklist(&["M1"]);
    desk.tick(1, HUMAN);
    desk.checklist(&["M1"]);
    let body = desk.issue();
    assert!(line(&body, HUMAN).starts_with("- [x] "), "{body}");
    assert!(
        line(&body, "example.driving.other").starts_with("- [ ] "),
        "{body}"
    );
}

#[test]
fn a_bumped_rule_is_checked_again() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    desk.tick(1, HUMAN);
    desk.edit_spec(&format!("r[{HUMAN}]"), &format!("r[{HUMAN}+2]"));
    desk.checklist(&["M1"]);
    let body = desk.issue();
    assert!(
        line(&body, &format!("{HUMAN}+2")).starts_with("- [ ] "),
        "its meaning changed, so its old tick no longer holds:\n{body}"
    );
}

#[test]
fn an_update_uploads_a_report_only_when_a_newer_one_is_there() {
    let desk = Desk::with_report();
    desk.checklist(&["M1"]);
    desk.checklist(&["M1"]);
    let gists = |desk: &Desk| desk.calls().matches("gist create").count();
    assert_eq!(gists(&desk), 1, "the same report keeps its link");
    assert!(desk.issue().contains("report1"), "{}", desk.issue());
    desk.report("run-2.json", 0);
    desk.checklist(&["M1"]);
    assert_eq!(gists(&desk), 2);
    let body = desk.issue();
    assert!(
        body.contains("run-2.json") && body.contains("report2"),
        "{body}"
    );
    assert!(!body.contains("run-1.json"), "{body}");
}
