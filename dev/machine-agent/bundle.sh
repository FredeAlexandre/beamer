#!/bin/sh
# Builds the Mac machine agent's .app, signs it "Beamer Dev" and installs it, outside the repo and
# outside /Applications (#47, ADR 0005). It never ships: nothing that builds Beamer reads this.
#
#   dev/machine-agent/bundle.sh        prints the installed .app's path
#
# The grant survives rebuilds because macOS ties Accessibility to the bundle id and the signing
# certificate (the designated requirement), not to the binary. An ad-hoc signature would tie it
# to this one binary, so a build without the identity is refused rather than installed.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
IDENTITY="${BEAMER_SIGN_IDENTITY:-Beamer Dev}"
BUNDLE_ID="uk.co.kalkman.beamer.dev.machine-agent"
APP="$HOME/Library/Developer/Beamer/Beamer Machine Agent.app"

if ! security find-identity -v -p codesigning | grep -qF "\"$IDENTITY\""; then
    echo "bundle.sh: no \"$IDENTITY\" signing identity in the keychain; not installing an ad-hoc build" >&2
    exit 1
fi

export PATH="$HOME/.cargo/bin:$PATH"
cargo build --release --quiet --manifest-path "$HERE/Cargo.toml"

STAGE="$HERE/target/bundle/Beamer Machine Agent.app"
rm -rf "$STAGE"
mkdir -p "$STAGE/Contents/MacOS"
cp "$HERE/target/release/machine-agent" "$STAGE/Contents/MacOS/machine-agent"
# A new CFBundleVersion on every build makes every build a new binary to macOS, so a grant that
# holds after this script is one that survives rebuilds.
cat > "$STAGE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>machine-agent</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleName</key><string>Beamer Machine Agent</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>CFBundleVersion</key><string>$(date -u +%Y%m%d%H%M%S)</string>
    <key>LSMinimumSystemVersion</key><string>14.2</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign "$IDENTITY" --identifier "$BUNDLE_ID" "$STAGE" 2>/dev/null
codesign --verify --strict "$STAGE"

# A running agent keeps the old binary: stop it, and wait until it is gone, before replacing the
# .app. `open` does nothing for an app macOS still sees running.
"$STAGE/Contents/MacOS/machine-agent" send '{"op":"quit"}' >/dev/null 2>&1 || true
tries=0
while pgrep -f "$APP/Contents/MacOS/machine-agent" >/dev/null; do
    tries=$((tries + 1))
    if [ "$tries" -ge 60 ]; then
        echo "bundle.sh: the running agent did not stop" >&2
        exit 1
    fi
    perl -e 'select(undef, undef, undef, 0.05)'
done
mkdir -p "$(dirname "$APP")"
rm -rf "$APP"
ditto "$STAGE" "$APP"
echo "$APP"
