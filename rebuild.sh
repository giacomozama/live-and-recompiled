#!/usr/bin/env bash
# rebuild.sh - rebuild the recompiled game from the disc image (ISO).
#
# Recreates everything that is derived from the ISO, so a fresh checkout (or a
# changed config / patched SDK) can be brought back to a built game:
#
#   1. extract the disc image into ./iso            (default.xbe, dvddata/, media/)
#   2. point game/assets at that tree               (default.xbe, dvddata, media)
#   3. build the native recompiler tools            (xbe_symbols, xbe_recomp)
#   4. write game/config/game.toml if it is absent  (xbe_symbols output)
#   5. recompile the XBE into game/generated/game   (xbe_recomp via CMake)
#
# Usage:
#   ./rebuild.sh                    extract (if needed), tools, config, recompile
#   ./rebuild.sh --iso FILE         use FILE as the disc image
#   ./rebuild.sh --no-extract       keep the existing ./iso tree, never extract
#   ./rebuild.sh --force-extract    re-extract even when ./iso is populated
#   ./rebuild.sh --fresh-config     regenerate config/game.toml (drops hand edits)
#   ./rebuild.sh --windows          also build the Windows exe (run-proton.sh --build-only)
#   ./rebuild.sh --harness          also build the Linux headless harness
#   ./rebuild.sh -j N               parallel jobs (default: nproc)
#   ./rebuild.sh -h|--help          this text
#
# env overrides: ISO (disc image), JOBS (jobs), EXTRACT_XISO (extract-xiso path)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME="$ROOT/game"
ISO_DIR="$ROOT/iso"
ASSETS="$GAME/assets"
REL="$GAME/out/build/release"
CONFIG="$GAME/config/game.toml"
EXTRACT_XISO="${EXTRACT_XISO:-extract-xiso}"
JOBS="${JOBS:-$(nproc)}"

DO_EXTRACT=auto
FRESH_CONFIG=0
DO_WINDOWS=0
DO_HARNESS=0
ISO="${ISO:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-extract)    DO_EXTRACT=no ;;
        --force-extract) DO_EXTRACT=force ;;
        --fresh-config)  FRESH_CONFIG=1 ;;
        --windows)       DO_WINDOWS=1 ;;
        --harness)       DO_HARNESS=1 ;;
        --iso)           ISO="${2:-}"; [ -n "$ISO" ] || { echo "--iso needs a path" >&2; exit 2; }; shift ;;
        --iso=*)         ISO="${1#--iso=}" ;;
        -j)              JOBS="${2:-}"; [ -n "$JOBS" ] || { echo "-j needs a number" >&2; exit 2; }; shift ;;
        -j*)             JOBS="${1#-j}" ;;
        -h|--help)       sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# first .iso found: --iso/env, the repo root, then the home directory
find_iso() {
    if [ -n "$ISO" ]; then
        [ -f "$ISO" ] || { echo "no such disc image: $ISO" >&2; exit 1; }
        printf '%s\n' "$ISO"; return
    fi
    local c
    for c in "$ROOT"/*.iso "$ROOT"/iso/*.iso "$HOME"/*.iso; do
        [ -f "$c" ] && { printf '%s\n' "$c"; return; }
    done
    echo "no .iso found (pass --iso FILE or set ISO=)" >&2
    exit 1
}

#[1/5] disc image -> ./iso
echo "[1/5] disc image"
if [ "$DO_EXTRACT" = no ]; then
    echo "      keeping the existing $ISO_DIR tree"
elif [ "$DO_EXTRACT" = auto ] && [ -f "$ISO_DIR/default.xbe" ]; then
    echo "      $ISO_DIR already extracted"
else
    command -v "$EXTRACT_XISO" >/dev/null 2>&1 || { echo "missing $EXTRACT_XISO" >&2; exit 1; }
    IMG="$(find_iso)"
    echo "      extracting $(basename "$IMG") -> $ISO_DIR"
    if [ "$DO_EXTRACT" = force ]; then rm -rf "$ISO_DIR"; fi
    mkdir -p "$ISO_DIR"
    "$EXTRACT_XISO" -x "$IMG" -d "$ISO_DIR"
fi
[ -f "$ISO_DIR/default.xbe" ] || { echo "no default.xbe in $ISO_DIR (is the disc image valid?)" >&2; exit 1; }

#[2/5] game/assets -> the extracted tree
echo "[2/5] game/assets"
mkdir -p "$ASSETS"
for d in default.xbe dvddata media; do
    [ -e "$ISO_DIR/$d" ] || { echo "missing $ISO_DIR/$d" >&2; exit 1; }
    ln -sfn "../../iso/$d" "$ASSETS/$d"
done
echo "      linked default.xbe, dvddata, media -> iso/"

#[3/5] native recompiler tools
echo "[3/5] recompiler tools"
if [ ! -f "$REL/build.ninja" ]; then
    echo "      configuring $REL"
    cmake -S "$GAME" -B "$REL" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
        -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
fi
cmake --build "$REL" --target xbe_symbols xbe_recomp -j "$JOBS"

#[4/5] config/game.toml
echo "[4/5] config"
if [ "$FRESH_CONFIG" = 1 ] || [ ! -f "$CONFIG" ]; then
    if [ -f "$CONFIG" ]; then
        echo "      --fresh-config: overwriting config/game.toml (hand-filled [abi] is lost)"
    fi
    ( cd "$GAME" && "$REL/xbe_symbols" assets/default.xbe sdk/xdk --out config/game.toml )
else
    echo "      keeping existing config/game.toml (use --fresh-config to regenerate)"
fi

#[5/5] recompile the XBE
echo "[5/5] recompile"
( cd "$GAME" && cmake --build "$REL" --target recompile_Game -j "$JOBS" )
echo "      game/generated/game is up to date"

if [ "$DO_HARNESS" = 1 ]; then
    echo "[+] headless harness"
    ( cd "$GAME" && JOBS="$JOBS" bash sdk/harness/build.sh -r . -g generated/game -o out/harness -n conker )
fi

if [ "$DO_WINDOWS" = 1 ]; then
    echo "[+] Windows exe"
    "$ROOT/run-proton.sh" --build-only
fi

echo
echo "  rebuilt from disc image. config/game.toml + game/generated/game are current."
