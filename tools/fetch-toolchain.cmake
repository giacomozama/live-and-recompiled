# fetch-toolchain.cmake - download the pinned Zig (tools/toolchains.cmake) into toolchain/, on Linux or Windows.
#
#   cmake -P tools/fetch-toolchain.cmake                          Zig for this host
#   cmake -DHOSTS="linux;windows" -P tools/fetch-toolchain.cmake   for the hosts named (Setup downloads its own; this is
#                                                                  for trying a kit by hand)
#
# Installs into $XBE_TOOLCHAIN_ROOT (default: toolchain/ at the repository root) as zig-<version>-<host>/.
include(${CMAKE_CURRENT_LIST_DIR}/toolchains.cmake)
if(NOT HOSTS)
    set(HOSTS ${TOOLCHAIN_HOST})
endif()
foreach(host ${HOSTS})
    toolchain_fetch(ZIG ${host})
endforeach()
