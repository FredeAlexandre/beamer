//! `cargo xtask spec digest`: the rules added, bumped or removed on a branch since a commit, the
//! body of the daily spec digest (#45). Each test builds a scratch history from the example spec
//! in `tests/fixtures/spec-example/` and reads the digest the way the routine does.

mod fixture;

use fixture::{Fixture, assert_ok, stderr, stdout};

const EXAMPLE: &str = include_str!("fixtures/spec-example/spec/example.md");
const SPEC: &str = "spec/example.md";

/// A history whose first commit holds the example spec. Returns it with that commit's hash.
fn history() -> (Fixture, String) {
    let ws = Fixture::new(&[(SPEC, EXAMPLE)]);
    let base = head(&ws);
    (ws, base)
}

fn head(ws: &Fixture) -> String {
    ws.git(&["rev-parse", "HEAD"]).trim().to_owned()
}

/// Edits the example spec on disk, replacing `from` with `to`, and commits it.
fn change(ws: &Fixture, from: &str, to: &str, message: &str) -> String {
    let text = ws.read(SPEC);
    assert!(text.contains(from), "{SPEC} has no {from:?}");
    ws.write(SPEC, &text.replacen(from, to, 1));
    ws.commit(message);
    head(ws)
}

/// Appends `rules` to the example spec and commits it.
fn add(ws: &Fixture, rules: &str, message: &str) -> String {
    let text = ws.read(SPEC);
    ws.write(SPEC, &format!("{text}\n{rules}\n"));
    ws.commit(message);
    head(ws)
}

fn digest(ws: &Fixture, args: &[&str]) -> String {
    let root = ws.root().to_str().unwrap();
    let mut all = vec!["spec", "digest", "--root", root];
    all.extend_from_slice(args);
    let out = ws.xtask(&all);
    assert_ok(&out);
    stdout(&out)
}

/// The digest's line for `id`: the bullet that names it.
fn line<'a>(digest: &'a str, id: &str) -> &'a str {
    digest
        .lines()
        .find(|l| l.starts_with("- ") && l.contains(&format!("`{id}")))
        .unwrap_or_else(|| panic!("no line for `{id}` in:\n{digest}"))
}

fn position(digest: &str, id: &str) -> usize {
    let found = line(digest, id);
    digest.find(found).unwrap()
}

#[test]
fn nothing_is_printed_when_no_rule_changed() {
    let (ws, base) = history();
    ws.write("README.md", "Not the spec.\n");
    ws.commit("a change outside spec/");
    let out = digest(&ws, &["--since", &base]);
    assert_eq!(out, "", "the routine posts nothing on an empty digest");
}

#[test]
fn a_wording_fix_that_keeps_the_version_is_not_listed() {
    let (ws, base) = history();
    change(
        &ws,
        "A zone leads to exactly one peer.",
        "Each zone leads to exactly one peer.",
        "wording",
    );
    assert_eq!(digest(&ws, &["--since", &base]), "");
}

#[test]
fn an_added_rule_is_listed_with_its_text_and_commit() {
    let (ws, base) = history();
    let commit = add(
        &ws,
        "r[example.zone.corner]\nA zone may be a corner of a display.",
        "add a rule",
    );
    let out = digest(&ws, &["--since", &base]);
    let found = line(&out, "example.zone.corner");
    assert!(found.contains("added"), "{out}");
    assert!(found.contains(&commit[..7]), "names the commit:\n{out}");
    assert!(
        out.contains("A zone may be a corner of a display."),
        "{out}"
    );
}

#[test]
fn a_bumped_rule_is_listed_with_its_old_and_new_versions() {
    let (ws, base) = history();
    change(
        &ws,
        "r[example.zone.push+2]",
        "r[example.zone.push+3]",
        "bump push",
    );
    let out = digest(&ws, &["--since", &base]);
    let found = line(&out, "example.zone.push+3");
    assert!(found.contains("bumped"), "{out}");
    assert!(found.contains("+2"), "says what it was:\n{out}");
}

#[test]
fn a_rule_bumped_from_version_one_says_so() {
    let (ws, base) = history();
    change(
        &ws,
        "r[example.zone.leads-to-peer]",
        "r[example.zone.leads-to-peer+2]",
        "bump",
    );
    let out = digest(&ws, &["--since", &base]);
    assert!(
        line(&out, "example.zone.leads-to-peer+2").contains("version 1"),
        "{out}"
    );
}

#[test]
fn a_removed_rule_is_listed() {
    let (ws, base) = history();
    change(
        &ws,
        "r[example.zone.leads-to-peer]\nA zone leads to exactly one peer.\n",
        "",
        "drop a rule",
    );
    let out = digest(&ws, &["--since", &base]);
    assert!(
        line(&out, "example.zone.leads-to-peer").contains("removed"),
        "{out}"
    );
}

