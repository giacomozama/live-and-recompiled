#!/usr/bin/env bash
# package.sh - build the player release: Setup.exe and a kit with everything but the game's code.
#
# Players build the game on their own machine from their own copy: Setup.exe finds the game in assets/, recompiles it,
# compiles the result with the kit's compiler and links Game.exe. Nothing in the release is the title's code (see
# docs/distribution.md).
#
#   tools/package.sh              build the kit, test it against the game in game/assets, zip it
#   tools/package.sh --no-test    skip the test (it needs the game in game/assets)
#   tools/package.sh -j N         parallel jobs (default: nproc)
#
# Output: game/out/release/Conker/ and game/out/release/Conker-<version>.zip
# env overrides: LLVM_MINGW_ROOT (toolchains, default toolchain/), JOBS
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$ROOT/game"
VERSION_TC="$(sed -n 's/^VERSION=//p' "$ROOT/tools/fetch-toolchain.sh")"
TC="${LLVM_MINGW_ROOT:-$ROOT/toolchain}"
LIN="$TC/llvm-mingw-$VERSION_TC-linux"
WIN="$TC/llvm-mingw-$VERSION_TC-windows"
BUILD="$GAME/out/build/llvm-mingw"
REL="$GAME/out/build/release"
OUT="$GAME/out/release"
NAME=Conker
JOBS="${JOBS:-$(nproc)}"
TEST=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-test) TEST=0 ;;
        -j) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

echo "[1/5] toolchains"
LLVM_MINGW_ROOT="$TC" "$ROOT/tools/fetch-toolchain.sh" --windows

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
cmake --build "$REL" --target xbe_recomp xbe_setup -j "$JOBS"
cmake --build "$BUILD" --target Game_kit -j "$JOBS"

echo "[3/5] staging $OUT/$NAME"
DEST="$OUT/$NAME"
rm -rf "$DEST"
mkdir -p "$OUT"
cp -r "$BUILD/package/Game" "$DEST"
cp "$GAME/README.md" "$DEST/README.md"

# the Windows-hosted compiler and linker, the x86_64 target's headers and libraries; nothing for other targets
T="$DEST/kit/toolchain"
CLANG_VER="$(ls "$WIN/lib/clang")"
mkdir -p "$T/bin" "$T/x86_64-w64-mingw32" "$T/lib/clang/$CLANG_VER/lib/windows"
for f in x86_64-w64-mingw32-clang++.exe clang++.exe clang.exe "clang-$CLANG_VER.exe" ld.lld.exe; do
    cp "$WIN/bin/$f" "$T/bin/"
done
cp "$WIN"/bin/*.cfg "$T/bin/"
# the DLLs those load, and the ones those load
want=$(ls "$T"/bin/*.exe)
while [ -n "$want" ]; do
    next=""
    for f in $want; do
        for dll in $("$LIN/bin/llvm-objdump" -p "$f" | awk '/DLL Name:/ {print $3}'); do
            if [ -f "$WIN/bin/$dll" ] && [ ! -f "$T/bin/$dll" ]; then
                cp "$WIN/bin/$dll" "$T/bin/"
                next="$next $T/bin/$dll"
            fi
        done
    done
    want="$next"
done
cp -r "$WIN/include" "$T/"
find "$T/include" -name '*.idl' -delete
cp -r "$WIN/x86_64-w64-mingw32/lib" "$T/x86_64-w64-mingw32/"
cp -r "$WIN/lib/clang/$CLANG_VER/include" "$T/lib/clang/$CLANG_VER/"
cp "$WIN/lib/clang/$CLANG_VER/lib/windows/libclang_rt.builtins-x86_64.a" "$T/lib/clang/$CLANG_VER/lib/windows/"
cp "$WIN/LICENSE.TXT" "$T/LICENSE.TXT"
echo "      toolchain $(du -sh "$T" | cut -f1), kit $(du -sh "$DEST/kit" | cut -f1)"

echo "[4/5] test"
if [ "$TEST" = 1 ]; then
    # the player's steps on this machine: the native Setup and recompiler, the Linux-hosted llvm-mingw of the same
    # release, the game from game/assets
    [ -e "$GAME/assets/default.xbe" ] || { echo "      no game in $GAME/assets (use --no-test)" >&2; exit 1; }
    SCRATCH="$(mktemp -d "$OUT/.test.XXXXXX")"
    cp -r "$DEST/kit" "$SCRATCH/kit"
    rm "$SCRATCH/kit/bin/xbe_recomp.exe"
    cp "$REL/xbe_recomp" "$SCRATCH/kit/bin/xbe_recomp"
    ln -s "$GAME/assets" "$SCRATCH/assets"
    "$REL/xbe_setup" --root "$SCRATCH" --toolchain "$LIN" --jobs "$JOBS" | tr '\r' '\n' | grep -v '^  [0-9]* / '
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
