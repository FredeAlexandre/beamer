# Routine: daily spec digest

A Claude Code cloud routine that tells Alex, once a day, which rules changed on `rust`: every rule
added, bumped (`+n`) or removed since the last digest, with the pull request that changed it. The
spec reviewer approves most rule changes alone, so this is how those changes reach Alex. Built for
#45. Parity changes and `human`-tier changes need Alex's approval (`spec/README.md`, #15,
ADR 0002), so they are listed first.

## What it posts

One comment a day on the pinned issue [#118 Spec digest](https://github.com/FredeAlexandre/beamer/issues/118),
and nothing on a day that changed no rule.

`cargo xtask spec digest` (`xtask/src/digest.rs`) writes the comment; the routine posts it as
printed. The digest:

- compares the rules in `spec/*.md` at the last digest's commit with the rules at the head of
  `rust`. A rule is **added** when its id is new, **bumped** when its version went up, and
  **removed** when its id is gone. A wording fix that keeps the version is not listed, and neither
  is a rule added and removed again between two digests.
- names the pull request of every commit that changed the rule. GitHub ties rebase-merged commits
  to their pull request, so `gh api repos/…/commits/<sha>/pulls` finds it. A commit pushed straight
  to `rust` shows its hash and "no PR".
- lists under **Parity and human tier** every change to a rule that carries a `parity:` marker or
  the `human` tier, before or after the change, so a rule leaving the `human` tier is listed there
  too. **Other changes** follow. Each entry shows the rule's text and markers, linked to its line.
- fits one comment. GitHub caps a comment at 65,536 characters, and stage 0 may add hundreds of
  rules in a day. Past 60,000 characters, other changes lose their text; past that again, the list
  ends with how many rules it left out.
- ends with `<!-- spec-digest through <sha> -->`, the commit it ran through. The next digest starts
  there. #118's body holds the first one: `rust`'s head when it was opened, before any rule.

Because the next digest starts at the last posted comment, a day with no change moves nothing: the
next day's digest still covers it. A second run on the same day finds no change and posts nothing.

When the routine cannot run, it opens a `needs:alex` issue (or comments on the one already open),
because a digest that fails silently looks like a quiet spec.

## Create the routine (Alex)

Routines run on Alex's claude.ai account, so this step is his. Do it once.

1. **Open** <https://claude.ai/code/routines> and choose **New routine**. (From a Claude Code
   session, `/schedule` does the same: give it the values below.)
2. **Name**: `Beamer: daily spec digest`.
3. **Repository**: `FredeAlexandre/beamer`. The routine reads `rust` and comments on issues; it
   never pushes. The Claude GitHub app must be installed on that repository.
