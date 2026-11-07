# Audio across Beamer: status board

Operator's working memory for the audio/microphone mission (Mac <-> PC). Updated at every
significant step. Branch: `feat/audio` (from `dev/local-build`).

## En attente d'Alex

1. **Mac: the `beamer-hal` root helper is missing** (start check 1 failed, 2026-11-07 14:51).
   `/usr/local/libexec/beamer-hal` does not exist and `sudo -n -l /usr/local/libexec/beamer-hal install`
   asks for a password, so there is no sudoers rule either. Needed: the helper script plus a
   NOPASSWD rule for `beamer-hal install|uninstall|restart`. Unblocks: installing
   `BeamerAudio.driver` (the remote mic as a real input on the Mac). Everything else continues;
   the driver is built and tested as far as possible without installing it.

## Phase

**Phase 1: prototyping and technical proof** (`dev/audio/proto/`, standalone programs, no change
to the Beamer app).

## Start checks (2026-11-07)

| # | Check | Result |
|---|---|---|
| 1 | `sudo -n -l /usr/local/libexec/beamer-hal install` | FAIL: helper absent, no sudo rule |
| 2 | `ssh -o BatchMode=yes lebig whoami` | OK `lebig\alex` |
| 3 | PC clone clean on `dev/local-build`, `git fetch pc` | OK |
| 4 | Claude agent on the PC (`claude -p`) | OK (answers in ~4 s) |
| 5 | `dev/deploy.sh` both, Beamer running | OK (57 s; Mac pid up, PC session 1 via task; links Mac<->PC and PC<->asusrog up) |
| 6 | Windows Update paused, caffeinate | OK (paused until 2026-11-21T13:48Z; `caffeinate -dimsu` running) |

## Tasks

| Task | Owner | State |
|---|---|---|

## Decisions

## Measurements

## Open questions
