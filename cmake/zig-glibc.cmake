# Build the native Linux port with the pinned Zig (tools/toolchains.cmake): `zig c++` against glibc 2.31, linked
# dynamically, with Zig's libc++ static, so the game needs only glibc and runs on any distro since 2020. The player
# kit's Linux half is built here, and Setup on Linux downloads the same Zig, so its build of the game code links with
# it.
# llvm-ar archives the static libraries (CMake's archiver can't be `zig ar`, a command with a subcommand).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/toolchains.cmake)
if(NOT EXISTS ${ZIG_HOST_DIR}/zig${HOST_EXE} OR NOT EXISTS ${LLVM_MINGW_HOST_DIR}/bin/llvm-ar${HOST_EXE})
    message(FATAL_ERROR "zig or llvm-mingw missing in ${TOOLCHAIN_ROOT}: run "
        "cmake -DTOOLS=\"llvm-mingw;zig\" -P tools/fetch-toolchain.cmake")
endif()

set(CMAKE_C_COMPILER ${ZIG_HOST_DIR}/zig${HOST_EXE} cc)
set(CMAKE_CXX_COMPILER ${ZIG_HOST_DIR}/zig${HOST_EXE} c++)
set(CMAKE_AR ${LLVM_MINGW_HOST_DIR}/bin/llvm-ar${HOST_EXE})
set(CMAKE_RANLIB ${LLVM_MINGW_HOST_DIR}/bin/llvm-ranlib${HOST_EXE})
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# Zig's libc headers, not this system's, so the program asks for no glibc newer than its target. SDL, the only C in the
# build, finds the X11, Wayland and audio headers of the libraries it loads at run time after them, in /usr/include
# (-idirafter also makes CMake count that folder as the compiler's own, so it never puts it first). The C++ flags stay
# without it: they are the compile line of the player's build too (add_xbe_player_kit).
# -g0: Zig writes debug info by default, which would make the game eight times its size.
set(XBE_ZIG_TARGET x86_64-linux-gnu.2.31)
set(CMAKE_C_FLAGS_INIT "-target ${XBE_ZIG_TARGET} -mcpu=x86_64_v2 -g0 -idirafter /usr/include")
set(CMAKE_CXX_FLAGS_INIT "-target ${XBE_ZIG_TARGET} -mcpu=x86_64_v2 -g0")
