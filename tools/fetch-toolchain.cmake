# fetch-toolchain.cmake - download the pinned llvm-mingw (tools/llvm-mingw.cmake) into toolchain/, on Linux or Windows.
#
#   cmake -P tools/fetch-toolchain.cmake                       this host's toolchain
#   cmake -DHOSTS="linux;windows" -P tools/fetch-toolchain.cmake  the ones named (Setup downloads its own; this is for
#                                                              trying a kit by hand)
#
# Installs into $LLVM_MINGW_ROOT (default: toolchain/ at the repository root) as llvm-mingw-<version>-<host>/.
include(${CMAKE_CURRENT_LIST_DIR}/llvm-mingw.cmake)
if(NOT HOSTS)
    set(HOSTS ${LLVM_MINGW_HOST})
endif()
foreach(host ${HOSTS})
    llvm_mingw_fetch(${host})
endforeach()
