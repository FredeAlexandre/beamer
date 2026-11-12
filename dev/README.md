# Two-machine development (fork only)

This fork is developed on two machines at once, so features that cross the network (audio and
microphone following input, next) can be built and tested end to end.

| | Mac (hub) | Windows PC |
|---|---|---|
| Name | `alexandres-macbook-air` | `lebig` (Tailscale MagicDNS) |
| Clone | `~/Repositories/github.com/FredeAlexandre/beamer` | `C:\Users\Alex\Developer\beamer` |
| Python | `mac_app/.venv` from the python.org universal2 framework (pinned in `mise.local.toml`) | `win_app\.venv`, made with `uv venv --seed --python 3.13` |
| Installed app | `/Applications/Beamer.app`, signed "Beamer Dev" | `%LOCALAPPDATA%\Beamer\Beamer.exe` |
| Log | `~/Library/Logs/Beamer/Beamer.log` | `%LOCALAPPDATA%\Beamer\Beamer.log` |

The Mac drives everything. The PC is reached with `ssh lebig` (PowerShell, key only, tailnet only);
the PC does not reach back into the Mac.

## Running things on the PC

```sh
ssh lebig 'Set-Location $HOME\Developer\beamer; git status'
ssh lebig 'Get-Content $env:LOCALAPPDATA\Beamer\Beamer.log -Tail 50'
ssh lebig 'Set-Location $HOME\Developer\beamer\win_app; .\.venv\Scripts\python.exe -m unittest discover -s tests -v'
```

Anything started directly over SSH runs in session 0, with no desktop: never `Start-Process` Beamer.
Start it through its scheduled task (`Get-ScheduledTask Beamer*`), as `dev/deploy.sh` does.

An agent can work on the PC with its own context:

```sh
ssh lebig 'Set-Location $HOME\Developer\beamer; claude -p "<task>"'
```

## Moving code between the machines

The PC clone is a git remote of the Mac clone, named `pc`, with `receive.denyCurrentBranch
updateInstead` on the PC, so a push updates its working tree directly:

```sh
git push pc my-branch        # Mac -> PC (refused if the PC's working tree is dirty)
git fetch pc                 # PC -> Mac, then merge or cherry-pick pc/my-branch
```

GitHub (`origin`, the fork) is for sharing, not for the inner loop.

## Deploying the same build to both

```sh
dev/deploy.sh          # push HEAD to the PC, build + install on both in parallel, start both
dev/deploy.sh mac      # or one side only
dev/deploy.sh pc
```

Only committed work is deployed, so both machines always run the same commit. Build logs land in
`dev/.logs/`. The Windows build skips its test suite (`BEAMER_SKIP_TESTS=1`) unless you pass
`--test`, because the suite takes about 20 minutes and has failures in this setup that are not
ours (below).

## Known Windows test failures here (not caused by the fork)

Running the suite on the PC (`--test`) gives 1 failure and 6 errors, all environmental:

- `test_profiles_migration` and `test_tile_resize` (4 errors) `git archive` commits `be9661d1` and
  `2dbe44ab`, which exist only in the maintainer's private history, not in the public repository.
- `test_admin_mode` and `test_admin_mode_ui` (2 errors) fail to import `core` under
  `unittest discover -s tests`.
- `test_a_shifted_digit_gives_the_symbol_the_layout_prints` assumes a US layout; the PC is French
  AZERTY, where VK_1 types `&`.

Treat anything else as a real failure.

## Branches

- `main` mirrors `upstream/main` (kalkman-code/beamer). Never commit to it.
- `dev/local-build` holds the fixes this setup needs (python.org 3.13.17 compileall, self-signed
  signing identity) plus these dev tools. Feature branches start from it.
- To offer a change upstream, cherry-pick it onto a branch from `upstream/main`, sign off every
  commit (`git commit -s`), and open an issue first for anything non-trivial (see CONTRIBUTING.md).

## Testing limits

Pointer crossing, key feel and anything heard or spoken need a person at the machines. Agents can
build, run unit tests, deploy, start the apps and read both logs.
