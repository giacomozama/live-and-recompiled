# Build the Windows port with the pinned Zig (tools/toolchains.cmake), on Linux or on Windows: `zig c++` for
# x86_64-windows-gnu, with Zig's own mingw-w64 and static libc++. Game.exe for development (tools/build.bat,
# tools/build.sh) and the player kit's Windows half are built here, and Setup on Windows downloads the same Zig, so its
# build of the game code links with it: one compiler for every build and both systems.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/toolchains.cmake)
if(NOT EXISTS ${ZIG_HOST_DIR}/zig${HOST_EXE})
    message(FATAL_ERROR "zig missing in ${TOOLCHAIN_ROOT}: run cmake -P tools/fetch-toolchain.cmake")
endif()

set(CMAKE_C_COMPILER ${ZIG_HOST_DIR}/zig${HOST_EXE} cc)
set(CMAKE_CXX_COMPILER ${ZIG_HOST_DIR}/zig${HOST_EXE} c++)
set(CMAKE_USER_MAKE_RULES_OVERRIDE ${CMAKE_CURRENT_LIST_DIR}/zig-rules.cmake)  # `zig ar` for the static libraries
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# the baseline CPU and Windows 10 (the mingw-w64 headers default to an older target); -g0: Zig writes debug info by
# default. The runtime links "xaudio2" (the Windows SDK's name); mingw-w64 ships it as xaudio2_8.
set(XBE_ZIG_TARGET x86_64-windows-gnu)
set(XBE_XAUDIO2 xaudio2_8)
set(CMAKE_C_FLAGS_INIT "-target ${XBE_ZIG_TARGET} -mcpu=x86_64_v2 -g0 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-target ${XBE_ZIG_TARGET} -mcpu=x86_64_v2 -g0 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
