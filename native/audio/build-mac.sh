#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds the macOS audio helper and the virtual-mic driver (PHASE2 §2.3, §10.1). Command Line Tools
# only (clang, lipo, codesign); no Xcode project, no CMake.
#
#   native/audio/build-mac.sh [--no-driver] [--jobs N]
#
# Outputs (native/audio/build/ is git-ignored):
#   build/mac/beamer-audio          universal (arm64 + x86_64) helper: engine + Mac backend
#   build/mac/backend_check         universal test harness for the Mac backend alone (not shipped)
#   build/mac/BeamerAudio.driver    the HAL plug-in (mac-driver/build.sh, which also stages it in
#                                   ~/Developer/beamer-hal-staging for beamer-hal)
# Objects are cached per arch under build/obj/<arch>/ and rebuilt only when the source or one of the
# headers it includes is newer (clang -MMD dependency files), or when the flags change.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
OUT="$BUILD/mac"
OBJ="$BUILD/obj"
ARCHS=(arm64 x86_64)
MIN=13.0
ENGINE="$HERE/engine"
TP="$HERE/third_party"
OPUS="$TP/opus-1.6.1"
MONO="$TP/monocypher-4.0.2"
JSON="$TP/nlohmann-json-3.11.3"
FRAMEWORKS=(-framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework Foundation
            -framework AVFoundation)

ROOT="$(cd "$HERE/../.." && pwd)"
# No home-folder path may end up in a binary (the app build refuses them): __FILE__ and debug info
# are made repo-relative, and the link strips the debug map (-Wl,-S).
COMMON=(-O2 -g -ffile-prefix-map="$ROOT/"= -mmacosx-version-min=$MIN -fvisibility=hidden -Wall -Wextra)
OPUS_FLAGS=(-std=c11 -w -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF -I"$OPUS/include" -I"$OPUS/celt" -I"$OPUS/silk"
            -I"$OPUS/silk/float" -I"$OPUS/src" -I"$OPUS")
C_FLAGS=(-std=c11 -I"$MONO/src")
CXX_FLAGS=(-std=c++17 -I"$ENGINE" -I"$HERE" -I"$OPUS/include" -I"$MONO/src" -I"$JSON/include" -I"$JSON/single_include")
MM_FLAGS=(-std=c++17 -fobjc-arc -I"$HERE" -Werror=unguarded-availability-new -Werror=return-type)

# ---- one compile, run by xargs: --cc <group> <arch> <src> <obj> ----
if [ "${1:-}" = "--cc" ]; then
    group="$2"; arch="$3"; src="$4"; obj="$5"
    case "$group" in
        opus) flags=("${COMMON[@]}" "${OPUS_FLAGS[@]}"); cc=clang ;;
        c)    flags=("${COMMON[@]}" "${C_FLAGS[@]}"); cc=clang ;;
        cxx)  flags=("${COMMON[@]}" "${CXX_FLAGS[@]}"); cc=clang++ ;;
        mm)   flags=("${COMMON[@]}" "${CXX_FLAGS[@]}" "${MM_FLAGS[@]}"); cc=clang++ ;;
        *) echo "unknown group $group" >&2; exit 1 ;;
    esac
    mkdir -p "$(dirname "$obj")"
    exec "$cc" -arch "$arch" "${flags[@]}" -MMD -MF "$obj.d" -c "$src" -o "$obj"
fi

DRIVER=1
JOBS="$(sysctl -n hw.ncpu)"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-driver) DRIVER=0 ;;
        --jobs) JOBS="$2"; shift ;;
        *) echo "usage: $0 [--no-driver] [--jobs N]" >&2; exit 2 ;;
    esac
    shift
done

command -v clang++ >/dev/null || { echo "clang++ not found (install the Command Line Tools)" >&2; exit 1; }
mkdir -p "$OUT"

