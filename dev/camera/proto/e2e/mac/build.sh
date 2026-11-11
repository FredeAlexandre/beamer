#!/bin/sh
# Build camsend (C4 Mac sender) with the Command Line Tools: camsend.mm + C1's synthetic source.
set -eu
cd "$(dirname "$0")"
mkdir -p build
clang++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-unguarded-availability-new -fobjc-arc -mmacosx-version-min=14.0 -arch arm64 \
    camsend.mm ../../mac/src/synth.mm \
    -framework Foundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox \
    -o build/camsend
codesign --force --sign - build/camsend
echo "built $(pwd)/build/camsend"