4. **Schedule**: daily at 07:00 local time. As cron: `0 7 * * *`.
5. **Prompt**: paste everything inside the block under [The prompt](#the-prompt), from
   `You are the daily spec digest` to the last line, without the outer four-backtick fence.
6. **Connectors**: remove them all. The routine needs only `gh`, `git` and `cargo`.
7. **Environment**:
   - Network access must reach `github.com`, `api.github.com`, `static.rust-lang.org` (rustup
     installs the pinned toolchain) and crates.io (`index.crates.io`, `static.crates.io`). The
     default trusted allowlist covers them. A cold run downloads about 300 MB: the toolchain, the
     crates `xtask` builds with, and the Rune mirror the workspace patches from.
   - The routine runs `gh`. If the environment has no authenticated `gh`, add an environment
     variable `GH_TOKEN`: a fine-grained token with **Issues: read and write** and **Contents:
     read** on `FredeAlexandre/beamer`. It goes in the routine's environment, never in repository
     secrets (#22).
8. **Save**, then **Run now** once. Expected while `spec/` has no rules: the run log ends with
   `No rule added, bumped or removed in …`, and nothing is posted on #118. If the log shows `gh`
   failing to authenticate, do step 7's `GH_TOKEN` and run it again.
9. Comment the routine's claude.ai link on #45, so the coordinator can close it.

The comments are posted as you, and GitHub never notifies you of your own comments: read the
digest on the pinned #118, or from the repository's Issues page, where it is pinned.

To change what the digest lists or how, edit `xtask/src/digest.rs` and its tests
(`xtask/tests/spec_digest.rs`): the routine runs whatever `rust` holds. To change the steps around
it, edit this file and the routine's prompt in the same sitting: the routine runs the copy pasted
into it, not this file.

## The prompt

````text
You are the daily spec digest for the Beamer 2.0 build (FredeAlexandre/beamer).

Beamer's Rust rewrite lives on the `rust` branch; what Beamer does is written as rules in its
`spec/*.md` files (spec/README.md). Your one job: post, on the pinned issue #118 "Spec digest",
the digest of rules added, bumped or removed on `rust` since the last digest, and nothing when no
rule changed. `cargo xtask spec digest` writes the digest; you post it as printed. You never change
code, open PRs, push, or post anywhere but the issues named here. Every `gh` call carries
`-R FredeAlexandre/beamer`. Never write to kalkman-code/beamer.

## 1. Make sure gh works

Run `gh auth status`. If `gh` is missing, install it (on Debian/Ubuntu: `apt-get install -y gh`,
or the official GitHub CLI apt repository). If it is not authenticated and GH_TOKEN is unset,
stop and report that in your final message; you cannot post anything.

## 2. Check out rust

Your checkout starts on the default branch, `main`, which is not the one you read. Run, from the
repository root:

    git fetch origin rust
    if [ "$(git rev-parse --is-shallow-repository)" = true ]; then git fetch --unshallow origin rust; fi
    git checkout --detach origin/rust

If `cargo` is missing, install rustup with no default toolchain and load it:

    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain none
    . "$HOME/.cargo/env"

The checkout's rust-toolchain.toml makes rustup install the pinned toolchain on first use.

## 3. Find where the last digest ended

    SINCE=$( { gh issue view 118 -R FredeAlexandre/beamer --json body --jq .body; gh api --paginate repos/FredeAlexandre/beamer/issues/118/comments --jq '.[].body'; } | grep -oE '<!-- spec-digest through [0-9a-f]{40} -->' | tail -n 1 | grep -oE '[0-9a-f]{40}')

It is the commit named on the last line of the last digest, or in #118's body before the first
digest. If SINCE is empty, the routine cannot run: go to step 5.

## 4. Run the digest

    cargo xtask spec digest --since "$SINCE" --until origin/rust --repo FredeAlexandre/beamer > /tmp/spec-digest.md

**It exits 0 and /tmp/spec-digest.md is empty**: no rule changed. Stop. Post nothing: no comment,
no issue, no label. Your final message is the line it printed on stderr.

**It exits 0 and /tmp/spec-digest.md is not empty**: post it, unchanged, as one comment:

    gh issue comment 118 -R FredeAlexandre/beamer --body-file /tmp/spec-digest.md

Do not edit, summarise, reorder or add to it: its order is the point (parity and human-tier
changes first), and its last line is where tomorrow's digest starts.

**It exits non-zero**: run step 4 once more. If it fails again, go to step 5.

## 5. When the digest cannot run

The digest cannot see, which Alex must know: a silent failure looks like a quiet spec. Look for an
open issue titled `Spec digest: the routine could not run`:

    gh issue list -R FredeAlexandre/beamer --state open --label needs:alex --search '"Spec digest: the routine could not run" in:title' --json number,title

If there is one, comment on it with today's date and the error output in a code block. Otherwise
open it with `gh issue create -R FredeAlexandre/beamer --label needs:alex --title "Spec digest: the routine could not run" --body-file ...`,
whose body is the ask `Alex: the daily spec digest could not run. Fix the routine (its prompt is in
dev/routines/spec-digest.md on rust) or the token it uses. Until then, rule changes on rust reach
nobody.`, then the step that failed and its error output in a code block. Never post the error on
#118.

If the error says the last digest's commit "is not an ancestor" of origin/rust, `rust` was
rewritten after the last digest: say so in that issue. Alex decides where the next digest starts,
by posting a comment on #118 whose last line is `<!-- spec-digest through <sha> -->`.

Your final message: what the digest printed, and the URL of any comment or issue you posted.
````
