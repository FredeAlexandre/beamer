#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds BeamerAudio.driver (universal arm64 + x86_64) into ./build and copies it to the staging
# folder the installer (beamer-hal) picks up. Command Line Tools only; never installs anything.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
BUNDLE="$BUILD/BeamerAudio.driver"
STAGING="${BEAMER_HAL_STAGING:-$HOME/Developer/beamer-hal-staging}"
SIGN_ID="Beamer Dev"

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUILD/obj"

CFLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -fvisibility=hidden -mmacosx-version-min=12.0)
for arch in arm64 x86_64; do
    clang "${CFLAGS[@]}" -arch "$arch" -bundle \
        -framework CoreAudio -framework CoreFoundation \
        -o "$BUILD/obj/BeamerAudio.$arch" "$HERE/src/BeamerAudio.c"
done
lipo -create -output "$BUNDLE/Contents/MacOS/BeamerAudio" "$BUILD/obj/BeamerAudio.arm64" "$BUILD/obj/BeamerAudio.x86_64"
cp "$HERE/Info.plist" "$BUNDLE/Contents/Info.plist"
plutil -lint -s "$BUNDLE/Contents/Info.plist"

# Only the factory may be exported.
for arch in arm64 x86_64; do
    exports="$(nm -gU -arch "$arch" "$BUNDLE/Contents/MacOS/BeamerAudio" | awk '{print $3}')"
    [ "$exports" = "_BeamerAudio_Create" ] || { echo "unexpected $arch exports: $exports" >&2; exit 1; }
done

if security find-identity -v -p codesigning 2>/dev/null | grep -q "\"$SIGN_ID\""; then
    codesign --force --timestamp=none --sign "$SIGN_ID" "$BUNDLE"
    echo "signed with '$SIGN_ID'"
else
    codesign --force --sign - "$BUNDLE"
    echo "signed ad-hoc"
fi
codesign --verify --strict "$BUNDLE"

mkdir -p "$STAGING"
rm -rf "$STAGING/BeamerAudio.driver"
ditto "$BUNDLE" "$STAGING/BeamerAudio.driver"

echo "built  $BUNDLE ($(lipo -archs "$BUNDLE/Contents/MacOS/BeamerAudio"))"
echo "staged $STAGING/BeamerAudio.driver"