# A changed flag set invalidates every object.
stamp="$(printf '%s\n' "${COMMON[@]}" "${OPUS_FLAGS[@]}" "${C_FLAGS[@]}" "${CXX_FLAGS[@]}" "${MM_FLAGS[@]}" | shasum | cut -c1-16)"
if [ "$(cat "$OBJ/flags.stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$OBJ"
    mkdir -p "$OBJ"
    echo "$stamp" > "$OBJ/flags.stamp"
fi

# obj needs a rebuild when it is missing or older than its source or any dependency in its .d file
stale() {
    local obj="$1" src="$2" dep
    [ -f "$obj" ] && [ -f "$obj.d" ] || return 0
    [ "$src" -nt "$obj" ] && return 0
    for dep in $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d"); do
        [ "$dep" -nt "$obj" ] && return 0
    done
    return 1
}

# Opus source list from its own automake fragments (float build, no intrinsics).
opus_sources() {
    local mk var
    for mk in celt_sources.mk silk_sources.mk opus_sources.mk; do
        for var in CELT_SOURCES SILK_SOURCES SILK_SOURCES_FLOAT OPUS_SOURCES OPUS_SOURCES_FLOAT; do
            awk -v v="$var" '
                $0 ~ "^"v"[ \t]*=" { on = 1; sub("^"v"[ \t]*=", "") }
                on { line = $0; cont = sub(/\\[ \t]*$/, "", line); n = split(line, w, /[ \t]+/)
                     for (i = 1; i <= n; i++) if (w[i] ~ /\.c$/) print w[i]
                     if (!cont) on = 0 }' "$OPUS/$mk"
        done
    done | sort -u
}

# engine/sources.txt: one path per line relative to engine/ (or to native/audio), '#' comments.
engine_sources() {
    local line
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%#*}"; line="$(echo "$line" | xargs)"
        [ -z "$line" ] && continue
        if [ -f "$ENGINE/$line" ]; then echo "$ENGINE/$line"
        elif [ -f "$HERE/$line" ]; then echo "$HERE/$line"
        else echo "engine/sources.txt: missing $line" >&2; exit 1
        fi
    done < "$ENGINE/sources.txt"
}

HAVE_ENGINE=0
[ -f "$ENGINE/sources.txt" ] && [ -d "$OPUS" ] && HAVE_ENGINE=1

MAC_BACKEND=(backend_mac.mm devices_mac.mm tap_mac.mm vmic_mac.mm)
jobs_file="$OBJ/jobs.txt"
: > "$jobs_file"
helper_objs=()
check_objs=()
for arch in "${ARCHS[@]}"; do
    add() {  # group src list-var...
        local group="$1" src="$2"; shift 2
        local rel="${src#$HERE/}"
        local obj="$OBJ/$arch/${rel//\//_}.o"
        stale "$obj" "$src" && echo "$group $arch $src $obj" >> "$jobs_file"
        local v
        for v in "$@"; do eval "$v+=(\"\$obj\")"; done
    }
    for f in "${MAC_BACKEND[@]}"; do add mm "$HERE/mac/$f" helper_objs check_objs; done
    add mm "$HERE/mac/backend_check.mm" check_objs
    if [ $HAVE_ENGINE = 1 ]; then
        add mm "$HERE/mac/main.mm" helper_objs
        while IFS= read -r src; do
            case "$src" in
                *.c) add c "$src" helper_objs ;;
                *.mm) add mm "$src" helper_objs ;;
                *) add cxx "$src" helper_objs ;;
            esac
        done < <(engine_sources)
        if ! grep -q 'monocypher' "$ENGINE/sources.txt"; then add c "$MONO/src/monocypher.c" helper_objs; fi
        while IFS= read -r src; do add opus "$OPUS/$src" helper_objs; done < <(opus_sources)
    fi
done

n="$(wc -l < "$jobs_file" | tr -d ' ')"
echo "compiling $n file(s) for ${ARCHS[*]} ($JOBS jobs)"
if [ "$n" -gt 0 ]; then
    tr '\n' '\0' < "$jobs_file" | xargs -0 -n 1 -P "$JOBS" sh -c 'set -f; exec "$0" --cc $1' "$0" \
        || { echo "compile failed" >&2; exit 1; }
fi

link() {  # name objs-for-all-archs...
    local name="$1"; shift
    local slices=()
    for arch in "${ARCHS[@]}"; do
        local mine=()
        for o in "$@"; do case "$o" in "$OBJ/$arch/"*) mine+=("$o") ;; esac; done
        clang++ -arch "$arch" -mmacosx-version-min=$MIN -Wl,-S "${mine[@]}" "${FRAMEWORKS[@]}" -o "$OBJ/$arch/$name"
        slices+=("$OBJ/$arch/$name")
    done
    lipo -create -output "$OUT/$name" "${slices[@]}"
    if strings -a "$OUT/$name" | grep -q "$HOME"; then
        echo "error: $OUT/$name embeds a path under $HOME" >&2; exit 1
    fi
    codesign --force --sign - "$OUT/$name" 2>/dev/null || true
    echo "built  $OUT/$name ($(lipo -archs "$OUT/$name"))"
}

link backend_check "${check_objs[@]}"
if [ $HAVE_ENGINE = 1 ]; then
    link beamer-audio "${helper_objs[@]}"
else
    echo "warning: engine/sources.txt or third_party/opus-1.6.1 missing (WP2 not merged): helper not built" >&2
fi

if [ $DRIVER = 1 ]; then
    "$HERE/mac-driver/build.sh"
    rm -rf "$OUT/BeamerAudio.driver"
    ditto "$HERE/mac-driver/build/BeamerAudio.driver" "$OUT/BeamerAudio.driver"
    echo "copied $OUT/BeamerAudio.driver"
fi

[ $HAVE_ENGINE = 1 ] || exit 3
