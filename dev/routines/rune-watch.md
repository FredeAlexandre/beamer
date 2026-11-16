# Routine: monthly Rune watch

A Claude Code cloud routine that checks once a month whether [Rune](https://github.com/anomalyco/rune)
is going quiet, so the build can fall back to GPUI calmly rather than in a hurry. It reopens the
toolkit decision; it never acts on it. Built for #46. The thresholds come from ADR 0001
(`docs/adr/0001-rune-ui-toolkit.md`) and the findings of #19.

## When it trips

Rune counts as **quiet** when any one of these holds on the day the routine runs:

| Signal | Quiet when | Today (2026-11-16) |
|---|---|---|
| Rune's repository | archived, private or gone (404), or its issue tracker closed | public, open |
| Commit activity on `dev` | fewer than **50 commits in each of the last 4 whole weeks** (Monday to Monday, UTC) | 263–817 a week |
| Contributors | `kitlangton`, who wrote 74% of Rune's commits, has **no commit in those 4 weeks** | 1,812 |
| Hex's Rune pin | Hex no longer depends on `rune`, Hex is archived, or Hex has kept **one Rune rev for 8 weeks** (56 days) | `9587433`, set 2026-11-15 |

Why these numbers:

- **50 a week for 4 weeks**: #19 names "fewer than ~50 commits a week for several weeks", and ADR 0001
  fixes it at 4 weeks. Rune ran at 250–800 a week when chosen. SST's 8 commits in 30 days is what
  quiet looks like at Anomaly, so 50 is far above dead and far below today.
- **Kit's commits**: #19's bus-factor finding. Kit and Hona wrote 94% of Rune; Kit's commits moving
  elsewhere is the first sign of the direction stalling. A month with none from him trips.
- **Hex's pin for 8 weeks**: ADR 0001. Hex, Anomaly's shipping app, bumped its Rune pin every day
  or two in November 2026. Hex freezing its pin, or dropping Rune, says Anomaly's own product stopped
  following it.

When it trips, the routine opens one `needs:alex` issue with the evidence (or comments on the one
already open). When nothing trips, it posts nothing. When it cannot run its checks at all, it opens a
`needs:alex` issue saying so, because a watch that fails silently is no watch.

The lower-risk signals in #19 (OpenCode Desktop moving to Rune, a tagged release) are not checked:
they never call for action.

## Create the routine (Alex)

Routines run on Alex's claude.ai account, so this step is his. Do it once.

1. **Open** <https://claude.ai/code/routines> and choose **New routine**. (From a Claude Code
   session, `/schedule` does the same: give it the values below.)
2. **Name**: `Beamer: monthly Rune watch`.
3. **Repository**: `FredeAlexandre/beamer`. The routine only reads it and files issues; it never
   pushes. The Claude GitHub app must be installed on that repository.
4. **Schedule**: monthly, on the 1st at 07:00 local time. As cron: `0 7 1 * *`. If the form offers no
   monthly preset, use the custom cron field.
