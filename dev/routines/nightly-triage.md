# Routine: nightly triage

A Claude Code cloud routine that reads the night's nightly run with `gh` and turns its findings into
issues the build works: surviving mutants, fuzz crashes and model failures. It also proposes the
fuzz corpus the run grew as a pull request. Built for #44. The nightly itself (`nightly.yml`, on
`main`) stays deterministic in Actions (#22, ADR 0005); this routine reads what it left.

## What it files

`cargo xtask triage issues` (`xtask/src/triage/`) decides what to file from the run's artifacts; the
routine posts its plan as printed. One issue per **group**:

| Group | Label | From the artifact | The issue carries |
|---|---|---|---|
| The survivors of one function: every mutant of it cargo-mutants reports missed or timed out, from all shards | `triage:mutant` | `mutants-<runner>-shard-<k>` (each shard's `mutants.out`) | each mutant, a link to its shard's artifact with the diff's path in it, the diffs themselves, and the function's lines at the tested `rust` commit |
| The crashes of one fuzz harness | `triage:fuzz` | `fuzz-crashes` | each crashing input, base64-encoded with the command that restores it into the harness's `corpus/` |
| One failing model test, or a model crate whose tests didn't run | `triage:model` | `model-failures` | the test's output, with its counterexample or failing input, and the command that replays it |
| The nightly's jobs that failed leaving nothing to triage (a mutants shard, the probe, the kill matrix, the bench, or a fuzz or model job without its artifact) | `needs:alex` | the run's jobs | each failed job, linked |

Each body ends with a hidden line naming its group, such as
`<!-- nightly-triage: mutant crates/beamer-types/src/lib.rs is_landscape -->`. A group whose issue
is still open is not filed again; the `needs:alex` group gets a comment instead, each night its jobs
fail again. A closed issue doesn't hold its group back: a mutant that survives after its issue was
settled is a new finding. The `triage:*` issues join the coordinator's frontier
(`.claude/skills/build-coordinator/queue.sh`) and are worked with the `build-mutant-triage` skill.

The artifacts are kept 30 days, so the issues copy what triage needs: the diffs and the inputs.

## The corpus pull request

When the run uploaded `fuzz-corpus`, `cargo xtask triage corpus` copies the entries `rust` lacks
into a checkout of the branch `claude/nightly-fuzz-corpus`, and the routine opens a pull request
into `rust` from it, or adds to the one already open. Entries are libFuzzer's, named by their
content's hash, so an entry is new when no file has its path. The pull request holds only those
files; nothing merges it automatically. The coordinator, or Alex, lands it like any other PR.

## What it never does

- No API key goes into repository secrets, and nothing runs from launchd: the routine runs in
  Anthropic's cloud on Alex's claude.ai account, and its GitHub token, if it needs one, lives in the
  routine's environment (#22, ADR 0005).
- It never pushes anywhere but `claude/nightly-fuzz-corpus`, never closes or edits an issue, and
  never writes to kalkman-code/beamer.
- It triages only scheduled runs. A `workflow_dispatch` run, such as a proof run on a demo branch,
  is never filed.

When the routine cannot run, it opens a `needs:alex` issue (or comments on the one already open),
because a triage that fails silently looks like a clean night.

## Create the routine (Alex)

Routines run on Alex's claude.ai account, so this step is his. Do it once.

1. **Open** <https://claude.ai/code/routines> and choose **New routine**. (From a Claude Code
   session, `/schedule` does the same: give it the values below.)
2. **Name**: `Beamer: nightly triage`.
3. **Repository**: `FredeAlexandre/beamer`. The Claude GitHub app must be installed on that
   repository. The routine pushes one branch, `claude/nightly-fuzz-corpus`: leave **unrestricted
   branch pushes** off, since a `claude/` branch needs no more.
4. **Schedule**: daily at 10:00 local time. As cron: `0 10 * * *`. The nightly starts at 02:17 UTC
   and its mutants shards may run five hours, so it has finished by then.
5. **Prompt**: paste everything inside the block under [The prompt](#the-prompt), from
   `You are the nightly triage` to the last line, without the outer four-backtick fence.
6. **Connectors**: remove them all. The routine needs only `gh`, `git` and `cargo`.
7. **Environment**:
   - Network access must reach `github.com`, `api.github.com`, `static.rust-lang.org` (rustup
     installs the pinned toolchain) and crates.io (`index.crates.io`, `static.crates.io`), which
     the default trusted allowlist covers. `gh run download` fetches artifacts from Azure blob
     storage (`*.blob.core.windows.net`) and `*.actions.githubusercontent.com`: if the first run's
     log shows the download failing on the network, add those two to the allowlist.
   - The routine runs `gh`. If the environment has no authenticated `gh`, add an environment
     variable `GH_TOKEN`: a fine-grained token on `FredeAlexandre/beamer` with **Actions: read**
     (artifacts and job logs), **Issues: read and write**, **Pull requests: read and write** and
     **Contents: read**. It goes in the routine's environment, never in repository secrets (#22).
8. **Save**. Run it once with **Run now** after the first scheduled nightly has finished: as of
   2026-11-16 only dispatched runs exist, and with no scheduled run to triage the routine files
   its "could not run" issue. Expected on that first run: the run log ends with the plan's counts,
   and any issue it opens is a real finding of that night. If the log shows `gh` failing to
   authenticate, do step 7's `GH_TOKEN` and run it again.
9. Comment the routine's claude.ai link on #44, so the coordinator can close it.

To change what is filed or how it is grouped, edit `xtask/src/triage/` and its tests
(`xtask/tests/triage.rs`): the routine runs whatever `rust` holds. To change the steps around it,
edit this file and the routine's prompt in the same sitting: the routine runs the copy pasted into
it, not this file. When `nightly.yml` renames a job or an artifact, `xtask/src/triage/` follows
it: it finds the probe by its name `What exists on the branch`, the fuzz and model jobs by the
names starting `Fuzz` and `Model`, and the artifacts by `mutants-*`, `fuzz-crashes`, `fuzz-corpus`
and `model-failures`.

## The prompt

````text
You are the nightly triage for the Beamer 2.0 build (FredeAlexandre/beamer).

Beamer's Rust rewrite lives on the `rust` branch. Each night the "Nightly" workflow
(.github/workflows/nightly.yml, scheduled on `main`) tests `rust`: cargo-mutants, fuzzing, a
model check. Your one job: file that run's findings as issues and propose its new fuzz corpus as a
pull request. `cargo xtask triage` decides what to file; you post its plan as printed. You never
change code, never close or edit an issue, and never push to any branch but
`claude/nightly-fuzz-corpus`. Every `gh` call carries `-R FredeAlexandre/beamer`. Never write to
kalkman-code/beamer.

Run the shell steps below as written. W is your scratch directory for the whole run.

## 1. Make sure gh works

Run `gh auth status`. If `gh` is missing, install it (on Debian/Ubuntu: `apt-get install -y gh`,
or the official GitHub CLI apt repository). If it is not authenticated and GH_TOKEN is unset,
stop and report that in your final message; you cannot post anything.

## 2. Check out rust

Your checkout starts on the default branch, `main`. Run, from the repository root:

    git fetch origin rust
    git checkout --detach origin/rust

If `cargo` is missing, install rustup with no default toolchain and load it:

    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none
    . "$HOME/.cargo/env"

The checkout's rust-toolchain.toml makes rustup install the pinned toolchain on first use.

## 3. Find the night's run

    W=/tmp/nightly-triage; rm -rf "$W"; mkdir -p "$W"
    gh run list -R FredeAlexandre/beamer -w nightly.yml --event schedule --status completed -L 1 \
      --json databaseId,createdAt,url > "$W/runs.json"
    RUN=$(jq -r '.[0].databaseId // empty' "$W/runs.json")
    CREATED=$(jq -r '.[0].createdAt // empty' "$W/runs.json")

Only scheduled runs count: never triage a `workflow_dispatch` run. If RUN is empty, or CREATED is
more than 48 hours ago (`[ $(( $(date -u +%s) - $(date -u -d "$CREATED" +%s) )) -gt 172800 ]`),
the nightly has stopped running: go to step 8 and say so, with CREATED. (GitHub disables a
scheduled workflow after 60 days without activity on the repository; Alex re-enables it on the
Actions tab.)

Triaging the same run twice files nothing new, so a run you already triaged yesterday is fine.

## 4. Gather the run

    gh run download "$RUN" -R FredeAlexandre/beamer -D "$W/artifacts" \
      -p 'mutants-*' -p 'fuzz-*' -p 'model-failures'

If it fails with "no artifact matches", the run left nothing to read: `mkdir -p "$W/artifacts"`
and go on. Any other failure: run it once more, then go to step 8.

    gh api "repos/FredeAlexandre/beamer/actions/runs/$RUN/artifacts?per_page=100" > "$W/artifacts.json"
    gh run view "$RUN" -R FredeAlexandre/beamer --json jobs > "$W/jobs.json"
    PROBE=$(jq -r '.jobs[] | select(.name == "What exists on the branch") | .databaseId' "$W/jobs.json")
    SHA=$(gh run view "$RUN" -R FredeAlexandre/beamer --log --job "$PROBE" \
      | grep -A1 'git log -1 --format=%H' | grep -oE '\b[0-9a-f]{40}\b' | head -n 1)
    for label in triage:mutant triage:fuzz triage:model needs:alex; do
      gh issue list -R FredeAlexandre/beamer --state open --label "$label" --limit 1000 --json number,body
    done | jq -s 'add' > "$W/open.json"

SHA is the `rust` commit the run tested. If it comes out empty, go on without it: the issues then
link no source lines.

## 5. Plan

    cargo xtask triage issues "$W/artifacts" --run "$RUN" --sha "$SHA" \
      --artifacts "$W/artifacts.json" --jobs "$W/jobs.json" --open "$W/open.json" > "$W/plan.json"

It prints a JSON plan: `open` (issues to open, each with its label, title and body), `comment`
(comments to post on issues already open) and `skip` (groups whose issue is already open). If it
exits non-zero, run it once more, then go to step 8.

## 6. File the plan

    jq -c '.open[]' "$W/plan.json" | while read -r issue; do
      jq -r .body <<<"$issue" > "$W/body.md"
      gh issue create -R FredeAlexandre/beamer --label "$(jq -r .label <<<"$issue")" \
        --title "$(jq -r .title <<<"$issue")" --body-file "$W/body.md" < /dev/null
    done
    jq -c '.comment[]' "$W/plan.json" | while read -r comment; do
      jq -r .body <<<"$comment" > "$W/body.md"
      gh issue comment "$(jq -r .number <<<"$comment")" -R FredeAlexandre/beamer \
        --body-file "$W/body.md" < /dev/null
    done

Post each one exactly as planned. Do not edit, summarise, merge or add to them, and open nothing
the plan does not list: the hidden last line of each body is how tomorrow's run knows the group is
filed. If a `gh` call fails, retry it once; if it fails again, go on with the rest and name it in
step 8's issue.

## 7. Propose the fuzz corpus

Skip this step when `$W/artifacts/fuzz-corpus` does not exist. Otherwise:

    B=claude/nightly-fuzz-corpus
    PR=$(gh pr list -R FredeAlexandre/beamer --head "$B" --base rust --state open --json number --jq '.[0].number // empty')
    git fetch origin "$B" 2>/dev/null || true  # the branch may not exist yet
    if [ -n "$PR" ]; then
      git checkout -B "$B" "origin/$B" && git rebase origin/rust
    else
      git checkout -B "$B" origin/rust
    fi
    cargo xtask triage corpus "$W/artifacts/fuzz-corpus" --write > "$W/corpus.txt"
    N=$(grep -c . "$W/corpus.txt" || true)

It prints each new entry's path, one per line, and copies it in. Then:

- **N is 0 and PR is empty**: nothing to propose. Stop this step.
- **N is 0 and PR is set**: `git push --force-with-lease origin "$B"`, so the open pull request
  stays up to date with `rust`.
- **N is more than 0**: commit those files alone and push:

      tr '\n' '\0' < "$W/corpus.txt" | xargs -0 git add --
      git commit -m "Fuzz corpus: $N new entries from nightly run $RUN"
      git push --force-with-lease origin "$B"

  If PR is set, comment on it: `gh pr comment "$PR" -R FredeAlexandre/beamer --body "Nightly run
  $RUN added $N entries."`. Otherwise open it, with auto-merge off:

      gh pr create -R FredeAlexandre/beamer --base rust --head "$B" \
        --title "Fuzz corpus from the nightly" --body-file "$W/pr.md"

  where `$W/pr.md` says: "New fuzz corpus entries from the nightly's `fuzz-corpus` artifact,
  proposed by the nightly triage routine (#44, `dev/routines/nightly-triage.md`). Each is an input
  libFuzzer found new coverage with; `cargo test` replays a harness's corpus, so merging them keeps
  that coverage in the merge gate. The routine adds each night's new entries here until this
  merges. The commits hold only corpus files." followed by the list in `$W/corpus.txt` as a
  bulleted list of code spans, and the line "Nightly run: https://github.com/FredeAlexandre/beamer/actions/runs/$RUN".

If a git or `gh` command in this step fails, go to step 8 with its output; never force anything
past an error.

## 8. When the routine cannot run

Alex must know: a triage that fails silently looks like a clean night. Look for an open issue
titled `Nightly triage: the routine could not run`:

    gh issue list -R FredeAlexandre/beamer --state open --label needs:alex --search '"Nightly triage: the routine could not run" in:title' --json number,title

If there is one, comment on it with today's date, the step that failed and its error output in a
code block. Otherwise open it with `gh issue create -R FredeAlexandre/beamer --label needs:alex
--title "Nightly triage: the routine could not run" --body-file ...`, whose body is the ask
`Alex: the nightly triage could not run. Fix the routine (its prompt is in
dev/routines/nightly-triage.md on rust), the token it uses, or the nightly it reads. Until then,
the nightly's findings reach nobody.`, then the step that failed and its error output in a code
block.

Your final message: the run you triaged (RUN and SHA), the plan's counts, the URL of every issue,
comment and pull request you posted, and anything that failed.
````
