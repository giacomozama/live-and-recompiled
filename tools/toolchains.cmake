# toolchains.cmake - the pinned toolchains, fetched into toolchain/ ($LLVM_MINGW_ROOT) when needed:
#
#   zig         the compiler of the release, for Windows and for Linux: `zig c++` for x86_64-windows-gnu builds Game.exe,
#               setup-windows.exe and the player kit's Windows half; against glibc 2.31 the native Linux port,
#               setup-linux and the kit's Linux half. Setup downloads the same Zig on the player's machine to build the
#               game with it, so the prebuilt half and the player's own build of the game code come from one compiler
#               and one libc++. A new Zig needs a new player release too.
#   llvm-mingw  only llvm-ar and llvm-ranlib for those builds (CMake's archiver can't be `zig ar`, a command with a
#               subcommand), and the compiler of the Windows developer build (build.bat)
#
# Included by cmake/llvm-mingw.cmake, cmake/zig-windows.cmake, cmake/zig-glibc.cmake,
# fetch-toolchain.cmake, configure.cmake and package.cmake. To update one: change its version and both SHA-256s
# (llvm-mingw's release page and ziglang.org's download index list them), then delete the build folders that use it.

set(LLVM_MINGW_VERSION 20260922)
set(LLVM_MINGW_URL https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION})
set(LLVM_MINGW_linux_ARCHIVE llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz)
set(LLVM_MINGW_linux_SHA256 bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21)
set(LLVM_MINGW_windows_ARCHIVE llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-x86_64.zip)
set(LLVM_MINGW_windows_SHA256 e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666)

set(ZIG_VERSION 0.16.0)
set(ZIG_URL https://ziglang.org/download/${ZIG_VERSION})
set(ZIG_linux_ARCHIVE zig-x86_64-linux-${ZIG_VERSION}.tar.xz)
set(ZIG_linux_SHA256 70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00)
set(ZIG_windows_ARCHIVE zig-x86_64-windows-${ZIG_VERSION}.zip)
set(ZIG_windows_SHA256 68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e)

# this host, and where the toolchains go: $LLVM_MINGW_ROOT, default toolchain/ at the repository root
if(CMAKE_HOST_WIN32)
    set(TOOLCHAIN_HOST windows)
    set(HOST_EXE .exe)
else()
    set(TOOLCHAIN_HOST linux)
    set(HOST_EXE "")
endif()
if(DEFINED ENV{LLVM_MINGW_ROOT})
    file(TO_CMAKE_PATH "$ENV{LLVM_MINGW_ROOT}" TOOLCHAIN_ROOT)
else()
    get_filename_component(TOOLCHAIN_ROOT ${CMAKE_CURRENT_LIST_DIR}/../toolchain ABSOLUTE)
endif()
foreach(host linux windows)
    set(LLVM_MINGW_${host}_DIR ${TOOLCHAIN_ROOT}/llvm-mingw-${LLVM_MINGW_VERSION}-${host})
    set(ZIG_${host}_DIR ${TOOLCHAIN_ROOT}/zig-${ZIG_VERSION}-${host})
endforeach()
set(LLVM_MINGW_HOST_DIR ${LLVM_MINGW_${TOOLCHAIN_HOST}_DIR})
set(ZIG_HOST_DIR ${ZIG_${TOOLCHAIN_HOST}_DIR})

# toolchain_fetch(<LLVM_MINGW|ZIG> <linux|windows>): downloads that toolchain's archive for that host, checks its
# SHA-256 and unpacks it into its folder above, unless it is there already
function(toolchain_fetch tool host)
    set(out ${${tool}_${host}_DIR})
    if(EXISTS ${out})
        message(STATUS "have ${out}")
        return()
    endif()
    set(archive ${${tool}_${host}_ARCHIVE})
    set(expected ${${tool}_${host}_SHA256})
    set(dl ${TOOLCHAIN_ROOT}/.download-${archive})
    file(REMOVE_RECURSE ${dl})
    message(STATUS "downloading ${archive}")
    file(DOWNLOAD ${${tool}_URL}/${archive} ${dl}/${archive} SHOW_PROGRESS STATUS status)
    list(GET status 0 code)
    if(code)
        list(GET status 1 why)
        file(REMOVE_RECURSE ${dl})
        message(FATAL_ERROR "downloading ${archive} failed: ${why}")
    endif()
    file(SHA256 ${dl}/${archive} sha)
    if(NOT sha STREQUAL "${expected}")
        file(REMOVE_RECURSE ${dl})
        message(FATAL_ERROR "${archive} is not the pinned file (sha256 ${sha}, expected ${expected})")
    endif()
    message(STATUS "unpacking ${archive}")
    file(ARCHIVE_EXTRACT INPUT ${dl}/${archive} DESTINATION ${dl}/unpacked)
    string(REGEX REPLACE "\\.(tar\\.xz|zip)$" "" top ${archive})
    file(RENAME ${dl}/unpacked/${top} ${out})
    file(REMOVE_RECURSE ${dl})
    message(STATUS "installed ${out}")
endfunction()
