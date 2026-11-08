#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# install-driver.sh: installs the BeamerAudio.driver that sits beside this script into Core Audio
# (/Library/Audio/Plug-Ins/HAL) and restarts coreaudiod (PHASE2 §10.1).
#
# Shipped as Beamer.app/Contents/Resources/audio/install-driver.sh. Beamer runs it as root through
# the macOS administrator prompt (mac_app/audio_driver.py):
#   /usr/bin/osascript -e 'do shell script "/bin/bash " & quoted form of "<this file>" with administrator privileges'
# Dev Macs use /usr/local/libexec/beamer-hal instead; the checks here are the same as there.
#
#   install-driver.sh           install (root)
#   install-driver.sh --check   only verify the bundle beside it and print its version (no root)
#
# Takes no path arguments and ignores the environment on purpose: what it installs is fixed by
# where it is.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SOURCE="$HERE/BeamerAudio.driver"
DEST_DIR=/Library/Audio/Plug-Ins/HAL
DEST="$DEST_DIR/BeamerAudio.driver"
BUNDLE_ID=com.beamer.audio.driver

die() { echo "install-driver: $*" >&2; exit 1; }

team_of() {
    codesign -dv "$1" 2>&1 | sed -n 's/^TeamIdentifier=//p' | grep -v '^not set$' || true
}

# verify <bundle>: the checks beamer-hal makes, plus: inside a signed Beamer.app with a Team ID,
# the driver must carry the same Team ID.
verify() {
    local bundle="$1" id app team_app team_driver
    [ -z "$(find "$bundle" -type l -print -quit)" ] || die "bundle contains symlinks; refused"
    [ -f "$bundle/Contents/Info.plist" ] && [ -f "$bundle/Contents/MacOS/BeamerAudio" ] || die "bundle is incomplete"
    id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$bundle/Contents/Info.plist" 2>/dev/null || true)"
    [ "$id" = "$BUNDLE_ID" ] || die "unexpected bundle identifier '$id'"
    codesign --verify --strict -R="identifier \"$BUNDLE_ID\"" "$bundle" || die "signature check failed"
    app="$(cd "$HERE/../../.." && pwd -P)"
    if [ "${app##*.}" = app ]; then
        team_app="$(team_of "$app")"
        team_driver="$(team_of "$bundle")"
        [ -z "$team_app" ] || [ "$team_app" = "$team_driver" ] \
            || die "driver Team ID '$team_driver' differs from Beamer's '$team_app'"
    fi
}

restart_coreaudiod() {
    # launchd relaunches coreaudiod on demand; launchctl kickstart is refused under SIP.
    killall coreaudiod 2>/dev/null || true
    for _ in $(seq 1 50); do
        pgrep -x coreaudiod >/dev/null && { echo "coreaudiod restarted"; return 0; }
        sleep 0.1
    done
    # Any Core Audio client brings it back.
    system_profiler SPAudioDataType >/dev/null 2>&1 || true
    pgrep -x coreaudiod >/dev/null && echo "coreaudiod restarted" \
        || echo "install-driver: coreaudiod not running yet (launchd starts it on demand)" >&2
}

[ $# -le 1 ] || die "usage: install-driver.sh [--check]"
[ -d "$SOURCE" ] && [ ! -L "$SOURCE" ] || die "no BeamerAudio.driver beside $0"

if [ "${1:-}" = --check ]; then
    verify "$SOURCE"
    echo "ok $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$SOURCE/Contents/Info.plist")"
    exit 0
fi
[ $# -eq 0 ] || die "usage: install-driver.sh [--check]"
[ "$(id -u)" -eq 0 ] || die "needs root (Beamer runs it through the administrator prompt)"

# Copy into a root-only temp dir first, then check the copy (not the user-writable source).
tmp="$(mktemp -d /private/var/tmp/beamer-driver.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
ditto --norsrc "$SOURCE" "$tmp/BeamerAudio.driver"
new="$tmp/BeamerAudio.driver"
verify "$new"

chown -R root:wheel "$new"
chmod -R u+rwX,go+rX,go-w "$new"
xattr -cr "$new" 2>/dev/null || true

mkdir -p "$DEST_DIR"
rm -rf "$DEST"
mv "$new" "$DEST"
echo "installed $DEST ($(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$DEST/Contents/Info.plist"))"
restart_coreaudiod
