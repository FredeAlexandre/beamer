#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds the engine test binary and the null-backend helper on macOS/Linux with clang (Command Line
# Tools are enough: no CMake, no Xcode).
#   native/audio/engine/build-tests.sh [--asan | --tsan] [--run] [-j N]
# Outputs: native/audio/build/tests[-asan|-tsan]/{engine_tests,beamer-audio-null}
# Objects are cached under native/audio/build/obj/<variant>/ and rebuilt when the source (or, for
# engine/test sources, any engine header) is newer.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"           # native/audio
OPUS="$ROOT/third_party/opus-1.6.1"
VARIANT=plain
RUN=0
JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) )"
while [ $# -gt 0 ]; do
  case "$1" in
    --asan) VARIANT=asan ;;
    --tsan) VARIANT=tsan ;;
    --run) RUN=1 ;;
    -j) shift; JOBS="$1" ;;
    *) echo "usage: $0 [--asan|--tsan] [--run] [-j N]" >&2; exit 2 ;;
  esac
  shift
done

CC="${CC:-clang}"
CXX="${CXX:-clang++}"
REPO="$(cd "$ROOT/../.." && pwd)"
# no home-folder paths in the binaries (__FILE__ in asserts, debug info): the Mac app build refuses them
COMMON="-g -fno-omit-frame-pointer -ffile-prefix-map=$REPO/="
case "$VARIANT" in
  plain) OPT="-O2"; SAN="" ;;
  asan)  OPT="-O1"; SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
  tsan)  OPT="-O1"; SAN="-fsanitize=thread" ;;
esac
if [ "$(uname)" = "Darwin" ]; then
  COMMON="$COMMON -mmacosx-version-min=13.0"
fi
OUT="$ROOT/build/tests$([ "$VARIANT" = plain ] || echo "-$VARIANT")"
OBJ="$ROOT/build/obj/$VARIANT"
mkdir -p "$OUT" "$OBJ/opus" "$OBJ/engine" "$OBJ/tests"

# ---- libopus from its own source lists (float build, no intrinsics/RTCD)
mkvar() {  # mkvar FILE VAR -> one path per line
  awk -v v="$2" '$1==v && $2=="=" {f=1; next} f { line=$0; gsub(/\\/,"",line); n=split(line,a," "); for(i=1;i<=n;i++) print a[i]; if ($0 !~ /\\$/) f=0 }' "$1"
}
OPUS_SRCS="$( (cd "$OPUS" && mkvar celt_sources.mk CELT_SOURCES; mkvar silk_sources.mk SILK_SOURCES; \
  mkvar silk_sources.mk SILK_SOURCES_FLOAT; mkvar opus_sources.mk OPUS_SOURCES; mkvar opus_sources.mk OPUS_SOURCES_FLOAT) )"
OPUS_FLAGS="$COMMON $OPT $SAN -w -DPACKAGE_VERSION=\"1.6.1\" -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF -I$OPUS/include -I$OPUS/celt -I$OPUS/silk -I$OPUS/silk/float -I$OPUS/src"

SPEEX="$ROOT/third_party/speexdsp-1.2.1"
INC="-I$HERE -I$OPUS/include -I$ROOT/third_party/monocypher-4.0.2/src -I$ROOT/third_party/nlohmann-json-3.11.3/single_include -I$SPEEX/include"
CXX_FLAGS="$COMMON $OPT $SAN -std=c++17 -Wall -Wextra -Wno-unused-parameter $INC"

NEWEST_HDR="$(ls -t "$HERE"/*.h | head -1)"

# job list: "compiler|flags-var|src|obj"
JOBLIST="$OBJ/jobs.txt"
: > "$JOBLIST"
OBJS=()
for s in $OPUS_SRCS; do
  o="$OBJ/opus/$(echo "$s" | tr '/' '_').o"
  OBJS+=("$o")
  if [ ! -f "$o" ] || [ "$OPUS/$s" -nt "$o" ]; then echo "C|$OPUS/$s|$o" >> "$JOBLIST"; fi
done
ENGINE_OBJS=()
for s in $(grep -v '^\s*#' "$HERE/sources.txt" | grep -v '^\s*$'); do
  b="$(basename "$s")"
  o="$OBJ/engine/${b%.*}.o"
  ENGINE_OBJS+=("$o")
  kind=X
  case "$s" in *speexdsp-*.c) kind=S ;; *.c) kind=E ;; esac
  if [ ! -f "$o" ] || [ "$HERE/$s" -nt "$o" ] || [ "$NEWEST_HDR" -nt "$o" ]; then echo "$kind|$HERE/$s|$o" >> "$JOBLIST"; fi
done
for s in engine_tests.cpp video_tests.cpp sync_tests.cpp null_main.cpp; do
  o="$OBJ/tests/${s%.cpp}.o"
  if [ ! -f "$o" ] || [ "$ROOT/tests/$s" -nt "$o" ] || [ "$NEWEST_HDR" -nt "$o" ]; then echo "X|$ROOT/tests/$s|$o" >> "$JOBLIST"; fi
done

N=$(wc -l < "$JOBLIST" | tr -d ' ')
echo "build-tests ($VARIANT): $N to compile, $JOBS jobs"
C_FLAGS="$COMMON $OPT $SAN -std=c99 -Wall -Wextra $INC"
# vendored speexdsp (echo canceller): float build, kiss FFT, no config.h, its own warnings off
SPEEX_FLAGS="$COMMON $OPT $SAN -std=c99 -w -DFLOATING_POINT -DUSE_KISS_FFT -DEXPORT= $INC"
export CC CXX OPUS_FLAGS CXX_FLAGS C_FLAGS SPEEX_FLAGS
if [ "$N" -gt 0 ]; then
  # shellcheck disable=SC2016
  tr '\n' '\0' < "$JOBLIST" | xargs -0 -P "$JOBS" -n 1 sh -c '
    IFS="|" read -r kind src obj <<EOF
$1
EOF
    case "$kind" in
      C) $CC $OPUS_FLAGS -c "$src" -o "$obj" || exit 255 ;;
      E) $CC $C_FLAGS -c "$src" -o "$obj" || exit 255 ;;
      S) $CC $SPEEX_FLAGS -c "$src" -o "$obj" || exit 255 ;;
      *) $CXX $CXX_FLAGS -c "$src" -o "$obj" || exit 255 ;;
    esac' sh
fi

LINK="$CXX $COMMON $SAN"
$LINK -o "$OUT/engine_tests" "$OBJ/tests/engine_tests.o" "$OBJ/tests/video_tests.o" "$OBJ/tests/sync_tests.o" "${ENGINE_OBJS[@]}" "${OBJS[@]}" -lpthread
$LINK -o "$OUT/beamer-audio-null" "$OBJ/tests/null_main.o" "${ENGINE_OBJS[@]}" "${OBJS[@]}" -lpthread
echo "built $OUT/engine_tests and $OUT/beamer-audio-null"

if [ "$RUN" = 1 ]; then
  if [ "$VARIANT" = tsan ]; then export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"; fi
  if [ "$VARIANT" = asan ]; then export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1" UBSAN_OPTIONS="print_stacktrace=1"; fi
  "$OUT/engine_tests" --vectors "$ROOT/tests/vectors"
fi
