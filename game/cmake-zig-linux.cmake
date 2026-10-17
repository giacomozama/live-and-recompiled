# Build static Linux x86_64 programs with the pinned Zig (tools/toolchains.cmake), on Linux or on Windows: `zig cc` and
# `zig c++` against musl, so the result runs on any distro. A release's setup-linux and Linux xbe_recomp come from here.
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
# Zig's linker can't write the dependency files CMake would ask it for
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# the baseline CPU every build uses, static and stripped
set(CMAKE_C_FLAGS_INIT "-target x86_64-linux-musl -mcpu=x86_64_v2")
set(CMAKE_CXX_FLAGS_INIT "-target x86_64-linux-musl -mcpu=x86_64_v2")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -s")
