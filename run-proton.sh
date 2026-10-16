#!/usr/bin/env bash
# run-proton.sh - build the Windows/Proton target and run it under GE-Proton via umu-run.
#
#   ./run-proton.sh                build (incremental) then launch
#   ./run-proton.sh --no-build     just launch the existing winrun/Game.exe
#   ./run-proton.sh --build-only   build and stop
#   ./run-proton.sh --skip-fmv     launch with XBE_SKIP_FMV: cutscenes report failed and are skipped
#   ./run-proton.sh --skippable-cutscenes   launch with XBE_SKIPPABLE_CUTSCENES: LT skips any in-engine cutscene,
#                                  seen or not
#
# env overrides: PROTONPATH, GAMEID, JOBS, UMU (umu-run, default: the one on PATH)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME="$ROOT/game"
WIN="$GAME/out/build/win"
RUN="$GAME/winrun"
XRE="$GAME/out/build/release/xbe_recomp"
PROTON="${PROTONPATH:-$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-7}"
UMU="${UMU:-umu-run}"
JOBS="${JOBS:-$(nproc)}"

export PROTON_ENABLE_WAYLAND=1

DO_BUILD=1
DO_RUN=1
SKIP_FMV=0
SKIPPABLE=0
for a in "$@"; do
    case "$a" in
        --no-build)  DO_BUILD=0 ;;
        --build-only) DO_RUN=0 ;;
        --skip-fmv)  SKIP_FMV=1 ;;
        --skippable-cutscenes) SKIPPABLE=1 ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

command -v "$UMU" >/dev/null || { echo "missing umu-run: install umu-launcher or set UMU" >&2; exit 1; }
[ -d "$PROTON" ] || { echo "missing Proton: $PROTON" >&2; exit 1; }

if [ "$DO_BUILD" = 1 ]; then
    # the runtime links "xaudio2"; mingw ships libxaudio2_8.a
    mkdir -p "$GAME/third_party/mingw/lib"
    ln -sf /usr/x86_64-w64-mingw32/lib/libxaudio2_8.a "$GAME/third_party/mingw/lib/libxaudio2.a"

    [ -x "$XRE" ] || { echo "missing native codegen tool: $XRE" >&2; exit 1; }

    if [ ! -f "$WIN/build.ninja" ]; then
        echo "== configuring the Windows build =="
        cmake -S "$GAME" -B "$WIN" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="$GAME/cmake-mingw.cmake" \
            -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
            -DCMAKE_EXE_LINKER_FLAGS="-L$GAME/third_party/mingw/lib -municode -static -lwinpthread" \
            -DXBE_CODEGEN_TOOL="$XRE"
    fi

    echo "== building Game.exe =="
    cmake --build "$WIN" --target Game -j "$JOBS"

    mkdir -p "$RUN"
    cp -f "$WIN/Game.exe" "$RUN/Game.exe"
    [ -e "$RUN/assets" ] || ln -sf ../assets "$RUN/assets"
fi

if [ "$DO_RUN" = 0 ]; then
    echo "built $RUN/Game.exe"
    exit 0
fi

[ -f "$RUN/Game.exe" ] || { echo "missing $RUN/Game.exe (run without --no-build)" >&2; exit 1; }

echo "== launching under $(basename "$PROTON") =="
SKIP_ENV=()
if [ "$SKIP_FMV" = 1 ]; then
    SKIP_ENV=(XBE_SKIP_FMV=1)
    echo "   XBE_SKIP_FMV=1 (cutscenes skipped)"
fi
if [ "$SKIPPABLE" = 1 ]; then
    SKIP_ENV+=(XBE_SKIPPABLE_CUTSCENES=1)
    echo "   XBE_SKIPPABLE_CUTSCENES=1 (LT skips any in-engine cutscene)"
fi
echo "   logs: $RUN/logs/game.log   (display required)"
cd "$RUN"
exec env WINEPREFIX="$GAME/prefix" PROTONPATH="$PROTON" GAMEID="${GAMEID:-conker-lar}" \
    "${SKIP_ENV[@]}" "$UMU" "$RUN/Game.exe"
