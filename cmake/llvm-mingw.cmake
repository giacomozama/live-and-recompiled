# Build the Windows port with the pinned llvm-mingw (tools/toolchains.cmake), on Linux or on Windows, with that host's
# build of it. The player package downloads the same release, so what is built here links with what players build.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/toolchains.cmake)
if(NOT LLVM_MINGW)
    if(DEFINED ENV{LLVM_MINGW})
        file(TO_CMAKE_PATH "$ENV{LLVM_MINGW}" LLVM_MINGW)
    else()
        set(LLVM_MINGW ${LLVM_MINGW_HOST_DIR})
    endif()
endif()
set(LLVM_MINGW ${LLVM_MINGW} CACHE PATH "llvm-mingw ${LLVM_MINGW_VERSION} for this host")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW)  # CMake's compiler checks run this file again
set(bin ${LLVM_MINGW}/bin)
if(NOT EXISTS ${bin}/x86_64-w64-mingw32-clang++${HOST_EXE})
    message(FATAL_ERROR "llvm-mingw not found at ${LLVM_MINGW}: run cmake -P tools/fetch-toolchain.cmake or set "
        "LLVM_MINGW")
endif()

set(CMAKE_C_COMPILER ${bin}/x86_64-w64-mingw32-clang${HOST_EXE})
set(CMAKE_CXX_COMPILER ${bin}/x86_64-w64-mingw32-clang++${HOST_EXE})
set(CMAKE_RC_COMPILER ${bin}/x86_64-w64-mingw32-windres${HOST_EXE})
set(CMAKE_AR ${bin}/llvm-ar${HOST_EXE})
set(CMAKE_RANLIB ${bin}/llvm-ranlib${HOST_EXE})

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# the runtime links "xaudio2" (the Windows SDK's name); mingw ships it as libxaudio2_8.a
set(XAUDIO2_SHIM ${CMAKE_BINARY_DIR}/shim)
file(MAKE_DIRECTORY ${XAUDIO2_SHIM})
file(COPY_FILE ${LLVM_MINGW}/x86_64-w64-mingw32/lib/libxaudio2_8.a ${XAUDIO2_SHIM}/libxaudio2.a ONLY_IF_DIFFERENT)

# the baseline CPU and Windows 10 (llvm-mingw's headers default to an older target); -static so the exe needs only system
# DLLs and XAudio2_8.dll
set(CMAKE_C_FLAGS_INIT "-march=x86-64-v2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${XAUDIO2_SHIM} -static")
