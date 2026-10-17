#!/usr/bin/env bash
# fetch-toolchain.sh - download the pinned llvm-mingw into toolchain/ (tools/fetch-toolchain.cmake; the pin is in
# tools/llvm-mingw.cmake).
#
#   tools/fetch-toolchain.sh            this host's toolchain
#   tools/fetch-toolchain.sh --windows  the Windows-hosted one too
#
# env overrides: LLVM_MINGW_ROOT (where they go, default toolchain/)
set -euo pipefail

ARGS=()
for a in "$@"; do
    case "$a" in
        --windows) ARGS=("-DHOSTS=linux;windows") ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done
exec cmake "${ARGS[@]}" -P "$(dirname "${BASH_SOURCE[0]}")/fetch-toolchain.cmake"
