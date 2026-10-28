#!/bin/bash
# build.sh: the headless harness exe of one image, on Linux with the pinned Zig (tools/toolchains.cmake), as every build
#   -r repo (holding src/runtime/ and the game in src/conker/)   -g generated folder of the image   -o out folder
#   -n exe name (default: the generated folder's name)
#   -x regex of sources to leave out (repeatable)   -e extra source file (repeatable)
#   -G folder for the generated objects (default <out>/gen), shared between builds of the same generated code
# env: CXX (the pinned zig c++ for glibc 2.31; tools/fetch-toolchain.sh downloads it), XBE_TOOLCHAIN_ROOT, JOBS (nproc)
# Leaves out the window, SDL input and audio, XAudio2 and Vulkan parts of the runtime and the sources of the repo's other images
# (src/conker/<name> for each other folder beside the generated one); the harness supplies their stand ins.
set -e
EXCL=(); EXTRA=()
while getopts r:g:o:n:x:e:G: opt; do
    case $opt in
        r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) EXE=$OPTARG ;;
        x) EXCL+=("$OPTARG") ;; e) EXTRA+=("$(realpath "$OPTARG")") ;; G) GEN_OBJ=$OPTARG ;;
        *) sed -n 2,8p "$0"; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] || { sed -n 2,8p "$0"; exit 2; }
REPO=$(realpath "$REPO"); GEN=$(realpath "$GEN"); mkdir -p "$OUT"; OUT=$(realpath "$OUT")
EXE=${EXE:-$(basename "$GEN")}; GEN_OBJ=${GEN_OBJ:-$OUT/gen}; mkdir -p "$GEN_OBJ" "$OUT/host"
H=$(dirname "$(realpath "$0")"); RT=$REPO/src/runtime; GS=$REPO/src/conker
if [ -z "$CXX" ]; then
    ZIG_VERSION=$(sed -n 's/^set(ZIG_VERSION \(.*\))$/\1/p' "$REPO/tools/toolchains.cmake")
    ZIG=${XBE_TOOLCHAIN_ROOT:-$REPO/toolchain}/zig-$ZIG_VERSION-linux/zig
    [ -x "$ZIG" ] || { echo "missing $ZIG: run tools/fetch-toolchain.sh" >&2; exit 1; }
    CXX="$ZIG c++ -target x86_64-linux-gnu.2.31"  # cmake/zig-glibc.cmake's target
fi
JOBS=${JOBS:-$(nproc)}
BASE="-std=c++20 -mcpu=x86_64_v2 -fno-strict-aliasing"
HOST="$BASE -O1 -g -I$GS -I$RT -I$GEN -I$H -I$H/compat -DXBE_HEADLESS=1 -DXBE_RAW_VERTICES=0 -DXBE_RENDER_THREAD=0"

# generated code, an object rebuilt only when its partition is newer
export CXX BASE RT GEN GEN_OBJ
ls "$GEN"/x86_recomp.*.cpp "$GEN"/x86_func_mapping.cpp | xargs -P "$JOBS" -I{} sh -c '
    o="$GEN_OBJ/$(basename {} .cpp).o"; [ "$o" -nt {} ] && exit 0
    $CXX $BASE -O1 -g0 -w -I"$RT" -I"$GEN" -c {} -o "$o.tmp" && mv "$o.tmp" "$o"'

others=$(ls "$GEN/.." | grep -vx "$(basename "$GEN")" | paste -sd'|' -)
SRC=$(ls "$RT"/*/*.cpp | grep -Ev "/runtime/app/|/core/input.cpp|/gpu/(vulkan|backend).cpp|/audio/(xaudio2|sdl_audio).cpp")
SRC="$SRC $(ls "$GS"/*/*.cpp | grep -Ev "/src/conker/(${others:-^$})/" || true)"
SRC="$SRC $(ls "$H"/*.cpp) ${EXTRA[*]}"
for x in "${EXCL[@]}"; do SRC=$(echo $SRC | tr ' ' '\n' | grep -Ev -- "$x"); done

# host objects, keyed by the flags; rebuilt when the source or any header of the repo, harness or image is newer
stamp=$(echo "$HOST" | md5sum | cut -c1-8)
newest=$(find "$RT" "$GS" "$H" "$GEN/image_symbols.h" -name '*.h' -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2)
objs=()
for f in $SRC; do
    o=$OUT/host/$(echo "${f#/}" | tr / _ | sed "s/\.cpp$/.$stamp.o/"); objs+=("$o")
    [ "$o" -nt "$f" ] && [ "$o" -nt "$newest" ] && continue
    echo "compile $f"
    rm -f "$o"
    $CXX $HOST -c "$f" -o "$o" &
    [ "$(jobs -r | wc -l)" -ge "$JOBS" ] && wait -n
done
wait
$CXX -o "$OUT/$EXE" "${objs[@]}" "$GEN_OBJ"/*.o -lpthread
echo "built $OUT/$EXE"