5. **Prompt**: paste everything inside the block under [The prompt](#the-prompt), from
   `You are the monthly Rune watch` to the last line, without the outer four-backtick fence.
6. **Connectors**: remove them all. The routine needs only `gh`.
7. **Environment**:
   - Network access must reach `api.github.com` (the default trusted allowlist does).
   - The routine runs `gh`. If the environment has no authenticated `gh`, add an environment
     variable `GH_TOKEN`: a fine-grained token with **Issues: read and write** on
     `FredeAlexandre/beamer` (public repositories are read with it too). It goes in the routine's
     environment, never in repository secrets (#22).
8. **Save**, then **Run now** once. Expected today: the run log shows the JSON report with
   `"tripped": false`, and nothing is posted on GitHub. If the log shows `gh` failing to
   authenticate, do step 7's `GH_TOKEN` and run it again.
9. Comment the routine's claude.ai link on #46, so the coordinator can close it.

To change a threshold later, edit this file and the routine's prompt in the same sitting: the
routine runs the copy pasted into it, not this file.

## The prompt

````text
You are the monthly Rune watch for the Beamer 2.0 build (FredeAlexandre/beamer).

Beamer's Rust rewrite draws its UI with Rune (github.com/anomalyco/rune), with GPUI as the
fallback (ADR 0001, docs/adr/0001-rune-ui-toolkit.md on the `rust` branch, decided in issues #13
and #19). Your one job: check whether Rune is going quiet, and bring it to Alex if it is. You
never change code, open PRs, push, or touch any repository other than FredeAlexandre/beamer's
issues. Every `gh` call that targets Beamer carries `-R FredeAlexandre/beamer`. Never write to
kalkman-code/beamer or to anything under anomalyco.

## 1. Make sure gh works

Run `gh auth status`. If `gh` is missing, install it (on Debian/Ubuntu: `apt-get install -y gh`,
or the official GitHub CLI apt repository). If it is not authenticated and GH_TOKEN is unset,
stop and report that in your final message; you cannot file anything.

## 2. Run the check

Write this script to /tmp/rune_watch.py exactly as given, then run `python3 /tmp/rune_watch.py`.
It prints one JSON report and exits 0 when the checks ran, 2 when they could not.

```python
"""Monthly Rune watch: is anomalyco/rune going quiet? Prints one JSON report.

Thresholds come from ADR 0001 and #19 on FredeAlexandre/beamer. Needs an authenticated `gh`.
Exit 0 with "tripped": false/true; exit 2 when a check could not run.
"""
import base64, datetime as dt, json, re, subprocess, sys, time

RUNE, HEX = "anomalyco/rune", "anomalyco/hex"
WEEKS, MIN_WEEKLY = 4, 50          # quiet: fewer than 50 commits in each of 4 weeks
PIN_DAYS = 56                      # quiet: Hex keeps one Rune rev for 8 weeks
LEAD = "kitlangton"                # quiet: Rune's lead author, 0 commits in 4 weeks
RUNE_DEP = re.compile(r'^rune\s*=\s*\{[^}\n]*\}', re.M)
REV = re.compile(r'git\s*=\s*"https://github\.com/anomalyco/rune(?:\.git)?"[^}\n]*rev\s*=\s*"([0-9a-f]{7,40})"')


def gh(*args, allow_404=False):
    for attempt in range(3):
        p = subprocess.run(["gh", "api", *args], capture_output=True, text=True)
        if p.returncode == 0:
            return json.loads(p.stdout) if p.stdout.strip() else None
        if allow_404 and "404" in p.stderr:
            return None
        time.sleep(5 * (attempt + 1))
    raise RuntimeError(f"gh api {' '.join(args)}: {p.stderr.strip()}")


def history_count(since, until, author_id=None):
    author = f', author: {{id: "{author_id}"}}' if author_id else ""
    history = f'history(since: "{since}", until: "{until}"{author}) {{ totalCount }}'
    q = ('query { repository(owner: "anomalyco", name: "rune") { defaultBranchRef { target { '
         '... on Commit { ' + history + ' } } } } }')
    return gh("graphql", "-f", f"query={q}")["data"]["repository"]["defaultBranchRef"]["target"]["history"]["totalCount"]


def iso(t):
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


def hex_rune_rev(ref):
    f = gh(f"repos/{HEX}/contents/Cargo.toml?ref={ref}", allow_404=True)
    if not f:
        return None, None
    text = base64.b64decode(f["content"]).decode()
    dep = RUNE_DEP.search(text)
    if not dep:
        return None, None
    rev = REV.search(dep.group(0))
    return dep.group(0), rev.group(1) if rev else None


def main():
    now = dt.datetime.now(dt.timezone.utc)
    # Whole weeks, Monday 00:00 UTC to Monday, ending at the last Monday before now.
    end = (now - dt.timedelta(days=now.weekday())).replace(hour=0, minute=0, second=0, microsecond=0)
    weeks = [(end - dt.timedelta(weeks=i + 1), end - dt.timedelta(weeks=i)) for i in range(WEEKS)]
    report = {"checked_at": iso(now), "trips": [], "evidence": {}}
    ev = report["evidence"]

    # 1. Rune still public and open.
    repo = gh(f"repos/{RUNE}", allow_404=True)
    if repo is None:
        report["trips"].append("Rune is gone or private: GET repos/anomalyco/rune returns 404.")
    else:
        ev["rune_repo"] = {k: repo[k] for k in ("archived", "private", "has_issues", "pushed_at", "default_branch")}
        if repo["archived"]:
            report["trips"].append("Rune is archived.")
        if repo["private"]:
            report["trips"].append("Rune is private.")
        if not repo["has_issues"]:
            report["trips"].append("Rune's issue tracker is closed.")

        # 2. Commit activity, per whole week, on the default branch.
        weekly = [{"week": iso(s)[:10], "commits": history_count(iso(s), iso(u))} for s, u in weeks]
        ev["weekly_commits"] = weekly
        if all(w["commits"] < MIN_WEEKLY for w in weekly):
            report["trips"].append(f"Fewer than {MIN_WEEKLY} commits in each of the last {WEEKS} weeks: "
                                   + ", ".join(f'{w["week"]}: {w["commits"]}' for w in weekly) + ".")

        # 3. Contributors: who wrote the last 4 weeks.
        since = iso(weeks[-1][0])
        top = gh(f"repos/{RUNE}/contributors?per_page=5") or []
        recent = {}
        for c in top + ([] if any(c["login"] == LEAD for c in top) else [{"login": LEAD}]):
            node = gh(f"users/{c['login']}")["node_id"]
            recent[c["login"]] = history_count(since, iso(end), node)
        ev["commits_last_4_weeks_by_author"] = recent
        ev["all_time_top_contributors"] = {c["login"]: c["contributions"] for c in top}
        if recent.get(LEAD, 0) == 0:
            report["trips"].append(f"{LEAD}, Rune's lead author (74% of all commits at #19), "
                                   f"has no commit on Rune since {since[:10]}.")

    # 4. Hex's Rune pin.
    hexrepo = gh(f"repos/{HEX}", allow_404=True)
    if hexrepo is None or hexrepo["archived"]:
        report["trips"].append("Hex is gone, private or archived.")
    else:
        branch = hexrepo["default_branch"]
        dep, rev = hex_rune_rev(branch)
        ev["hex_rune_dependency"] = dep
        if dep is None:
            report["trips"].append("Hex's Cargo.toml no longer depends on `rune`: Hex dropped Rune.")
        elif rev is None:
            ev["hex_note"] = "Hex depends on rune, but not as a git rev of anomalyco/rune. Read the line."
        else:
            ev["hex_pin"] = rev
            pinned_since, page, done = None, 1, False
            while not done:
                commits = gh(f"repos/{HEX}/commits?path=Cargo.toml&sha={branch}&per_page=30&page={page}")
                if not commits:
                    break
                for c in commits:
                    when = c["commit"]["committer"]["date"]
                    if hex_rune_rev(c["sha"])[1] != rev:
                        done = True
                        break
                    pinned_since = when
                    if now - dt.datetime.fromisoformat(when.replace("Z", "+00:00")) > dt.timedelta(days=PIN_DAYS):
                        done = True  # already past the threshold; no need to dig further
                        break
                page += 1
            ev["hex_pin_since"] = pinned_since
            if pinned_since:
                age = (now - dt.datetime.fromisoformat(pinned_since.replace("Z", "+00:00"))).days
                ev["hex_pin_age_days"] = age
                if age >= PIN_DAYS:
                    report["trips"].append(f"Hex has kept Rune at {rev[:9]} since {pinned_since[:10]} "
                                           f"({age} days, threshold {PIN_DAYS}).")

    report["tripped"] = bool(report["trips"])
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a watch that cannot look is reported, never silent
        print(json.dumps({"tripped": None, "error": str(e)}, indent=2))
        sys.exit(2)
```

The thresholds live in the script. Do not change them, and do not add judgement of your own: the
script decides whether Rune is quiet.

## 3. Act on the report

**Exit 0 and `"tripped": false`**: stop. Post nothing: no issue, no comment, no label. Your final
message is the JSON report.

**Exit 0 and `"tripped": true`**:

1. Look for an open watch issue:
   `gh issue list -R FredeAlexandre/beamer --state open --label needs:alex --search "Rune watch in:title" --json number,title`
2. If one is open, add one comment to it: `Rune watch, <YYYY-MM>: still tripped.` followed by the
   trips and the evidence (format below). Open nothing new.
3. Otherwise open one issue:
   - Title: `Rune watch: Rune may be going quiet (<YYYY-MM>)`
   - Label: `needs:alex`
   - Body, in this order:
     - The ask, as the first line: `Alex: decide whether to reopen the toolkit choice (ADR 0001):
       keep Rune as pinned, maintain it ourselves from the FredeAlexandre/rune mirror, or start
       the GPUI fallback.`
     - `## What tripped`: one bullet per entry in `trips`, verbatim.
     - `## Evidence`: the weekly commit counts as a table (week, commits); the last-4-weeks
       commits by author as a table; Rune's repository flags; Hex's Rune dependency line, its
       pin and since when. Link https://github.com/anomalyco/rune/commits/dev,
       https://github.com/anomalyco/rune/graphs/contributors and
       https://github.com/anomalyco/hex/blob/dev/Cargo.toml.
     - `## Thresholds`: "Fewer than 50 commits in each of 4 whole weeks; kitlangton with no
       commit in those 4 weeks; Hex dropping Rune or keeping one Rune rev for 56 days; Rune
       archived, private, gone or with issues closed. From ADR 0001 and #19; set in
       dev/routines/rune-watch.md (#46)."
     - The raw JSON report in a collapsed `<details>` block.
   Create it with `gh issue create -R FredeAlexandre/beamer --label needs:alex --title ... --body-file ...`.

**Exit 2, or the script crashed**: run it once more. If it fails again, the watch cannot see,
which Alex must know. Look for an open issue titled `Rune watch: the check could not run`; if
there is one, comment the error with today's date; otherwise open it with the label `needs:alex`,
the ask `Alex: the monthly Rune watch could not run its checks. Fix the routine (its prompt is in
dev/routines/rune-watch.md) or the token it uses.`, and the error output in a code block.

Your final message: the JSON report, and the URL of any issue or comment you posted.
````
