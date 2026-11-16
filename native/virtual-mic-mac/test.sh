#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# One-command test: builds the driver (build.sh), then drives it out of process with test_host:
#   1. the signed universal bundle, native arch (and x86_64 under Rosetta when available)
#   2. an AddressSanitizer + UndefinedBehaviorSanitizer build of the same source
#   3. a ThreadSanitizer build (the loopback runs on two threads, like coreaudiod's IO threads)
# Nothing is installed; coreaudiod is not involved.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
"$HERE/build.sh"

CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -mmacosx-version-min=12.0 -framework CoreAudio -framework CoreFoundation)
clang "${CFLAGS[@]}" -arch arm64 -arch x86_64 -o "$BUILD/test_host" "$HERE/test/test_host.c"

status=0
run() { echo; echo "### $1"; shift; "$@" || status=1; }

run "signed bundle, native" "$BUILD/test_host" "$BUILD/BeamerAudio.driver"
if [ "$(uname -m)" = x86_64 ] || [ -e /Library/Apple/usr/libexec/oah/libRosettaRuntime ]; then
    run "signed bundle, x86_64 (Rosetta)" arch -x86_64 "$BUILD/test_host" "$BUILD/BeamerAudio.driver"
else
    echo; echo "### x86_64 slice: skipped (Rosetta not installed); slice verified by lipo/nm only"
fi

for san in address,undefined thread; do
    dir="$BUILD/san-${san%%,*}"
    rm -rf "$dir"; mkdir -p "$dir/BeamerAudio.driver/Contents/MacOS"
    cp "$HERE/Info.plist" "$dir/BeamerAudio.driver/Contents/Info.plist"
    clang "${CFLAGS[@]}" -fsanitize=$san -fno-omit-frame-pointer -bundle \
        -o "$dir/BeamerAudio.driver/Contents/MacOS/BeamerAudio" "$HERE/src/BeamerAudio.c"
    clang "${CFLAGS[@]}" -fsanitize=$san -fno-omit-frame-pointer -o "$dir/test_host" "$HERE/test/test_host.c"
    run "sanitizer: $san" env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        TSAN_OPTIONS=halt_on_error=1 "$dir/test_host" "$dir/BeamerAudio.driver"
done

echo
[ $status -eq 0 ] && echo "ALL TEST RUNS PASSED" || echo "SOME TEST RUNS FAILED"
exit $status
