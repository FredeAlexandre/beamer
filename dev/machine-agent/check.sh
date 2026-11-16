#!/bin/sh
# Proves #47 on the Mac: rebuilds and re-signs the agent, starts it the way the harness will, and
# through it moves the pointer and types a key, reading both back from the OS.
#
#   dev/machine-agent/check.sh
#
# Exit 0: the agent is signed "Beamer Dev", still trusted after this rebuild, and its input lands.
# Exit 2: macOS has not granted the agent Accessibility (README.md, "One-time grant").
# Exit 1: anything else.
# It moves the pointer for a moment and puts it back. The key is F19, which macOS binds to nothing.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
LOG="$HOME/Library/Logs/Beamer/machine-agent.log"

APP="$("$HERE/bundle.sh")"
AGENT="$APP/Contents/MacOS/machine-agent"
say() { printf '%s\n' "check: $*"; }
fail() { printf '%s\n' "check: FAIL: $1" >&2; exit "${2:-1}"; }
send() { "$AGENT" send "$@"; }
field() { sed -E "s/.*\"$1\":([^,}]*).*/\\1/"; }
pause() { perl -e 'select(undef, undef, undef, 0.05)'; }

authority="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)"
[ "$authority" = "Beamer Dev" ] || fail "signed by '$authority', not 'Beamer Dev'"
say "built, signed by $authority: $APP"

# launchd starts the .app, so macOS holds the agent itself responsible for its input, and asks
# for the agent's own grant (a binary run from a terminal would borrow the terminal's). `-n`,
# because LaunchServices can still see the agent bundle.sh just stopped as running.
mkdir -p "$(dirname "$LOG")"
open -n -g "$APP" --stderr "$LOG"
tries=0
until send '{"op":"clock"}' >/dev/null 2>&1; do
    tries=$((tries + 1))
    [ "$tries" -lt 100 ] || fail "the agent did not start; see $LOG"
    pause
done

hello="$(send '{"op":"hello"}')"
say "hello: $hello"
if [ "$(echo "$hello" | field trusted)" != true ]; then
    # Restarted asking for access, macOS lists the agent under Accessibility (off) and shows its
    # prompt; the agent keeps running until the next rebuild or its idle time.
    send '{"op":"quit"}' >/dev/null 2>&1 || true
    while pgrep -f "$AGENT" >/dev/null; do pause; done
    open -n -g "$APP" --stderr "$LOG" --args --ask-for-access
    fail "macOS does not trust the agent yet. In System Settings > Privacy & Security >
  Accessibility, switch \"Beamer Machine Agent\" on (README.md, \"One-time grant\"), then run me again" 2
fi

# Poll a read until it shows `want`: the OS applies a posted event within milliseconds, not at once.
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
