# llvm-mingw.cmake - the pinned llvm-mingw. The Windows build, the player kit and Setup all use this release: the
# prebuilt half of a release and the player's own build of the game code must come from the same compiler and runtime
# libraries. Included by game/cmake-llvm-mingw.cmake, fetch-toolchain.cmake, configure.cmake and package.cmake.
#
# To update: change the version and both SHA-256s (the release page lists them), then delete game/out/build/llvm-mingw.
# Players need a new release, since the host library and the kit's compiler must match.

set(LLVM_MINGW_VERSION 20260922)
set(LLVM_MINGW_URL https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION})
set(LLVM_MINGW_linux_ARCHIVE llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz)
set(LLVM_MINGW_linux_SHA256 bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21)
set(LLVM_MINGW_windows_ARCHIVE llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-x86_64.zip)
set(LLVM_MINGW_windows_SHA256 e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666)

# where the toolchains go ($LLVM_MINGW_ROOT, default toolchain/ at the repository root) and the one for this host
if(CMAKE_HOST_WIN32)
    set(LLVM_MINGW_HOST windows)
    set(LLVM_MINGW_EXE .exe)
else()
    set(LLVM_MINGW_HOST linux)
    set(LLVM_MINGW_EXE "")
endif()
if(DEFINED ENV{LLVM_MINGW_ROOT})
    file(TO_CMAKE_PATH "$ENV{LLVM_MINGW_ROOT}" LLVM_MINGW_ROOT)
else()
    get_filename_component(LLVM_MINGW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../toolchain ABSOLUTE)
endif()
set(LLVM_MINGW_HOST_DIR ${LLVM_MINGW_ROOT}/llvm-mingw-${LLVM_MINGW_VERSION}-${LLVM_MINGW_HOST})

# llvm_mingw_fetch(<linux|windows>): downloads that host's archive, checks its SHA-256 and unpacks it into
# LLVM_MINGW_ROOT/llvm-mingw-<version>-<host>, unless it is there already
function(llvm_mingw_fetch host)
    set(out ${LLVM_MINGW_ROOT}/llvm-mingw-${LLVM_MINGW_VERSION}-${host})
    if(EXISTS ${out})
        message(STATUS "have ${out}")
        return()
    endif()
    set(archive ${LLVM_MINGW_${host}_ARCHIVE})
    set(dl ${LLVM_MINGW_ROOT}/.download-${host})
    file(REMOVE_RECURSE ${dl})
    message(STATUS "downloading ${archive}")
    file(DOWNLOAD ${LLVM_MINGW_URL}/${archive} ${dl}/${archive} SHOW_PROGRESS STATUS status)
    list(GET status 0 code)
    if(code)
        list(GET status 1 why)
        file(REMOVE_RECURSE ${dl})
        message(FATAL_ERROR "downloading ${archive} failed: ${why}")
    endif()
    file(SHA256 ${dl}/${archive} sha)
    if(NOT sha STREQUAL "${LLVM_MINGW_${host}_SHA256}")
        file(REMOVE_RECURSE ${dl})
        message(FATAL_ERROR "${archive} is not the pinned file (sha256 ${sha}, expected ${LLVM_MINGW_${host}_SHA256})")
    endif()
    message(STATUS "unpacking ${archive}")
    file(ARCHIVE_EXTRACT INPUT ${dl}/${archive} DESTINATION ${dl}/unpacked)
    string(REGEX REPLACE "\\.(tar\\.xz|zip)$" "" top ${archive})
    file(RENAME ${dl}/unpacked/${top} ${out})
    file(REMOVE_RECURSE ${dl})
    message(STATUS "installed ${out}")
endfunction()
