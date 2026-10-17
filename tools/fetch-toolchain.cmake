# fetch-toolchain.cmake - download pinned toolchains (tools/toolchains.cmake) into toolchain/, on Linux or Windows.
#
#   cmake -P tools/fetch-toolchain.cmake                          llvm-mingw for this host
#   cmake -DTOOLS="llvm-mingw;zig" -P tools/fetch-toolchain.cmake  the ones named (zig: what packaging uses)
#   cmake -DHOSTS="linux;windows" -P tools/fetch-toolchain.cmake   for the hosts named (Setup downloads its own; this is
#                                                                  for trying a kit by hand)
#
# Installs into $LLVM_MINGW_ROOT (default: toolchain/ at the repository root) as <tool>-<version>-<host>/.
include(${CMAKE_CURRENT_LIST_DIR}/toolchains.cmake)
if(NOT TOOLS)
    set(TOOLS llvm-mingw)
endif()
if(NOT HOSTS)
    set(HOSTS ${TOOLCHAIN_HOST})
endif()
foreach(tool ${TOOLS})
    if(tool STREQUAL "llvm-mingw")
        set(tool LLVM_MINGW)
    elseif(tool STREQUAL "zig")
        set(tool ZIG)
    else()
        message(FATAL_ERROR "TOOLS: llvm-mingw or zig, not '${tool}'")
    endif()
    foreach(host ${HOSTS})
        toolchain_fetch(${tool} ${host})
    endforeach()
endforeach()
