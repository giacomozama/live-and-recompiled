#!/usr/bin/env bash
# build.sh - build the port from a checkout: the game into assets/, the pinned toolchain, Game.exe.
#
#   tools/build.sh                 build out/build/zig-windows/Game.exe from the game in assets/
#   tools/build.sh --iso FILE      first extract the disc image into assets/
#   tools/build.sh --store PATH    first unpack the digital release's DefaultPackage (or its folder) into assets/
#   tools/build.sh --game DIR      first copy extracted files into assets/ (default.xbe, dvddata/, media/)
#   tools/build.sh --linux         also build the native Linux port, out/build/zig-glibc/Game (SDL3, Vulkan)
#   tools/build.sh --android       also build the Android port, out/build/zig-android/libmain.so, and the app with it in,
#                                  out/build/android/app/outputs/apk/debug/app-debug.apk (Gradle's debug key; needs the
#                                  NDK, the SDK and a JDK 17 to 21: ANDROID_NDK_HOME, ANDROID_HOME, JAVA_HOME)
#   tools/build.sh --harness       also build the Linux headless harness, out/harness/conker
#   tools/build.sh --pdb           also write Game.pdb (off again: cmake -DXBE_PDB=OFF out/build/zig-windows)
#   tools/build.sh -j N            parallel jobs (default: nproc)
#
# The first build recompiles the XBE and compiles the result, which takes a while; later ones are incremental.
# Game.exe finds assets/ by walking up from its own folder: run it from where it is with Proton or Wine.
# env overrides: XBE_TOOLCHAIN_ROOT (the toolchain, default toolchain/), JOBS, EXTRACT_XISO (extract-xiso path)
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/configure.sh"
ASSETS="$ROOT/assets"
EXTRACT_XISO="${EXTRACT_XISO:-extract-xiso}"
ISO=""
STORE=""
FROM=""
HARNESS=0
LINUX=0
ANDROID=0
PDB=0

while [ $# -gt 0 ]; do
    case "$1" in
        --iso) ISO="${2:-}"; [ -n "$ISO" ] || { echo "--iso needs a file" >&2; exit 2; }; shift ;;
        --store) STORE="${2:-}"; [ -n "$STORE" ] || { echo "--store needs a package" >&2; exit 2; }; shift ;;
        --game) FROM="${2:-}"; [ -n "$FROM" ] || { echo "--game needs a folder" >&2; exit 2; }; shift ;;
        --harness) HARNESS=1 ;;
        --linux) LINUX=1 ;;
        --android) ANDROID=1 ;;
        --pdb) PDB=1 ;;
        -j) JOBS="${2:-}"; [ -n "$JOBS" ] || { echo "-j needs a number" >&2; exit 2; }; shift ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# ready_assets: true when assets/ is free for a game; links an older checkout left there (to iso/) are removed
ready_assets() {
    mkdir -p "$ASSETS"
    local d
    for d in default.xbe dvddata media; do
        if [ -L "$ASSETS/$d" ]; then rm "$ASSETS/$d"; fi
    done
    if [ -e "$ASSETS/default.xbe" ]; then
        echo "      assets/ already holds a game (empty it to put in another)"
        return 1
    fi
}

echo "[1/4] game"
if [ -n "$ISO" ]; then
    [ -f "$ISO" ] || { echo "no such disc image: $ISO" >&2; exit 1; }
    if ready_assets; then
        command -v "$EXTRACT_XISO" >/dev/null || { echo "missing $EXTRACT_XISO (or set EXTRACT_XISO)" >&2; exit 1; }
        echo "      extracting $(basename "$ISO") -> assets/"
        "$EXTRACT_XISO" -q -x "$ISO" -d "$ASSETS"
    fi
elif [ -n "$STORE" ]; then
    if ready_assets; then
        configure_linux
        cmake --build "$LIN" --target setup -j "$JOBS" >/dev/null
        echo "      unpacking $(basename "$STORE") -> assets/"
        "$LIN/setup-linux" --unpack "$STORE" "$ASSETS"
    fi
elif [ -n "$FROM" ]; then
    for d in default.xbe dvddata media; do
        [ -e "$FROM/$d" ] || { echo "no $d in $FROM" >&2; exit 1; }
    done
    if ready_assets; then
        echo "      copying $FROM -> assets/"
        cp -r --reflink=auto "$FROM/default.xbe" "$FROM/dvddata" "$FROM/media" "$ASSETS/"
    fi
fi
if [ ! -f "$ASSETS/default.xbe" ]; then
    echo "no game in assets/: pass --iso FILE (a disc image), --store PATH (the digital release's package) or --game DIR (extracted files)" >&2
    exit 1
fi
echo "      $(sha1sum "$ASSETS/default.xbe" | cut -c1-40)  default.xbe"

echo "[2/4] toolchain"
"$ROOT/tools/fetch-toolchain.sh"

echo "[3/4] recompiler"
configure_linux
cmake --build "$LIN" --target xbe_recomp -j "$JOBS"

echo "[4/4] Game.exe"
configure_windows
[ "$PDB" = 1 ] && cmake -DXBE_PDB=ON "$BUILD" >/dev/null
cmake --build "$BUILD" --target Game -j "$JOBS"
echo "      $BUILD/Game.exe"

if [ "$LINUX" = 1 ]; then
    echo "[+] native Linux port"
    cmake --build "$LIN" --target Game -j "$JOBS"
    echo "      $LIN/Game"
fi

if [ "$ANDROID" = 1 ]; then
    echo "[+] Android port"
    cmake -DWHAT=zig-android -P "$ROOT/tools/configure.cmake"
    cmake --build "$ROOT/out/build/zig-android" --target Game -j "$JOBS"
    mkdir -p "$ROOT/out/build/zig-android/apk/arm64-v8a"
    cp "$ROOT/out/build/zig-android/libmain.so" "$ROOT/out/build/zig-android/apk/arm64-v8a/"
    [ -n "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}" ] || [ ! -d "$HOME/Android/Sdk" ] || export ANDROID_HOME="$HOME/Android/Sdk"
    (cd "$ROOT/src/android" && ./gradlew -q assembleDebug -PnativeLibs="$ROOT/out/build/zig-android/apk")
    echo "      $ROOT/out/build/android/app/outputs/apk/debug/app-debug.apk (adb install -r it)"
fi

if [ "$HARNESS" = 1 ]; then
    echo "[+] headless harness"
    (cd "$ROOT" && JOBS="$JOBS" bash src/harness/build.sh -r . -g generated/game -o out/harness -n conker)
    echo "      $ROOT/out/harness/conker"
fi
