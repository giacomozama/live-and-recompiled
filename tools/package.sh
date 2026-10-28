#!/usr/bin/env bash
# package.sh - build, test and zip the player release (tools/package.cmake, which says what it does).
#
#   tools/package.sh                  build the kit, test it against the game in assets/, zip it
#   tools/package.sh --no-test        skip the test (it needs the game in assets/)
#   tools/package.sh --test-download  test with the compiler Setup downloads, not the one in toolchain/
#   tools/package.sh -j N             parallel jobs (default: every core)
#
# Output: out/release/LiveAndRecompiled/, out/release/LiveAndRecompiled-<version>-windows-x86_64.zip and out/release/LiveAndRecompiled-<version>-linux-x86_64.tar.gz
# env overrides: XBE_TOOLCHAIN_ROOT (the toolchain, default toolchain/)
set -euo pipefail

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-test) ARGS+=(-DTEST=OFF) ;;
        --test-download) ARGS+=(-DTEST_DOWNLOAD=ON) ;;
        -j) ARGS+=("-DJOBS=${2:?-j needs a number}"); shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
exec cmake "${ARGS[@]}" -P "$(dirname "${BASH_SOURCE[0]}")/package.cmake"
