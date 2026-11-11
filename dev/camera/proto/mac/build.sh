#!/bin/sh
# Build beamer-camera with the Command Line Tools only (no Xcode project).
# Output: build/beamer-camera (gitignored). Universal unless ARCHS is set (e.g. ARCHS=arm64).
set -eu
cd "$(dirname "$0")"
mkdir -p build
ARCHS="${ARCHS:-arm64 x86_64}"
AFLAGS=""
for a in $ARCHS; do AFLAGS="$AFLAGS -arch $a"; done
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unguarded-availability-new -fobjc-arc -mmacosx-version-min=14.0 $AFLAGS \
    src/main.mm src/synth.mm src/encode.mm src/capture.mm \
    -framework Foundation -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox \
    -o build/beamer-camera
# Ad-hoc sign with the camera entitlement and an embedded Info.plist (usage string), as the eventual
# helper would be; harmless for a dev build.
codesign --force --sign - --options runtime --entitlements entitlements.plist build/beamer-camera 2>/dev/null || \
    codesign --force --sign - build/beamer-camera
echo "built $(pwd)/build/beamer-camera"
