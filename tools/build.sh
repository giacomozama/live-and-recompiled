#!/usr/bin/env bash
# build.sh - build the port from a checkout: the game into game/assets, the pinned toolchain, Game.exe.
#
#   tools/build.sh                 build game/out/build/llvm-mingw/Game.exe from the game in game/assets
#   tools/build.sh --iso FILE      first extract the disc image into iso/ and link game/assets to it
#   tools/build.sh --store PATH    the same from the digital release's DefaultPackage (or its folder), see unpack-store
#   tools/build.sh --game DIR      first link game/assets to an extracted copy (default.xbe, dvddata/, media/)
#   tools/build.sh --harness       also build the Linux headless harness, game/out/harness/conker
#   tools/build.sh --pdb           also write Game.pdb (off again: cmake -DXBE_PDB=OFF game/out/build/llvm-mingw)
#   tools/build.sh -j N            parallel jobs (default: nproc)
#
# The first build recompiles the XBE and compiles the result, which takes a while; later ones are incremental.
# Game.exe finds assets/ by walking up from its own folder: run it from where it is with Proton or Wine.
# env overrides: LLVM_MINGW_ROOT (toolchains, default toolchain/), JOBS, EXTRACT_XISO (extract-xiso path)
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/configure.sh"
ASSETS="$GAME/assets"
ISO_DIR="$ROOT/iso"
EXTRACT_XISO="${EXTRACT_XISO:-extract-xiso}"
ISO=""
STORE=""
FROM=""
HARNESS=0
PDB=0

while [ $# -gt 0 ]; do
    case "$1" in
        --iso) ISO="${2:-}"; [ -n "$ISO" ] || { echo "--iso needs a file" >&2; exit 2; }; shift ;;
        --store) STORE="${2:-}"; [ -n "$STORE" ] || { echo "--store needs a package" >&2; exit 2; }; shift ;;
        --game) FROM="${2:-}"; [ -n "$FROM" ] || { echo "--game needs a folder" >&2; exit 2; }; shift ;;
        --harness) HARNESS=1 ;;
        --pdb) PDB=1 ;;
        -j) JOBS="${2:-}"; [ -n "$JOBS" ] || { echo "-j needs a number" >&2; exit 2; }; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# link <dir> <prefix>: game/assets/{default.xbe,dvddata,media} -> <prefix><name>, never over real files
link_assets() {
    mkdir -p "$ASSETS"
    local d
    for d in default.xbe dvddata media; do
        [ -e "$1/$d" ] || { echo "no $d in $1" >&2; exit 1; }
        if [ -e "$ASSETS/$d" ] && [ ! -L "$ASSETS/$d" ]; then
            echo "game/assets/$d is a real file or folder, not a link: move it away first" >&2
            exit 1
        fi
        ln -sfn "$2$d" "$ASSETS/$d"
    done
    echo "      game/assets -> $1"
}

echo "[1/4] game"
if [ -n "$ISO" ]; then
    [ -f "$ISO" ] || { echo "no such disc image: $ISO" >&2; exit 1; }
    if [ -f "$ISO_DIR/default.xbe" ]; then
        echo "      iso/ is already extracted (delete it to extract another image)"
    else
        command -v "$EXTRACT_XISO" >/dev/null || { echo "missing $EXTRACT_XISO (or set EXTRACT_XISO)" >&2; exit 1; }
        echo "      extracting $(basename "$ISO") -> iso/"
        mkdir -p "$ISO_DIR"
        "$EXTRACT_XISO" -q -x "$ISO" -d "$ISO_DIR"
    fi
    link_assets "$ISO_DIR" "../../iso/"
elif [ -n "$STORE" ]; then
    if [ -f "$ISO_DIR/default.xbe" ]; then
        echo "      iso/ is already extracted (delete it to unpack another package)"
    else
        configure_rel
        cmake --build "$REL" --target unpack_store -j "$JOBS" >/dev/null
        mkdir -p "$ISO_DIR"
        "$REL/unpack-store" "$STORE" "$ISO_DIR"
    fi
    link_assets "$ISO_DIR" "../../iso/"
elif [ -n "$FROM" ]; then
    FROM="$(realpath "$FROM")"
    link_assets "$FROM" "$FROM/"
fi
if [ ! -f "$ASSETS/default.xbe" ]; then
    echo "no game in game/assets: pass --iso FILE (a disc image), --store PATH (the digital release's package) or --game DIR (extracted files)" >&2
    exit 1
fi
echo "      $(sha1sum "$ASSETS/default.xbe" | cut -c1-40)  default.xbe"

echo "[2/4] toolchain"
"$ROOT/tools/fetch-toolchain.sh"

echo "[3/4] recompiler"
configure_rel
cmake --build "$REL" --target xbe_recomp -j "$JOBS"

echo "[4/4] Game.exe"
configure_mingw
[ "$PDB" = 1 ] && cmake -DXBE_PDB=ON "$BUILD" >/dev/null
cmake --build "$BUILD" --target Game -j "$JOBS"
echo "      $BUILD/Game.exe"

if [ "$HARNESS" = 1 ]; then
    echo "[+] headless harness"
    (cd "$GAME" && JOBS="$JOBS" bash sdk/harness/build.sh -r . -g generated/game -o out/harness -n conker)
    echo "      $GAME/out/harness/conker"
fi
