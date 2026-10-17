# configure.sh - the build folders for tools/build.sh, configured by tools/configure.cmake the way package.cmake does.
# Sourced.
#
#   $REL    game/out/build/release     native: xbe_recomp, the codegen tool of the other builds
#   $BUILD  game/out/build/llvm-mingw  Windows, with the pinned llvm-mingw: Game.exe and the player kit
#
# env overrides: LLVM_MINGW_ROOT (toolchains, default toolchain/), JOBS

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$ROOT/game"
REL="$GAME/out/build/release"
BUILD="$GAME/out/build/llvm-mingw"
JOBS="${JOBS:-$(nproc)}"

configure_rel() { cmake -DWHAT=release -P "$ROOT/tools/configure.cmake"; }
configure_mingw() { cmake -DWHAT=llvm-mingw -P "$ROOT/tools/configure.cmake"; }
