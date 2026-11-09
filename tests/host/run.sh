#!/usr/bin/env bash
#
# Build and run the host side decoder tests.
#
# macOS / Linux counterpart of run.ps1.  Works with any C++17 compiler; the
# script looks, in this order, for:
#   1. --compiler <path> argument, or the COMPILER environment variable
#   2. zig on PATH, or tools/zig/zig inside the project
#   3. g++ / clang++ on PATH
#
# Usage:
#   ./tests/host/run.sh
#   ./tests/host/run.sh --compiler /opt/homebrew/bin/g++-14
#   COMPILER=clang++ ./tests/host/run.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

out="tests/host/test_decoder"
sources=("tests/host/test_decoder.cpp" "src/ook_decoder.cpp")

compiler="${COMPILER:-}"

usage() {
    awk 'NR > 2 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else { exit } }' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--compiler)
            [[ $# -ge 2 ]] || { echo "error: --compiler needs a path" >&2; exit 2; }
            compiler="$2"
            shift 2
            ;;
        --compiler=*)
            compiler="${1#*=}"
            shift
            ;;
        -h|--help)
            usage 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage 2
            ;;
    esac
done

if [[ -n "$compiler" ]]; then
    if ! command -v "$compiler" >/dev/null 2>&1; then
        echo "error: compiler not found: $compiler" >&2
        exit 2
    fi
else
    local_zig="$(find tools/zig -type f -name zig -perm -u+x 2>/dev/null | head -n 1 || true)"
    for candidate in "$local_zig" zig g++ clang++; do
        [[ -n "$candidate" ]] || continue
        if command -v "$candidate" >/dev/null 2>&1; then
            compiler="$candidate"
            break
        fi
    done
fi

if [[ -z "$compiler" ]]; then
    echo "error: no C++ compiler found." >&2
    echo "       install g++/clang++ (xcode-select --install) or pass --compiler <path>." >&2
    exit 2
fi

is_zig=false
case "$(basename "$compiler")" in
    zig*) is_zig=true ;;
esac

build_args=(-std=c++17 -O2 -Wall -I tests/host -I src -o "$out" "${sources[@]}")

echo "Building with: $compiler"
if [[ "$is_zig" == true ]]; then
    "$compiler" c++ "${build_args[@]}"
else
    "$compiler" "${build_args[@]}"
fi

echo
"$out"
