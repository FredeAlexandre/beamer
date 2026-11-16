# Two-machine development (fork only)
<!-- Throwaway line from #34: proves auto-merge; the next PR reverts it. -->

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

- `main` is the fork's own line: upstream (kalkman-code/beamer) plus this setup's fixes, the dev
  tools and the finished features (audio and microphone, camera; merged 2026-11-12; shared
  displays, merged 2026-11-13). It no longer mirrors `upstream/main`: take upstream releases with
  `git merge upstream/main`. Feature branches start from `main` and come back to it once Alex
  calls them done; the branch is then deleted (Mac, PC and `origin`) and its tip tagged
  `archive/<branch>`.
- `main` is the only branch. Finished branches live on as tags: `archive/feat-audio`,
  `archive/feat-camera`, `archive/feat-shared-display`, and `archive/dev-local-build` (the fixes
  this setup needs: python.org 3.13.17 compileall, self-signed signing identity, plus the dev
  tools). Recreate one with `git branch <name> archive/<name>` if ever needed.
- To offer a change upstream, cherry-pick it onto a branch from `upstream/main`, sign off every
  commit (`git commit -s`), and open an issue first for anything non-trivial (see CONTRIBUTING.md).

## Machine agents

The `machine` test tier posts real input on both machines through a dev-only machine agent, never
through a hook inside Beamer (ADR 0005). Both are in [`machine-agent/`](machine-agent/README.md),
with the protocol they speak in [`machine-agent/PROTOCOL.md`](machine-agent/PROTOCOL.md).
`dev/machine-agent/check.sh` proves the Mac's works. `dev/machine-agent/check-pc.sh`, run on the
Mac, proves the PC's: it installs the agent in `%LOCALAPPDATA%\BeamerDev\`, outside Beamer's
install, and starts it in the desktop session through the `BeamerDev-MachineAgent` scheduled task.

## Testing limits

Pointer crossing, key feel and anything heard or spoken need a person at the machines. Agents can
build, run unit tests, deploy, start the apps and read both logs.

## Shared sccache on the Mac

The Beamer 2.0 build (#35) gives every worktree on the Mac one sccache disk cache. The rules
agents follow are in `CLAUDE.md` on `rust`, under "Building on the Mac". It was set up on
2026-11-16 as follows. Neither config file existed before. If one does, add the keys to it
instead of overwriting it:

```sh
brew install sccache                     # 0.19.0 at setup
mkdir -p "$HOME/Library/Application Support/Mozilla.sccache"
cat > "$HOME/Library/Application Support/Mozilla.sccache/config" <<TOML
[cache.disk]
dir = "$HOME/Library/Caches/Mozilla.sccache"
size = 53687091200  # 50 GiB
TOML
cat > "$HOME/.cargo/config.toml" <<'TOML'
[build]
rustc-wrapper = "/opt/homebrew/bin/sccache"
TOML
sccache --stop-server; sccache --start-server   # restart to read the config; not mid-build
sccache --show-stats                            # "Max cache size 50 GiB", the cache dir above
```

`~/.cargo/config.toml` applies to every cargo project on the Mac, not only Beamer. To undo the
setup, delete that file and the sccache config, run `sccache --stop-server` and delete the cache
dir.

Measured at setup with the workspace of the time (`xtask` and its dependencies), by running
`cargo build --workspace --all-targets` in a fresh worktree after the same build in another:
38 cache hits and 6 misses. `serde_core` and `serde_json` missed because they `include!` a
file from their `OUT_DIR`, and `serde_spanned`, `toml` and `toml_datetime` missed because they're
downstream of `serde_core`. `clap` missed because it uses the `clap_derive` proc-macro, which is
built in each worktree. After `cargo clean`, a rebuild in the *same* worktree got 44 hits and
0 misses.
