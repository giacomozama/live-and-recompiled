#!/usr/bin/env bash
# package.sh - build the player release: Setup.exe, setup-linux and a kit with everything but the game's code.
#
# Players build the game on their own machine from their own copy: Setup finds the game in assets/, downloads the
# pinned llvm-mingw for its host, recompiles the game, compiles the result and links Game.exe. Nothing in the release
# is the title's code, or the compiler (see docs/distribution.md).
#
#   tools/package.sh                  build the kit, test it against the game in game/assets, zip it
#   tools/package.sh --no-test        skip the test (it needs the game in game/assets)
#   tools/package.sh --test-download  test with the compiler setup-linux downloads, not the one in toolchain/
#   tools/package.sh -j N             parallel jobs (default: nproc)
#
# Output: game/out/release/Conker/ and game/out/release/Conker-<version>.zip
# env overrides: LLVM_MINGW_ROOT (toolchains, default toolchain/), JOBS
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$ROOT/game"
VERSION_TC="$(sed -n 's/^VERSION=//p' "$ROOT/tools/fetch-toolchain.sh")"
TC="${LLVM_MINGW_ROOT:-$ROOT/toolchain}"
LIN="$TC/llvm-mingw-$VERSION_TC-linux"
BUILD="$GAME/out/build/llvm-mingw"
REL="$GAME/out/build/release"
LINUX="$GAME/out/build/linux-kit"  # setup-linux and xbe_recomp for players: static, any distro
OUT="$GAME/out/release"
NAME=Conker
JOBS="${JOBS:-$(nproc)}"
TEST=1
TEST_DOWNLOAD=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-test) TEST=0 ;;
        --test-download) TEST_DOWNLOAD=1 ;;
        -j) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

echo "[1/5] toolchain"
LLVM_MINGW_ROOT="$TC" "$ROOT/tools/fetch-toolchain.sh"

echo "[2/5] kit"
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$GAME" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$GAME/cmake-llvm-mingw.cmake" -DLLVM_MINGW="$LIN" \
        -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
fi
if [ ! -f "$REL/build.ninja" ]; then
    cmake -S "$GAME" -B "$REL" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
        -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
fi
if [ ! -f "$LINUX/build.ninja" ]; then
    cmake -S "$GAME" -B "$LINUX" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" -DCMAKE_EXE_LINKER_FLAGS="-static" \
        -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
fi
cmake --build "$REL" --target xbe_recomp -j "$JOBS"
cmake --build "$LINUX" --target xbe_recomp xbe_setup -j "$JOBS"
cmake --build "$BUILD" --target Game_kit -j "$JOBS"

echo "[3/5] staging $OUT/$NAME"
DEST="$OUT/$NAME"
rm -rf "$DEST"
mkdir -p "$OUT"
cp -r "$BUILD/package/Game" "$DEST"
cp "$GAME/README.md" "$DEST/README.md"
cp "$LINUX/xbe_setup" "$DEST/setup-linux"
cp "$LINUX/xbe_recomp" "$DEST/kit/bin/xbe_recomp"

# no compiler in the release: Setup downloads the llvm-mingw of the same release for its host on the player's machine
{
    echo
    echo "# the compiler (llvm-mingw $VERSION_TC) for the host Setup runs on, downloaded into kit/toolchain/<host> once"
    "$ROOT/tools/fetch-toolchain.sh" --print | while read -r host url sha; do
        printf '[toolchain.%s]\nurl = "%s"\nsha256 = "%s"\n' "$host" "$url" "$sha"
    done
} >> "$DEST/kit/kit.toml"
echo "      kit $(du -sh "$DEST/kit" | cut -f1), Setup downloads llvm-mingw $VERSION_TC"

echo "[4/5] test"
if [ "$TEST" = 1 ]; then
    # a Linux player's steps: the release's setup-linux and recompiler, the game from game/assets, and the Linux-hosted
    # llvm-mingw from toolchain/ (or, with --test-download, the one setup-linux downloads)
    [ -e "$GAME/assets/default.xbe" ] || { echo "      no game in $GAME/assets (use --no-test)" >&2; exit 1; }
    SCRATCH="$(mktemp -d "$OUT/.test.XXXXXX")"
    cp -r "$DEST/kit" "$SCRATCH/kit"
    cp "$DEST/setup-linux" "$SCRATCH/"
    ln -s "$GAME/assets" "$SCRATCH/assets"
    TC_ARGS=(--toolchain "$LIN")
    [ "$TEST_DOWNLOAD" = 1 ] && TC_ARGS=()
    "$SCRATCH/setup-linux" "${TC_ARGS[@]}" --jobs "$JOBS" | tr '\r' '\n' | grep -v '^  [0-9]* / '
    [ -f "$SCRATCH/Game.exe" ] || { echo "      the test build made no Game.exe" >&2; exit 1; }
    [ ! -d "$SCRATCH/kit/work" ] || { echo "      Setup left kit/work behind" >&2; exit 1; }
    rm -rf "$SCRATCH"
    echo "      the kit builds Game.exe"
else
    echo "      skipped"
fi

echo "[5/5] zip"
VERSION="$(git -C "$ROOT" describe --always --dirty)"
ZIP="$OUT/$NAME-$VERSION.zip"
rm -f "$ZIP"
(cd "$OUT" && zip -qr -9 "$ZIP" "$NAME")
echo "      $ZIP ($(du -h "$ZIP" | cut -f1))"
