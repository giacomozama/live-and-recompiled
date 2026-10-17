# package.cmake - build the player release, on Linux or Windows: setup-windows.exe, setup-linux and a kit with
# everything but the game's code.
#
# Players build the game on their own machine from their own copy: Setup finds the game in assets/, downloads the
# pinned compiler for its host, recompiles the game, compiles the result and links it: Game.exe with llvm-mingw, or on
# Linux the native Game with Zig (against glibc). Nothing in the release is the title's code, or the compiler.
#
#   cmake -P tools/package.cmake                    build the kit, test it against the game in game/assets, zip it
#   cmake -DTEST=OFF -P tools/package.cmake         skip the test
#   cmake -DTEST_DOWNLOAD=ON -P tools/package.cmake test with the compiler Setup downloads, not the one in toolchain/
#   cmake -DJOBS=N -P tools/package.cmake           parallel jobs (default: every core)
# tools/package.sh and tools/package.bat run it with the same options as flags.
#
# Output: game/out/release/Conker/ and game/out/release/Conker-<version>.zip
# setup-linux and the Linux xbe_recomp are built with the pinned Zig, so those are the same from either host. The kit's
# Linux half (the native game's prebuilt host) is built on Linux only, against the X11, Wayland and audio headers SDL
# needs: a release packaged on Windows builds Game.exe alone. The test runs this host's Setup: setup-linux on Linux for
# both the native Game and Game.exe, setup-windows.exe on Windows.
cmake_minimum_required(VERSION 3.25)
include(${CMAKE_CURRENT_LIST_DIR}/configure.cmake)

set(OUT ${XBE_GAME}/out/release)
set(NAME Conker)
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

message(STATUS "[1/5] toolchains")
toolchain_fetch(LLVM_MINGW ${TOOLCHAIN_HOST})
toolchain_fetch(ZIG ${TOOLCHAIN_HOST})

message(STATUS "[2/5] kit")
if(NOT EXISTS ${XBE_GAME}/assets/default.xbe)
    message(FATAL_ERROR "no game in game/assets: put the supported release there first (see README.md)")
endif()
if(NOT CMAKE_HOST_WIN32)
    xbe_configure(release)  # the Linux codegen tool; on Windows the llvm-mingw build runs its own
    build(release xbe_recomp)
endif()
xbe_configure(zig-linux)
build(zig-linux xbe_recomp xbe_setup unpack_store)
xbe_configure(llvm-mingw)
build(llvm-mingw Game_kit unpack_store)
if(NOT CMAKE_HOST_WIN32)
    xbe_configure(zig-glibc)
    build(zig-glibc Game_kit)
endif()

set(DEST ${OUT}/${NAME})
message(STATUS "[3/5] staging ${DEST}")
file(REMOVE_RECURSE ${DEST})
file(COPY ${XBE_BUILDS}/llvm-mingw/package/Game/ DESTINATION ${DEST})
if(NOT CMAKE_HOST_WIN32)
    # the Linux half beside it: kit.toml and the rest are the same in both
    file(COPY ${XBE_BUILDS}/zig-glibc/package/Game/kit/linux.toml DESTINATION ${DEST}/kit)
    file(COPY ${XBE_BUILDS}/zig-glibc/package/Game/kit/lib/linux DESTINATION ${DEST}/kit/lib)
