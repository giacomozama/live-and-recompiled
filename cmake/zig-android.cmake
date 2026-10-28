# Build the Android port with the pinned Zig (tools/toolchains.cmake): `zig c++` for aarch64 Android against the NDK's
# sysroot (Bionic's headers and the system libraries' link stubs), with Zig's own libc++ static, as the other builds
# have it. The game is a shared library, libmain.so, that SDL's Java side loads; the player kit's Android half is built
# here (add_xbe_android_kit), and the APK builder on the player's PC links the game code with the same Zig against the
# sysroot the kit carries.
#
# CMake's own Android support drives the NDK's compiler and adds its flags, so this is a Linux build with ANDROID set,
# which is what SDL's build and ours ask. The NDK is $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT, or the newest under
# $ANDROID_HOME/ndk (or ~/Android/Sdk/ndk); only its sysroot is used, never its compiler.
#
# API 28: Zig links an executable for API 29 and later statically, which CMake's compiler checks trip on; a shared
# library is the same either way, and the game asks for nothing newer.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(ANDROID TRUE)
set(XBE_ANDROID_API 28)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/toolchains.cmake)
if(NOT EXISTS ${ZIG_HOST_DIR}/zig${HOST_EXE})
    message(FATAL_ERROR "zig missing in ${TOOLCHAIN_ROOT}: run cmake -P tools/fetch-toolchain.cmake")
endif()

if(NOT XBE_ANDROID_SYSROOT)
    set(ndk "")
    foreach(var ANDROID_NDK_HOME ANDROID_NDK_ROOT)
        if(NOT ndk AND DEFINED ENV{${var}} AND EXISTS "$ENV{${var}}")
            set(ndk "$ENV{${var}}")
        endif()
    endforeach()
    if(NOT ndk)
        set(sdks "$ENV{ANDROID_HOME}" "$ENV{ANDROID_SDK_ROOT}" "$ENV{HOME}/Android/Sdk")
        foreach(sdk ${sdks})
            if(NOT ndk AND sdk AND IS_DIRECTORY "${sdk}/ndk")
                file(GLOB versions LIST_DIRECTORIES TRUE "${sdk}/ndk/*")
                list(SORT versions COMPARE NATURAL ORDER DESCENDING)
                if(versions)
                    list(GET versions 0 ndk)
                endif()
            endif()
        endforeach()
    endif()
    file(GLOB sysroot LIST_DIRECTORIES TRUE "${ndk}/toolchains/llvm/prebuilt/*/sysroot")
    if(NOT ndk OR NOT sysroot)
        message(FATAL_ERROR "No Android NDK found: set ANDROID_NDK_HOME, or XBE_ANDROID_SYSROOT to an NDK's sysroot")
    endif()
    list(GET sysroot 0 sysroot)
    set(XBE_ANDROID_SYSROOT ${sysroot} CACHE PATH "The NDK sysroot the Android build compiles and links against")
endif()

# Zig reads where libc is from a file named by ZIG_LIBC; the compiler and the linker are started through `cmake -E env`
# with it set, so a plain ninja run has it too (and CMake's own checks, which read this file again)
set(XBE_ANDROID_LIBC ${CMAKE_BINARY_DIR}/zig-android-libc.txt)
file(WRITE ${XBE_ANDROID_LIBC} "include_dir=${XBE_ANDROID_SYSROOT}/usr/include
sys_include_dir=${XBE_ANDROID_SYSROOT}/usr/include/aarch64-linux-android
crt_dir=${XBE_ANDROID_SYSROOT}/usr/lib/aarch64-linux-android/${XBE_ANDROID_API}
msvc_lib_dir=
kernel32_lib_dir=
gcc_dir=
")
foreach(lang C CXX)
    set(CMAKE_${lang}_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env ZIG_LIBC=${XBE_ANDROID_LIBC})
    set(CMAKE_${lang}_LINKER_LAUNCHER ${CMAKE_COMMAND} -E env ZIG_LIBC=${XBE_ANDROID_LIBC})
endforeach()
set(ENV{ZIG_LIBC} ${XBE_ANDROID_LIBC})

set(CMAKE_C_COMPILER ${ZIG_HOST_DIR}/zig${HOST_EXE} cc)
set(CMAKE_CXX_COMPILER ${ZIG_HOST_DIR}/zig${HOST_EXE} c++)
set(CMAKE_USER_MAKE_RULES_OVERRIDE ${CMAKE_CURRENT_LIST_DIR}/zig-rules.cmake)  # `zig ar` for the static libraries
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)
# libraries looked for (SDL's OpenSLES, log, android) are the sysroot's
set(CMAKE_FIND_ROOT_PATH ${XBE_ANDROID_SYSROOT})
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-android)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ARMv8.2: every phone with Vulkan 1.3 has it, and it brings the atomics without the outline calls; -fPIC on the compile
# line itself, as the player kit's compile of the game code (add_xbe_android_kit) takes these flags as they are
set(XBE_ZIG_TARGET aarch64-linux-android.${XBE_ANDROID_API})
set(CMAKE_C_FLAGS_INIT "-target ${XBE_ZIG_TARGET} -mcpu=generic+v8_2a -fPIC -g0 -Wno-nullability-completeness")
set(CMAKE_CXX_FLAGS_INIT "-target ${XBE_ZIG_TARGET} -mcpu=generic+v8_2a -fPIC -g0 -Wno-nullability-completeness")
# Zig looks for the -l libraries only where it is told to, the libc file's crt_dir is not one of those places
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L${XBE_ANDROID_SYSROOT}/usr/lib/aarch64-linux-android/${XBE_ANDROID_API}")
