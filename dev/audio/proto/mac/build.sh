#!/bin/sh
# Build beamer-audio with the Command Line Tools only (no Xcode project).
# Output: .build/beamer-audio
set -eu
cd "$(dirname "$0")"
mkdir -p .build
clang -O2 -Wall -Wextra -mmacosx-version-min=14.2 -c Sources/rt.c -o .build/rt.o
swiftc -O -swift-version 5 -target "$(uname -m)-apple-macosx14.2" \
    -import-objc-header Sources/rt.h \
    Sources/main.swift Sources/CA.swift Sources/IO.swift Sources/Commands.swift Sources/Measure.swift \
    .build/rt.o \
    -framework CoreAudio -framework AudioToolbox -framework Foundation \
    -o .build/beamer-audio
echo "built $(pwd)/.build/beamer-audio"