#[test]
fn only_changes_after_since_are_listed() {
    let (ws, _) = history();
    add(
        &ws,
        "r[example.zone.before]\nOld news.",
        "before the digest",
    );
    let since = head(&ws);
    add(&ws, "r[example.zone.after]\nNew.", "after the digest");
    let out = digest(&ws, &["--since", &since]);
    line(&out, "example.zone.after");
    assert!(!out.contains("example.zone.before"), "{out}");
}

#[test]
fn until_bounds_the_range() {
    let (ws, base) = history();
    let first = add(&ws, "r[example.zone.first]\nOne.", "first");
    add(&ws, "r[example.zone.second]\nTwo.", "second");
    let out = digest(&ws, &["--since", &base, "--until", &first]);
    line(&out, "example.zone.first");
    assert!(!out.contains("example.zone.second"), "{out}");
}

#[test]
fn a_rule_added_then_bumped_in_the_range_is_listed_once_with_both_commits() {
    let (ws, base) = history();
    let added = add(&ws, "r[example.zone.corner]\nA corner.", "add");
    let bumped = change(
        &ws,
        "r[example.zone.corner]\nA corner.",
        "r[example.zone.corner+2]\nA corner, at least 8 pixels wide.",
        "bump",
    );
    let out = digest(&ws, &["--since", &base]);
    let found = line(&out, "example.zone.corner+2");
    assert!(found.contains("added"), "{out}");
    assert!(
        found.contains(&added[..7]) && found.contains(&bumped[..7]),
        "{out}"
    );
    assert_eq!(out.matches("example.zone.corner").count(), 1, "{out}");
    assert!(
        out.contains("at least 8 pixels wide"),
        "shows the new text:\n{out}"
    );
}

#[test]
fn a_rule_added_and_removed_in_the_range_is_not_listed() {
    let (ws, base) = history();
    add(&ws, "r[example.zone.brief]\nGone soon.", "add");
    change(&ws, "r[example.zone.brief]\nGone soon.\n", "", "remove");
    assert_eq!(digest(&ws, &["--since", &base]), "");
}

#[test]
fn parity_and_human_tier_changes_are_listed_first() {
    let (ws, base) = history();
    add(
        &ws,
        "r[example.aaa.plain]\nSorts first by id, but has no marker.\n\n\
         > r[example.zzz.parity]\n> A parity change.\n>\n\
         > - parity: Python did it otherwise (#11 asks for this)\n\n\
         > r[example.zzz.human]\n> A human-tier rule.\n>\n> - tier: human (felt at the desk)\n\n\
         > r[example.aab.machine]\n> A machine-tier rule.\n>\n> - tier: machine (real input)",
        "add four rules",
    );
    let out = digest(&ws, &["--since", &base]);
    let parity = position(&out, "example.zzz.parity");
    let human = position(&out, "example.zzz.human");
    let plain = position(&out, "example.aaa.plain");
    let machine = position(&out, "example.aab.machine");
    assert!(parity < plain && parity < machine, "{out}");
    assert!(human < plain && human < machine, "{out}");
    assert!(
        out.contains("Python did it otherwise"),
        "shows the parity marker:\n{out}"
    );
    assert!(
        out.contains("felt at the desk"),
        "shows the tier's reason:\n{out}"
    );
}

#[test]
fn a_rule_that_leaves_the_human_tier_is_listed_first() {
    let (ws, base) = history();
    add(&ws, "r[example.aaa.plain]\nNo marker.", "plain");
    change(
        &ws,
        "> r[example.driving.landing-point]",
        "> r[example.driving.landing-point+2]",
        "bump",
    );
    change(
        &ws,
        ">\n> - tier: human (whether the pointer appears where the eye expects is judged at the desk)",
        "",
        "now ci",
    );
    let out = digest(&ws, &["--since", &base]);
    assert!(
        position(&out, "example.driving.landing-point+2") < position(&out, "example.aaa.plain"),
        "{out}"
    );
}

#[test]
fn the_digest_ends_with_the_commit_it_runs_through() {
    let (ws, base) = history();
    let until = add(&ws, "r[example.zone.corner]\nA corner.", "add");
    let out = digest(&ws, &["--since", &base]);
    assert_eq!(
        out.trim_end().lines().last().unwrap(),
        format!("<!-- spec-digest through {until} -->"),
        "the routine reads the next --since from this line"
    );
}

#[test]
fn a_since_that_is_not_on_the_branch_fails() {
    let (ws, _) = history();
    ws.git(&["switch", "--quiet", "--create", "side"]);
    let side = add(&ws, "r[example.zone.side]\nSide.", "side");
    ws.git(&["switch", "--quiet", "main"]);
    add(&ws, "r[example.zone.main]\nMain.", "main");
    let root = ws.root().to_str().unwrap();
    let out = ws.xtask(&["spec", "digest", "--root", root, "--since", &side]);
    assert!(!out.status.success(), "{}", stdout(&out));
    assert!(stderr(&out).contains("not an ancestor"), "{}", stderr(&out));
    assert!(stdout(&out).is_empty(), "prints no digest on failure");
}

