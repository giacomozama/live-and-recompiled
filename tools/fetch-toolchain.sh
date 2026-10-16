#!/usr/bin/env bash
# fetch-toolchain.sh - download the pinned llvm-mingw the Windows build and the player package use.
#
# The prebuilt half of a release and the player's own build of the game code must come from the same compiler and
# runtime libraries, so the version is pinned here and checked by SHA-256.
#
#   tools/fetch-toolchain.sh            the Linux-hosted toolchain (builds Game.exe and the package on this machine)
#   tools/fetch-toolchain.sh --windows  the Windows-hosted toolchain too (what the package ships to players)
#
# Installs into $LLVM_MINGW_ROOT (default: toolchain/ at the repository root):
#   llvm-mingw-<ver>-linux/    llvm-mingw-<ver>-windows/
set -euo pipefail

VERSION=20260922
LINUX_SHA=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
WINDOWS_SHA=e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666
URL=https://github.com/mstorsjo/llvm-mingw/releases/download/$VERSION

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${LLVM_MINGW_ROOT:-$ROOT/toolchain}"

WINDOWS=0
for a in "$@"; do
    case "$a" in
        --windows) WINDOWS=1 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

# fetch <archive> <sha256> <folder>: downloads into a folder of its own, checks it, unpacks into another
fetch() {
    local name="$1" sha="$2" out="$DEST/$3"
    if [ -d "$out" ]; then
        echo "have $out"
        return
    fi
    local dl
    dl="$(mktemp -d "$DEST/.download.XXXXXX")"
    echo "downloading $name"
    curl -fL --retry 3 -o "$dl/$name" "$URL/$name"
    echo "$sha  $dl/$name" | sha256sum -c --quiet
    mkdir "$dl/unpacked"
    case "$name" in
        *.tar.xz) tar -xJf "$dl/$name" -C "$dl/unpacked" ;;
        *.zip) (command -v bsdtar >/dev/null && bsdtar -xf "$dl/$name" -C "$dl/unpacked") || unzip -q "$dl/$name" -d "$dl/unpacked" ;;
    esac
    mv "$dl/unpacked/${name%%.tar.xz}" "$out" 2>/dev/null || mv "$dl/unpacked/${name%.zip}" "$out"
    rm -rf "$dl"
    echo "installed $out"
}

mkdir -p "$DEST"
fetch "llvm-mingw-$VERSION-ucrt-ubuntu-22.04-x86_64.tar.xz" "$LINUX_SHA" "llvm-mingw-$VERSION-linux"
[ "$WINDOWS" = 1 ] && fetch "llvm-mingw-$VERSION-ucrt-x86_64.zip" "$WINDOWS_SHA" "llvm-mingw-$VERSION-windows"
exit 0
