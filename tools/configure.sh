# configure.sh - the build folders for tools/build.sh, configured by tools/configure.cmake the way package.cmake does.
# Sourced.
#
#   $REL    out/build/release     native: xbe_recomp, the codegen tool of the other builds
#   $BUILD  out/build/zig-windows Windows, with the pinned Zig: Game.exe and the player kit's Windows half
#
# env overrides: XBE_TOOLCHAIN_ROOT (the toolchain, default toolchain/), JOBS

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="$ROOT/out/build/release"
BUILD="$ROOT/out/build/zig-windows"
JOBS="${JOBS:-$(nproc)}"

configure_rel() { cmake -DWHAT=release -P "$ROOT/tools/configure.cmake"; }
configure_windows() { cmake -DWHAT=zig-windows -P "$ROOT/tools/configure.cmake"; }