endif()
file(COPY ${XBE_ROOT}/README.md DESTINATION ${DEST})
file(COPY ${XBE_BUILDS}/zig-linux/xbe_setup DESTINATION ${DEST})
file(RENAME ${DEST}/xbe_setup ${DEST}/setup-linux)
file(COPY_FILE ${XBE_BUILDS}/zig-linux/unpack-store ${DEST}/unpack-store-linux)
file(COPY_FILE ${XBE_BUILDS}/llvm-mingw/unpack-store.exe ${DEST}/unpack-store-windows.exe)
file(COPY ${XBE_BUILDS}/zig-linux/xbe_recomp DESTINATION ${DEST}/kit/bin)
# no compiler in the release: Setup downloads the one of the same release for its host on the player's machine,
# llvm-mingw for Game.exe from either, Zig for the native Linux game from Linux
function(kit_toolchain os tool version)
    set(toml "\n# the compiler (${tool} ${version}) for the host Setup runs on, downloaded into ")
    string(APPEND toml "kit/toolchain/${os}-<host> once\n")
    foreach(host ${ARGN})
        string(APPEND toml "[toolchain.${host}]\nurl = \"${${tool}_URL}/${${tool}_${host}_ARCHIVE}\"\n")
        string(APPEND toml "sha256 = \"${${tool}_${host}_SHA256}\"\n")
    endforeach()
    file(APPEND ${DEST}/kit/${os}.toml "${toml}")
endfunction()
kit_toolchain(windows LLVM_MINGW ${LLVM_MINGW_VERSION} windows linux)
message(STATUS "      Setup downloads llvm-mingw ${LLVM_MINGW_VERSION} for Game.exe")
if(EXISTS ${DEST}/kit/linux.toml)
    kit_toolchain(linux ZIG ${ZIG_VERSION} linux)
    message(STATUS "      setup-linux downloads Zig ${ZIG_VERSION} for the native Game")
endif()

message(STATUS "[4/5] test")
if(TEST)
    # a player's steps on this host: the release's Setup and recompiler, the game's default.xbe, and this host's
    # compiler from toolchain/ (or, with TEST_DOWNLOAD, the one Setup downloads)
    string(RANDOM LENGTH 6 tag)
    set(scratch ${OUT}/.test-${tag})
    file(MAKE_DIRECTORY ${scratch}/assets)
    file(COPY ${DEST}/kit DESTINATION ${scratch})
    file(COPY_FILE ${XBE_GAME}/assets/default.xbe ${scratch}/assets/default.xbe)
    if(NOT CMAKE_HOST_WIN32)
        file(COPY ${DEST}/setup-linux DESTINATION ${scratch})
        set(setup ${scratch}/setup-linux)
        set(targets linux windows)
    else()
        file(COPY ${DEST}/setup-windows.exe DESTINATION ${scratch})
        set(setup ${scratch}/setup-windows.exe)
        set(targets windows)
    endif()
    foreach(target ${targets})
        set(args --target ${target} --jobs ${JOBS})
        if(target STREQUAL "windows")
            set(game Game.exe)
            set(toolchain ${LLVM_MINGW_HOST_DIR})
        else()
            set(game Game)
            set(toolchain ${ZIG_HOST_DIR})
        endif()
        if(NOT TEST_DOWNLOAD)
            list(APPEND args --toolchain ${toolchain})
        endif()
        execute_process(COMMAND ${setup} ${args} RESULT_VARIABLE code)
        if(code OR NOT EXISTS ${scratch}/${game})
            message(FATAL_ERROR "the test build made no ${game} (exit ${code}); ${scratch}/setup.log has the run")
        endif()
        if(EXISTS ${scratch}/kit/work)
            message(FATAL_ERROR "Setup left kit/work behind in ${scratch}")
        endif()
        message(STATUS "      the kit builds ${game}")
    endforeach()
    file(REMOVE_RECURSE ${scratch})
else()
    message(STATUS "      skipped")
endif()

message(STATUS "[5/5] zip")
execute_process(COMMAND git -C ${XBE_ROOT} describe --always --dirty OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE code ERROR_QUIET)
if(code OR NOT version)
    set(version dev)
endif()
set(zip ${OUT}/${NAME}-${version}.zip)
file(REMOVE ${zip})
execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf ${zip} --format=zip ${NAME} WORKING_DIRECTORY ${OUT}
    COMMAND_ERROR_IS_FATAL ANY)
file(SIZE ${zip} size)
math(EXPR mb "(${size} + 524288) / 1048576")
message(STATUS "      ${zip} (${mb} MB)")
