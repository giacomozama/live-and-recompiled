# configure.cmake - the build folders tools/build.sh and tools/package.cmake share, configured the same way.
#
#   release     native, Linux only: xbe_recomp, the codegen tool of the other builds there, and the native Linux port
#   llvm-mingw  Windows, with the pinned llvm-mingw (fetched first if missing): Game.exe and the player kit. On Windows
#               it runs its own xbe_recomp
#   zig-linux   static Linux programs with the pinned Zig (musl), on either host: setup-linux and xbe_recomp for
#               players
#   zig-glibc   the native Linux port with the pinned Zig against glibc, Linux only: the player kit's Linux half
#
# Included, it defines xbe_configure(<folder>); run as cmake -DWHAT=<folder> -P tools/configure.cmake, it configures
# game/out/build/<folder>. A folder is configured once: delete it to configure it again.
include(${CMAKE_CURRENT_LIST_DIR}/toolchains.cmake)
get_filename_component(XBE_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(XBE_GAME ${XBE_ROOT}/game)
set(XBE_BUILDS ${XBE_GAME}/out/build)

function(xbe_configure what)
    set(dir ${XBE_BUILDS}/${what})
    # the toolchains, also when configured already: the build needs them
    if(what MATCHES "^(llvm-mingw|zig-linux|zig-glibc)$")
        toolchain_fetch(LLVM_MINGW ${TOOLCHAIN_HOST})
    endif()
    if(what MATCHES "^zig-")
        toolchain_fetch(ZIG ${TOOLCHAIN_HOST})
    endif()
    if(EXISTS ${dir}/build.ninja)
        return()
    endif()
    # the native compiler and the baseline CPU every build uses; on Linux the native recompiler does the codegen
    set(native -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2")
    set(codegen -DXBE_CODEGEN_TOOL=${XBE_BUILDS}/release/xbe_recomp)
    if(what STREQUAL "release")
        set(args ${native} ${codegen})
    elseif(what STREQUAL "zig-linux")
        # only its xbe_setup and xbe_recomp are built, so its codegen setting is never used
        set(args -DCMAKE_TOOLCHAIN_FILE=${XBE_GAME}/cmake-zig-linux.cmake)
    elseif(what STREQUAL "zig-glibc")
        set(args -DCMAKE_TOOLCHAIN_FILE=${XBE_GAME}/cmake-zig-glibc.cmake ${codegen})
    elseif(what STREQUAL "llvm-mingw")
        set(args -DCMAKE_TOOLCHAIN_FILE=${XBE_GAME}/cmake-llvm-mingw.cmake -DLLVM_MINGW=${LLVM_MINGW_HOST_DIR})
        if(NOT CMAKE_HOST_WIN32)
            list(APPEND args ${codegen})
        endif()
    else()
        message(FATAL_ERROR "xbe_configure: release, llvm-mingw, zig-linux or zig-glibc, not '${what}'")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${XBE_GAME} -B ${dir} -G Ninja -DCMAKE_BUILD_TYPE=Release ${args}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    xbe_configure(${WHAT})
endif()
