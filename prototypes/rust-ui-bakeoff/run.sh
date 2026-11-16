#!/bin/sh
# PROTOTYPE: run one bake-off toolkit on the Mac. Usage: run.sh <rune|gpui|slint> [flags...]
set -e
export PATH="$HOME/.cargo/bin:$PATH"
tk="$1"; shift
cd "$(dirname "$0")/$tk"
exec cargo run --release -- "$@"
