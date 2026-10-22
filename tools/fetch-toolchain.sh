#!/usr/bin/env bash
# fetch-toolchain.sh - download the pinned Zig into toolchain/ (tools/fetch-toolchain.cmake; the pin is in
# tools/toolchains.cmake).
#
#   tools/fetch-toolchain.sh            Zig for this host
#   tools/fetch-toolchain.sh --windows  the Windows-hosted one too
#
# env overrides: XBE_TOOLCHAIN_ROOT (where it goes, default toolchain/)
set -euo pipefail

ARGS=()
for a in "$@"; do
    case "$a" in
        --windows) ARGS+=("-DHOSTS=linux;windows") ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done
exec cmake "${ARGS[@]}" -P "$(dirname "${BASH_SOURCE[0]}")/fetch-toolchain.cmake"
