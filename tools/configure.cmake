# configure.cmake - the build folders tools/build.sh and tools/package.cmake share, configured the same way.
#
#   zig-glibc   the native Linux port with the pinned Zig against glibc, Linux only: xbe_recomp (the codegen tool of
#               every build there), the native Game, setup-linux and the player kit's Linux half
#   zig-windows the Windows port with the pinned Zig, on either host: Game.exe, the player kit's Windows half and
#               setup-windows.exe. On Windows it runs its own xbe_recomp
#   profile     zig-glibc with frame pointers, for perf (docs/performance.md); its own folder, never packaged
#
# Every build is the pinned Zig (fetched first if missing), so what is measured in one holds for the others: no system
# compiler, whose C and C++ libraries differ from the ones players get.
#
# Included, it defines xbe_configure(<folder>); run as cmake -DWHAT=<folder> -P tools/configure.cmake, it configures
# out/build/<folder>. A folder is configured once: delete it to configure it again.
include(${CMAKE_CURRENT_LIST_DIR}/toolchains.cmake)
get_filename_component(XBE_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(XBE_BUILDS ${XBE_ROOT}/out/build)

function(xbe_configure what)
    set(dir ${XBE_BUILDS}/${what})
    # the toolchain, also when configured already: the build needs it
    toolchain_fetch(ZIG ${TOOLCHAIN_HOST})
    if(EXISTS ${dir}/build.ninja)
        return()
    endif()
    # on Linux zig-glibc's own recompiler does the codegen of the others
    set(glibc -DCMAKE_TOOLCHAIN_FILE=${XBE_ROOT}/cmake/zig-glibc.cmake)
    set(codegen -DXBE_CODEGEN_TOOL=${XBE_BUILDS}/zig-glibc/xbe_recomp)
    if(what STREQUAL "zig-glibc")
        set(args ${glibc})
    elseif(what STREQUAL "profile")
        set(args ${glibc} "-DXBE_ZIG_EXTRA_FLAGS=-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer" ${codegen})
    elseif(what STREQUAL "zig-windows")
        set(args -DCMAKE_TOOLCHAIN_FILE=${XBE_ROOT}/cmake/zig-windows.cmake)
        if(NOT CMAKE_HOST_WIN32)
            list(APPEND args ${codegen})
        endif()
    else()
        message(FATAL_ERROR "xbe_configure: zig-glibc, zig-windows or profile, not '${what}'")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${XBE_ROOT} -B ${dir} -G Ninja -DCMAKE_BUILD_TYPE=Release ${args}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    xbe_configure(${WHAT})
endif()
