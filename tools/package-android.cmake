# package-android.cmake - build the Android release, on Linux or Windows: apk-builder-linux and
# apk-builder-windows.exe, each with the Android kit inside it.
#
# Players build the app on their PC from their own copy, as with Setup: the APK builder takes the game's default.xbe
# from where it is pointed, downloads the pinned Zig for its host, recompiles the game, compiles the result for 64-bit
# ARM Android, links libmain.so and puts it in the base APK this script builds with Gradle, signed with the player's
# own key. The game's files go onto the device separately; the app unpacks them there the first time it runs. Nothing
# in the release is the title's code, or the compiler.
#
#   cmake -P tools/package-android.cmake                    build the kit, test it against the game in assets/, zip it
#   cmake -DTEST=OFF -P tools/package-android.cmake         skip the test
#   cmake -DTEST_DOWNLOAD=ON -P tools/package-android.cmake test with the compiler the builder downloads
#   cmake -DJOBS=N -P tools/package-android.cmake           parallel jobs (default: every core)
# tools/package-android.sh and tools/package-android.bat run it with the same options as flags.
#
# Needs, besides what tools/package.cmake does: an Android NDK (for its sysroot, cmake/zig-android.cmake finds it), the
# Android SDK with platform 36 ($ANDROID_HOME, default ~/Android/Sdk) and a JDK 17 to 21 for Gradle ($JAVA_HOME).
#
# Output: out/release/LiveAndRecompiled-android/, out/release/LiveAndRecompiled-<version>-android-builder-windows.zip
# and, packaged on Linux, out/release/LiveAndRecompiled-<version>-android-builder-linux.tar.gz
cmake_minimum_required(VERSION 3.25)
include(${CMAKE_CURRENT_LIST_DIR}/configure.cmake)

set(OUT ${XBE_ROOT}/out/release)
set(NAME LiveAndRecompiled)
if(NOT DEFINED TEST)
    set(TEST ON)
endif()
if(NOT JOBS)
    cmake_host_system_information(RESULT JOBS QUERY NUMBER_OF_LOGICAL_CORES)
endif()

