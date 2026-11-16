#!/bin/sh
# Proves #48 from the Mac: ships this commit's machine agent to the PC, builds and installs it there
# (install.ps1), starts it through its scheduled task in the signed-in desktop session, and through
# the SSH tunnel the harness uses, moves the PC's pointer and types a key, reading both back from
# Windows.
#
#   dev/machine-agent/check-pc.sh
#
# Exit 0: the agent runs in the desktop session, Windows takes its input, and its input lands.
# Exit 2: no one is signed in at the PC's desktop, or it is locked (README.md, "On the PC").
# Exit 1: anything else.
# Only committed work is shipped, as dev/deploy.sh does, and the PC's clone is left alone. It moves
# the PC's pointer for a moment and puts it back. The key is F19, which Windows binds to nothing.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
PC="${BEAMER_PC:-lebig}"
TUNNEL_PORT="${BEAMER_PC_AGENT_PORT:-24871}"
TASK=BeamerDev-MachineAgent
SCRATCH="$(mktemp -d)"
TUNNEL=
cleanup() {
    [ -z "$TUNNEL" ] || kill "$TUNNEL" 2>/dev/null || true
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

say() { printf '%s\n' "check-pc: $*"; }
fail() { printf '%s\n' "check-pc: FAIL: $1" >&2; exit "${2:-1}"; }
field() { sed -E "s/.*\"$1\":([^,}]*).*/\\1/"; }
pause() { perl -e 'select(undef, undef, undef, 0.1)'; }
pc() { ssh -o BatchMode=yes "$PC" "$@"; }

[ -z "$(git -C "$HERE" status --porcelain -- .)" ] ||
    say "uncommitted changes in dev/machine-agent are not shipped: checking HEAD"
commit="$(git -C "$HERE" rev-parse --short HEAD)"

# Its own source directory on the PC, next to the install, keeping its target/ between runs.
# From the root: in a subdirectory, git archive takes only paths below it.
git -C "$(git -C "$HERE" rev-parse --show-toplevel)" archive --format=tar -o "$SCRATCH/agent.tar" \
    HEAD:dev/machine-agent
pc 'New-Item -ItemType Directory -Force $env:LOCALAPPDATA\BeamerDev\machine-agent-src | Out-Null'
scp -q -o BatchMode=yes "$SCRATCH/agent.tar" "$PC:AppData/Local/BeamerDev/machine-agent-src.tar"
pc '$ErrorActionPreference = "Stop"
    $src = "$env:LOCALAPPDATA\BeamerDev\machine-agent-src"
    Get-ChildItem $src -Exclude target | Remove-Item -Recurse -Force
    tar -xf "$src.tar" -C $src
    if ($LASTEXITCODE -ne 0) { throw "tar failed" }
    Remove-Item "$src.tar"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$src\install.ps1"
    if ($LASTEXITCODE -ne 0) { exit 1 }' >"$SCRATCH/install.log" 2>&1 ||
    { cat "$SCRATCH/install.log" >&2; fail "the build or install on $PC failed"; }
dir="$(tr -d '\r' <"$SCRATCH/install.log" | tail -1)"
say "built $commit on $PC, installed in $dir"

# Through the task, never over SSH, which would land it in session 0, away from the desktop.
pc "Start-ScheduledTask -TaskName $TASK"

# The client is this Mac's build of the same agent, sending through the tunnel.
export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release --quiet --manifest-path "$HERE/Cargo.toml"
AGENT="$HERE/target/release/machine-agent"
send() { "$AGENT" send --port "$TUNNEL_PORT" "$@"; }
# A connection of its own (-S none): through a shared ControlMaster, the forward would outlive
# this script, held by the master, and the master would open a shell for -N.
ssh -N -S none -o BatchMode=yes -o ExitOnForwardFailure=yes -L "$TUNNEL_PORT:127.0.0.1:24870" "$PC" </dev/null &
TUNNEL=$!

tries=0
until send '{"op":"clock"}' >/dev/null 2>&1; do
    tries=$((tries + 1))
    if [ "$tries" -ge 150 ]; then
        kill -0 "$TUNNEL" 2>/dev/null || fail "the tunnel on 127.0.0.1:$TUNNEL_PORT did not open (is the port taken?)"
        user="$(pc '(Get-CimInstance Win32_ComputerSystem).UserName' | tr -d '\r')"
        [ -n "$user" ] || fail "no one is signed in at $PC's desktop, so its task cannot start the agent.
  Sign in at the PC, then run me again" 2
        fail "the agent did not start; on $PC see Get-ScheduledTaskInfo $TASK and $dir\\machine-agent.log"
    fi
    pause
done

hello="$(send '{"op":"hello"}')"
say "hello: $hello"
[ "$(echo "$hello" | field agent)" = '"pc"' ] || fail "the agent on 127.0.0.1:$TUNNEL_PORT is not the PC's"
session="$(pc '(Get-Process machine-agent-windowless).SessionId' | tr -d '\r')"
say "the task started it in session $session"
if [ "$(echo "$hello" | field trusted)" != true ]; then
    send '{"op":"quit"}' >/dev/null 2>&1 || true
    fail "Windows does not take the agent's input: its session ($session) has no desktop, or the
  desktop is locked. Unlock the PC (README.md, \"On the PC\"), then run me again" 2
fi

# Poll a read until it shows `want`: the OS applies a posted event soon after, not at once.
until_reads() {
    request="$1" want="$2" tries=0
    until send "$request" | grep -qF "$want"; do
        tries=$((tries + 1))
        [ "$tries" -lt 40 ] || return 1
        pause
    done
}

start="$(send '{"op":"pointer"}')"
x="$(echo "$start" | field x)" y="$(echo "$start" | field y)"
send '{"op":"move","to":[123,145]}' >/dev/null
until_reads '{"op":"pointer"}' '"x":123.0,"y":145.0' || fail "the pointer did not reach (123, 145)"
say "moved the pointer to (123, 145)"
send "{\"op\":\"move\",\"to\":[$x,$y]}" >/dev/null

send '{"op":"key","key":"F19","action":"down"}' >/dev/null
until_reads '{"op":"key-state","key":"F19"}' '"down":true' || fail "F19 did not go down"
send '{"op":"key","key":"F19","action":"up"}' >/dev/null
until_reads '{"op":"key-state","key":"F19"}' '"down":false' || fail "F19 did not come back up"
say "typed F19"

send '{"op":"quit"}' >/dev/null
say "PASS"
