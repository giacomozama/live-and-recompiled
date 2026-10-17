# configure.sh - the build folders tools/build.sh and tools/package.sh share, configured the same way. Sourced.
#
#   $REL    game/out/build/release     native: xbe_recomp, the codegen tool of the other builds
#   $BUILD  game/out/build/llvm-mingw  Windows, with the pinned llvm-mingw: Game.exe and the player kit
#   $LINUX  game/out/build/linux-kit   native and static: setup-linux and xbe_recomp for players
#
# Each configure_* function configures its folder once; delete the folder to configure it again.
# env overrides: LLVM_MINGW_ROOT (toolchains, default toolchain/), JOBS

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$ROOT/game"
VERSION_TC="$(sed -n 's/^VERSION=//p' "$ROOT/tools/fetch-toolchain.sh")"
TC="${LLVM_MINGW_ROOT:-$ROOT/toolchain}"
LIN="$TC/llvm-mingw-$VERSION_TC-linux"
REL="$GAME/out/build/release"
BUILD="$GAME/out/build/llvm-mingw"
LINUX="$GAME/out/build/linux-kit"
JOBS="${JOBS:-$(nproc)}"

# the native compiler and the baseline CPU every build uses
NATIVE_FLAGS=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2")

configure_rel() {
    [ -f "$REL/build.ninja" ] && return
    cmake -S "$GAME" -B "$REL" -G Ninja -DCMAKE_BUILD_TYPE=Release "${NATIVE_FLAGS[@]}" \
        -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
}

configure_mingw() {
    [ -f "$BUILD/build.ninja" ] && return
    cmake -S "$GAME" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$GAME/cmake-llvm-mingw.cmake" -DLLVM_MINGW="$LIN" \
        -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
}

configure_linux_kit() {
    [ -f "$LINUX/build.ninja" ] && return
    cmake -S "$GAME" -B "$LINUX" -G Ninja -DCMAKE_BUILD_TYPE=Release "${NATIVE_FLAGS[@]}" \
        -DCMAKE_EXE_LINKER_FLAGS="-static" -DXBE_CODEGEN_TOOL="$REL/xbe_recomp"
}