function(build dir)
    execute_process(COMMAND ${CMAKE_COMMAND} --build ${XBE_BUILDS}/${dir} --target ${ARGN} -j ${JOBS}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

execute_process(COMMAND git -C ${XBE_ROOT} describe --tags --always --dirty OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE code ERROR_QUIET)
if(code OR NOT version)
    set(version dev)
endif()
# the app's version code, which must grow from one release to the next for Android to install it as an update
execute_process(COMMAND git -C ${XBE_ROOT} rev-list --count HEAD OUTPUT_VARIABLE version_code
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE code ERROR_QUIET)
if(code OR NOT version_code)
    set(version_code 1)
endif()

message(STATUS "[1/6] toolchain")
toolchain_fetch(ZIG ${TOOLCHAIN_HOST})
if(NOT EXISTS ${XBE_ROOT}/assets/default.xbe)
    message(FATAL_ERROR "no game in assets/: put the supported release there first (see README.md)")
endif()

message(STATUS "[2/6] the builders and the kit")
# the recompilers and the builders for both hosts; the codegen tool first (zig-glibc's on Linux, zig-windows' on
# Windows), then the Android build that uses it
if(NOT CMAKE_HOST_WIN32)
    xbe_configure(zig-glibc)
    build(zig-glibc xbe_recomp apkbuilder)
endif()
xbe_configure(zig-windows)
build(zig-windows xbe_recomp apkbuilder)
xbe_configure(zig-android)
build(zig-android Game_android_kit)

message(STATUS "[3/6] the base APK")
# the app without libmain.so, which the builder adds; Gradle with the SDK and JDK the environment names
if(CMAKE_HOST_WIN32)
    set(gradlew ${XBE_ROOT}/src/android/gradlew.bat)
else()
    set(gradlew ${XBE_ROOT}/src/android/gradlew)
endif()
if(NOT DEFINED ENV{ANDROID_HOME} AND NOT DEFINED ENV{ANDROID_SDK_ROOT} AND EXISTS "$ENV{HOME}/Android/Sdk")
    set(ENV{ANDROID_HOME} "$ENV{HOME}/Android/Sdk")
endif()
set(sdl_java ${XBE_BUILDS}/zig-android/_deps/sdl3-src/android-project/app/src/main/java)
execute_process(COMMAND ${gradlew} --no-daemon -q assembleRelease -PsdlJava=${sdl_java} -PversionName=${version}
        -PversionCode=${version_code}
    WORKING_DIRECTORY ${XBE_ROOT}/src/android COMMAND_ERROR_IS_FATAL ANY)
set(base_apk ${XBE_BUILDS}/android/app/outputs/apk/release/app-release-unsigned.apk)
if(NOT EXISTS ${base_apk})
    message(FATAL_ERROR "Gradle made no ${base_apk}")
endif()

set(DEST ${OUT}/${NAME}-android)
message(STATUS "[4/6] staging ${DEST}")
file(REMOVE_RECURSE ${DEST})
file(COPY ${XBE_BUILDS}/zig-android/package/Game-android/ DESTINATION ${DEST})
file(COPY ${XBE_ROOT}/LICENSE DESTINATION ${DEST})
file(MAKE_DIRECTORY ${DEST}/kit/apk)
file(COPY_FILE ${base_apk} ${DEST}/kit/apk/base.apk)
file(MAKE_DIRECTORY ${DEST}/kit/bin)
file(COPY ${XBE_BUILDS}/zig-windows/xbe_recomp.exe ${XBE_BUILDS}/zig-windows/apk-builder-windows.exe
    DESTINATION ${DEST}/kit/bin)
if(NOT CMAKE_HOST_WIN32)
    file(COPY ${XBE_BUILDS}/zig-glibc/xbe_recomp DESTINATION ${DEST}/kit/bin)
    file(COPY ${XBE_BUILDS}/zig-glibc/apk-builder-linux DESTINATION ${DEST})
endif()
file(RENAME ${DEST}/kit/bin/apk-builder-windows.exe ${DEST}/apk-builder-windows.exe)
# no compiler in the release: the builder downloads the Zig of the same release for its host
set(toml "\n# the compiler (Zig ${ZIG_VERSION}) for the host the builder runs on, downloaded into work/toolchain/android-<host> once\n")
foreach(host linux windows)
    string(APPEND toml "[toolchain.${host}]\nurl = \"${ZIG_URL}/${ZIG_${host}_ARCHIVE}\"\nsha256 = \"${ZIG_${host}_SHA256}\"\n")
endforeach()
file(APPEND ${DEST}/kit/android.toml "${toml}")

# the kit goes inside each builder: a zip of its files with this host's recompiler, appended to the executable and
# then a footer of "XBEAPK01" and the zip's size in 16 decimal digits (the builder unpacks it into its working folder)
function(embed_kit builder recomp)
    set(stage ${OUT}/.apk-kit)
    file(REMOVE_RECURSE ${stage})
    file(MAKE_DIRECTORY ${stage}/bin)
    foreach(f kit.toml android.toml)
        file(COPY ${DEST}/kit/${f} DESTINATION ${stage})
    endforeach()
    foreach(d config xdk include lib sysroot apk)
        file(COPY ${DEST}/kit/${d} DESTINATION ${stage})
    endforeach()
    file(COPY ${DEST}/kit/bin/${recomp} DESTINATION ${stage}/bin)
    set(zip ${OUT}/.apk-kit.zip)
    file(REMOVE ${zip})
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf ${zip} --format=zip kit.toml android.toml config xdk include lib
            sysroot apk bin
        WORKING_DIRECTORY ${stage} COMMAND_ERROR_IS_FATAL ANY)
    file(SIZE ${zip} size)
    string(LENGTH "${size}" digits)
    math(EXPR pad "16 - ${digits}")
    string(REPEAT "0" ${pad} zeros)
    file(WRITE ${OUT}/.apk-kit-footer "XBEAPK01${zeros}${size}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E cat ${DEST}/${builder} ${zip} ${OUT}/.apk-kit-footer
        OUTPUT_FILE ${DEST}/.${builder} COMMAND_ERROR_IS_FATAL ANY)
    file(RENAME ${DEST}/.${builder} ${DEST}/${builder})
    if(NOT builder MATCHES "\\.exe$")
        file(CHMOD ${DEST}/${builder} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif()
    file(REMOVE_RECURSE ${stage})
    file(REMOVE ${zip} ${OUT}/.apk-kit-footer)
endfunction()
embed_kit(apk-builder-windows.exe xbe_recomp.exe)
if(EXISTS ${DEST}/apk-builder-linux)
    embed_kit(apk-builder-linux xbe_recomp)
endif()
file(REMOVE_RECURSE ${DEST}/kit)

message(STATUS "[5/6] test")
if(TEST)
    # a player's steps on this host: the release's builder, the game's default.xbe, this host's compiler from
    # toolchain/ (or, with TEST_DOWNLOAD, the one the builder downloads), and a key of the test's own
    string(RANDOM LENGTH 6 tag)
    set(scratch ${OUT}/.test-android-${tag})
    file(MAKE_DIRECTORY ${scratch}/assets)
    file(COPY_FILE ${XBE_ROOT}/assets/default.xbe ${scratch}/assets/default.xbe)
    if(CMAKE_HOST_WIN32)
        set(builder apk-builder-windows.exe)
    else()
        set(builder apk-builder-linux)
    endif()
    file(COPY ${DEST}/${builder} DESTINATION ${scratch})
    set(args --cli --source ${scratch}/assets --jobs ${JOBS} --key ${scratch}/key.toml --out ${scratch})
    if(NOT TEST_DOWNLOAD)
        list(APPEND args --toolchain ${ZIG_HOST_DIR})
    endif()
    execute_process(COMMAND ${scratch}/${builder} ${args} RESULT_VARIABLE code)
    if(code OR NOT EXISTS ${scratch}/LiveAndRecompiled.apk)
        message(FATAL_ERROR "the test build made no LiveAndRecompiled.apk (exit ${code})")
    endif()
    if(EXISTS ${scratch}/work/generated)
        message(FATAL_ERROR "the builder left its generated code behind in ${scratch}/work")
    endif()
    # the SDK's own check of the signature, when it is there
    set(sdk "$ENV{ANDROID_HOME}")
    if(NOT sdk)
        set(sdk "$ENV{ANDROID_SDK_ROOT}")
    endif()
    file(GLOB apksigners "${sdk}/build-tools/*/apksigner" "${sdk}/build-tools/*/apksigner.bat")
    if(apksigners)
        list(SORT apksigners COMPARE NATURAL ORDER DESCENDING)
        list(GET apksigners 0 apksigner)
        execute_process(COMMAND ${apksigner} verify ${scratch}/LiveAndRecompiled.apk RESULT_VARIABLE code)
        if(code)
            message(FATAL_ERROR "apksigner does not take the test build's signature (${scratch}/LiveAndRecompiled.apk)")
        endif()
        message(STATUS "      apksigner verifies the APK's signature")
    endif()
    message(STATUS "      the kit builds LiveAndRecompiled.apk")
    file(REMOVE_RECURSE ${scratch})
else()
    message(STATUS "      skipped")
endif()

message(STATUS "[6/6] archives")
# one archive per platform, each a LiveAndRecompiled/ folder with that platform's builder
function(archive os builder ext flags format)
    set(file ${OUT}/${NAME}-${version}-android-builder-${os}.${ext})
    file(REMOVE ${file})
    file(REMOVE_RECURSE ${OUT}/.archive)
    file(MAKE_DIRECTORY ${OUT}/.archive/${NAME})
    file(COPY ${DEST}/${builder} ${DEST}/LICENSE ${DEST}/THIRD_PARTY.txt DESTINATION ${OUT}/.archive/${NAME})
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar ${flags} ${file} --format=${format} ${NAME}
        WORKING_DIRECTORY ${OUT}/.archive COMMAND_ERROR_IS_FATAL ANY)
    file(REMOVE_RECURSE ${OUT}/.archive)
    file(SIZE ${file} size)
    math(EXPR mb "(${size} + 524288) / 1048576")
    message(STATUS "      ${file} (${mb} MB)")
endfunction()
archive(windows apk-builder-windows.exe zip cf zip)
if(EXISTS ${DEST}/apk-builder-linux)
    archive(linux apk-builder-linux tar.gz czf gnutar)
endif()
