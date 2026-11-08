#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# One-time setup, run as root: installs /usr/local/libexec/beamer-hal and its sudoers rule.
#   sudo ./setup-beamer-hal.sh <user> <staged bundle path>
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
[ $# -eq 2 ] || { echo "usage: $0 <user> <staged BeamerAudio.driver path>" >&2; exit 1; }
USER_NAME="$1"; STAGED="$2"
id "$USER_NAME" >/dev/null
case "$STAGED" in /*) ;; *) echo "staged path must be absolute" >&2; exit 1 ;; esac
case "$STAGED" in *'"'*|*'$'*|*'`'*|*'\'*) echo "bad characters in staged path" >&2; exit 1 ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
HELPER=/usr/local/libexec/beamer-hal

install -d -o root -g wheel -m 755 /usr/local /usr/local/libexec
tmp="$(mktemp /usr/local/libexec/.beamer-hal.XXXXXX)"
sed "s|__STAGED__|$STAGED|" "$HERE/beamer-hal" > "$tmp"
chown root:wheel "$tmp"; chmod 755 "$tmp"
bash -n "$tmp"
mv -f "$tmp" "$HELPER"

rule="$(mktemp /private/var/tmp/beamer-hal-sudoers.XXXXXX)"
cat > "$rule" <<RULE
# beamer-hal: install/uninstall/restart the Beamer Core Audio plug-in without a password.
$USER_NAME ALL=(root) NOPASSWD: $HELPER install, $HELPER uninstall, $HELPER restart
RULE
visudo -cf "$rule"
chown root:wheel "$rule"; chmod 440 "$rule"
mv -f "$rule" /etc/sudoers.d/beamer-hal
visudo -c >/dev/null
echo "installed $HELPER and /etc/sudoers.d/beamer-hal for $USER_NAME"
