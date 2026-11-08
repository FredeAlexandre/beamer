#!/bin/bash
# Build and install the same commit of Beamer on this Mac and on the Windows PC, then start both.
#
#   dev/deploy.sh            both machines
#   dev/deploy.sh mac        this Mac only
#   dev/deploy.sh pc         the PC only
#   dev/deploy.sh --test ... also run the Windows test suite first (about 20 minutes; the Mac
#                            build runs none). Without it the PC build skips the suite.
#
# Assumes the two-machine setup in dev/README.md: the PC is reachable as `ssh $BEAMER_PC`
# (default lebig), its clone is ~/Developer/beamer with receive.denyCurrentBranch=updateInstead, and
# this clone has a `pc` remote pointing at it. Only committed work is deployed, so both machines
# always run the exact same commit.
set -euo pipefail

RUN_TESTS=0
if [ "${1:-}" = --test ]; then RUN_TESTS=1; shift; fi
TARGET="${1:-both}"
PC="${BEAMER_PC:-lebig}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="$ROOT/dev/.logs"
mkdir -p "$LOG_DIR"
cd "$ROOT"

case "$TARGET" in both|mac|pc) ;; *) echo "usage: $0 [both|mac|pc]" >&2; exit 2 ;; esac

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "Uncommitted changes: commit them first, so the Mac and the PC build the same thing." >&2
    exit 1
fi
BRANCH="$(git symbolic-ref --short HEAD)"
COMMIT="$(git rev-parse --short HEAD)"
echo "Deploying $BRANCH @ $COMMIT to: $TARGET"

deploy_mac() {
    # mise.local.toml (git-excluded) holds the python.org Python and the "Beamer Dev" identity. A
    # worktree has none: copy the main clone's, and refuse to build ad-hoc, because an ad-hoc
    # signature makes macOS drop Beamer's privacy grants (Accessibility, Microphone, ...). Both
    # checks run before Beamer is quit.
    local main identity
    main="$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')"
    if [ ! -f "$ROOT/mise.local.toml" ] && [ -f "$main/mise.local.toml" ]; then
        cp "$main/mise.local.toml" "$ROOT/mise.local.toml"
        echo "mac: copied mise.local.toml from $main"
    fi
    identity="$(cd mac_app && mise exec -- printenv BEAMER_SIGN_IDENTITY 2>/dev/null || true)"
    if [ -z "$identity" ] || ! security find-identity -v -p codesigning | grep -qF "\"$identity\""; then
        echo "mac: no signing identity (BEAMER_SIGN_IDENTITY='$identity'); not deploying an ad-hoc build" >&2
        return 1
    fi
    osascript -e 'quit app "Beamer"' 2>/dev/null || true
    (cd mac_app && mise exec -- ./Install-Beamer-Mac.command </dev/null) > "$LOG_DIR/mac.log" 2>&1
    open -a /Applications/Beamer.app
    echo "mac: installed and started ($COMMIT)"
}

deploy_pc() {
    # Not forced: if an agent on the PC committed to this branch, fetch and merge those first. A
    # dirty PC working tree also refuses the push (updateInstead), so nothing there is overwritten.
    git push -q pc "HEAD:refs/heads/$BRANCH"
    ssh "$PC" "Set-Location \$HOME\\Developer\\beamer; git switch -q '$BRANCH'; if ((git rev-parse --short HEAD) -ne '$COMMIT') { 'PC clone is not at $COMMIT'; exit 1 }"
    local skip=1
    [ "$RUN_TESTS" -eq 1 ] && skip=0
    ssh "$PC" "\$env:BEAMER_SKIP_TESTS='$skip'"'
        Set-Location $HOME\Developer\beamer\win_app
        powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Beamer.ps1 *>&1
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        # Over SSH the installer cannot start Beamer: anything launched here lands in session 0.
        # The autostart task runs in the signed-in desktop session instead.
        # Beamer-<host>-<user> runs Beamer.exe; other Beamer-* tasks (GameInput recovery) do not.
        $task = Get-ScheduledTask -EA SilentlyContinue | Where-Object { $_.Actions.Execute -like "*\Beamer.exe" } | Select-Object -First 1
        if ($task) { Start-ScheduledTask -TaskName $task.TaskName; "started via task $($task.TaskName)" }
        else { "no Beamer task yet: start Beamer once at the PC and turn on Start at sign-in" }' \
        > "$LOG_DIR/pc.log" 2>&1
    echo "pc: installed ($COMMIT) - $(tail -1 "$LOG_DIR/pc.log")"
}

status=0
pids=()
[ "$TARGET" != mac ] && { deploy_pc & pids+=($!); }
[ "$TARGET" != pc ] && { deploy_mac & pids+=($!); }
for pid in "${pids[@]}"; do wait "$pid" || status=1; done
[ "$status" -eq 0 ] || echo "A build failed: see $LOG_DIR/mac.log and $LOG_DIR/pc.log" >&2
exit "$status"
