# Dispatching a task

Every agent starts with a fresh context, in its own worktree on `build/<n>-<slug>` cut from
`origin/rust`. You create the worktree; the agent works only inside it. Pick `<slug>` from the
issue title: a few lowercase words joined by hyphens.

## The brief

Fill in the angle brackets and keep it this short: the issue, `CLAUDE.md` and the skill carry
everything else.

```
Build task #<n> of FredeAlexandre/beamer. You are on <the Mac | lebig>, in the worktree <path>,
on branch build/<n>-<slug>, cut from origin/rust. Work only in that worktree.

1. Read CLAUDE.md there, then the issue: gh issue view <n> -R FredeAlexandre/beamer
2. <Mac: Claim the issue as CLAUDE.md says, naming the branch and the Mac. | lebig: The
   coordinator has claimed the issue for you; gh is not logged in here, so make no gh call.>
3. <Run the build-<role> skill. | There is no role skill for this task. Meet every acceptance
   criterion in the issue, test-first with the tdd skill where you write xtask code.>
4. <Mac: Finish as the skill says. Without a skill: push the branch, open a PR into rust
   (gh pr create -R FredeAlexandre/beamer --base rust) whose body names #<n>, and, once #34 is
   closed, turn on auto-merge (gh pr merge <pr> --auto --rebase). | lebig: Commit everything on
   build/<n>-<slug> and push nothing; the coordinator opens the PR from the Mac.>
5. Report: the PR URL (or the last commit on lebig), and how you checked each acceptance
   criterion. Leave merging and closing the issue to the coordinator.
```

## On the Mac

```sh
git -C <your rust worktree> worktree add ../beamer-<n> -b build/<n>-<slug> origin/rust
```

Then start a background subagent with the brief. It claims the issue itself.

## On lebig

`gh` is not logged in there, so you do its GitHub steps from the Mac. Claim the issue for the
agent first:

```sh
gh issue edit <n> -R FredeAlexandre/beamer --add-assignee @me
gh issue comment <n> -R FredeAlexandre/beamer --body 'Claimed: branch `build/<n>-<slug>`, on lebig.'
ssh lebig 'Set-Location $HOME\Developer\beamer; git fetch origin; git worktree add ..\beamer-<n> -b build/<n>-<slug> origin/rust'
```

Write the brief to a file and pipe it in. Passing it as an argument mangles its quotes in
PowerShell. The agent runs unattended, so it needs a permission mode: `claude -p` alone denies
every tool call that would prompt. Run it as a background command; an SSH command runs in session
0, which builds and tests fine but cannot start Beamer (see `dev/README.md`).

```sh
scp brief.md lebig:Developer/beamer-<n>.brief.md
ssh lebig 'Set-Location $HOME\Developer\beamer-<n>; Get-Content -Raw ..\beamer-<n>.brief.md | claude -p --permission-mode bypassPermissions'
```

When it reports, bring its branch to GitHub and open the PR:

```sh
git fetch pc build/<n>-<slug>
git push origin pc/build/<n>-<slug>:refs/heads/build/<n>-<slug>
gh pr create -R FredeAlexandre/beamer --base rust --head build/<n>-<slug> --title '<issue title> (#<n>)' --body '<summary; names #<n>>'
```

After the merge, clean up on the PC as well:

```sh
ssh lebig 'Set-Location $HOME\Developer\beamer; git worktree remove ..\beamer-<n>; git branch -D build/<n>-<slug>; Remove-Item ..\beamer-<n>.brief.md'
```