/// With `--repo`, each commit is named by its pull request, looked up with `gh`. A fake `gh` on
/// `PATH` answers for one commit and knows no pull request for the other.
#[cfg(unix)]
#[test]
fn with_a_repo_each_change_names_its_pull_request() {
    use std::os::unix::fs::PermissionsExt;

    let (ws, base) = history();
    let by_pr = add(&ws, "r[example.zone.by-pr]\nVia a PR.", "add");
    let direct = add(&ws, "r[example.zone.direct]\nPushed.", "add");
    let bin = tempfile::tempdir().unwrap();
    let gh = bin.path().join("gh");
    std::fs::write(
        &gh,
        format!(
            "#!/bin/sh\n\
             echo \"$@\" >> \"{log}\"\n\
             case \"$*\" in\n\
               *repos/owner/repo/commits/{by_pr}/pulls*) echo 123 ;;\n\
               *) ;;\n\
             esac\n",
            log = bin.path().join("calls").display()
        ),
    )
    .unwrap();
    std::fs::set_permissions(&gh, std::fs::Permissions::from_mode(0o755)).unwrap();
    let path = format!(
        "{}:{}",
        bin.path().display(),
        std::env::var("PATH").unwrap()
    );
    let root = ws.root().to_str().unwrap();
    let out = std::process::Command::new(env!("CARGO_BIN_EXE_xtask"))
        .args(["spec", "digest", "--root", root, "--since", &base])
        .args(["--repo", "owner/repo"])
        .env("PATH", path)
        .current_dir(ws.root())
        .output()
        .unwrap();
    assert_ok(&out);
    let out = stdout(&out);
    assert!(line(&out, "example.zone.by-pr").contains("#123"), "{out}");
    let pushed = line(&out, "example.zone.direct");
    assert!(
        pushed.contains(&direct[..7]) && pushed.contains("no PR"),
        "{out}"
    );
    assert!(
        out.contains(&format!(
            "https://github.com/owner/repo/blob/{}/spec/example.md#L",
            head(&ws)
        )),
        "links each rule to its line:\n{out}"
    );
}

#[test]
fn the_example_spec_itself_reads_as_all_added() {
    // From an empty spec/ to the example: every rule in it is new, the start of stage 0.
    let ws = Fixture::new(&[]);
    std::fs::remove_file(ws.path("spec/demo.md")).unwrap();
    ws.commit("empty spec");
    let base = head(&ws);
    ws.write(SPEC, EXAMPLE);
    ws.commit("the example");
    let out = digest(&ws, &["--since", &base]);
    for id in [
        "example.zone.leads-to-peer",
        "example.zone.push+2",
        "example.zone.held-at-edge",
        "example.driving.move-number+3",
        "example.driving.landing-point",
        "example.frame.decode",
    ] {
        assert!(line(&out, id).contains("added"), "{out}");
    }
    assert!(
        position(&out, "example.zone.held-at-edge") < position(&out, "example.zone.leads-to-peer"),
        "the parity rule leads:\n{out}"
    );
}

/// `count` plain rules, each with a long text, plus one parity rule.
fn many_rules(count: usize) -> String {
    let filler = "and the pointer keeps its speed across the edge ".repeat(4);
    let mut rules = String::from(
        "> r[example.bulk.parity]\n> The parity rule.\n>\n> - parity: Python did otherwise (#11)\n",
    );
    for n in 0..count {
        rules += &format!("\nr[example.bulk.rule-{n:04}]\nRule {n} holds, {filler}\n");
    }
    rules
}

/// A GitHub comment holds at most 65,536 characters.
const COMMENT_LIMIT: usize = 65_536;

#[test]
fn a_digest_too_long_for_one_comment_drops_the_text_of_other_changes_first() {
    let (ws, base) = history();
    add(&ws, &many_rules(300), "stage 0");
    let out = digest(&ws, &["--since", &base]);
    assert!(out.chars().count() < COMMENT_LIMIT, "{} chars", out.len());
    for n in 0..300 {
        line(&out, &format!("example.bulk.rule-{n:04}"));
    }
    assert!(
        out.contains("The parity rule."),
        "parity changes keep their text"
    );
    assert!(!out.contains("Rule 12 holds"), "other changes lose theirs");
    assert!(out.trim_end().ends_with("-->"), "{out}");
}

#[test]
fn a_digest_too_long_even_without_texts_lists_as_many_rules_as_fit() {
    let (ws, base) = history();
    let until = add(&ws, &many_rules(2000), "stage 0");
    let out = digest(&ws, &["--since", &base]);
    assert!(out.chars().count() < COMMENT_LIMIT, "{} chars", out.len());
    line(&out, "example.bulk.parity");
    line(&out, "example.bulk.rule-0000");
    assert!(!out.contains("example.bulk.rule-1999"), "{out}");
    assert!(out.contains("more"), "says how many are left out");
    assert!(
        out.trim_end()
            .ends_with(&format!("<!-- spec-digest through {until} -->")),
        "the next digest still starts after these commits"
    );
}
