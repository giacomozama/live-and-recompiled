# package.cmake - build the player release, on Linux or Windows: setup-windows.exe, setup-linux and a kit with
# everything but the game's code.
#
# Players build the game on their own machine from their own copy: Setup finds the game in assets/, downloads the
# pinned llvm-mingw for its host, recompiles the game, compiles the result and links Game.exe. Nothing in the release
# is the title's code, or the compiler.
#
#   cmake -P tools/package.cmake                    build the kit, test it against the game in game/assets, zip it
#   cmake -DTEST=OFF -P tools/package.cmake         skip the test
#   cmake -DTEST_DOWNLOAD=ON -P tools/package.cmake test with the compiler Setup downloads, not the one in toolchain/
#   cmake -DJOBS=N -P tools/package.cmake           parallel jobs (default: every core)
# tools/package.sh and tools/package.bat run it with the same options as flags.
#
# Output: game/out/release/Conker/ and game/out/release/Conker-<version>.zip
# setup-linux and the Linux xbe_recomp are built with the pinned Zig, so a release is the same from either host. The
# test runs this host's Setup: setup-linux on Linux, setup-windows.exe on Windows.
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

set(DEST ${OUT}/${NAME})
message(STATUS "[3/5] staging ${DEST}")
file(REMOVE_RECURSE ${DEST})
file(COPY ${XBE_BUILDS}/llvm-mingw/package/Game/ DESTINATION ${DEST})
file(COPY ${XBE_GAME}/README.md DESTINATION ${DEST})
file(COPY ${XBE_BUILDS}/zig-linux/xbe_setup DESTINATION ${DEST})
file(RENAME ${DEST}/xbe_setup ${DEST}/setup-linux)
file(COPY_FILE ${XBE_BUILDS}/zig-linux/unpack-store ${DEST}/unpack-store-linux)
file(COPY_FILE ${XBE_BUILDS}/llvm-mingw/unpack-store.exe ${DEST}/unpack-store-windows.exe)
file(COPY ${XBE_BUILDS}/zig-linux/xbe_recomp DESTINATION ${DEST}/kit/bin)
# no compiler in the release: Setup downloads the llvm-mingw of the same release for its host on the player's machine
set(toml "\n# the compiler (llvm-mingw ${LLVM_MINGW_VERSION}) for the host Setup runs on, downloaded into ")
string(APPEND toml "kit/toolchain/<host> once\n")
foreach(host windows linux)
    string(APPEND toml "[toolchain.${host}]\nurl = \"${LLVM_MINGW_URL}/${LLVM_MINGW_${host}_ARCHIVE}\"\n")
    string(APPEND toml "sha256 = \"${LLVM_MINGW_${host}_SHA256}\"\n")
endforeach()
file(APPEND ${DEST}/kit/kit.toml "${toml}")
message(STATUS "      Setup downloads llvm-mingw ${LLVM_MINGW_VERSION}")

message(STATUS "[4/5] test")
if(TEST)
    # a player's steps on this host: the release's Setup and recompiler, the game's default.xbe, and this host's
    # llvm-mingw from toolchain/ (or, with TEST_DOWNLOAD, the one Setup downloads)
    string(RANDOM LENGTH 6 tag)
    set(scratch ${OUT}/.test-${tag})
    file(MAKE_DIRECTORY ${scratch}/assets)
    file(COPY ${DEST}/kit DESTINATION ${scratch})
    file(COPY_FILE ${XBE_GAME}/assets/default.xbe ${scratch}/assets/default.xbe)
    if(NOT CMAKE_HOST_WIN32)
        file(COPY ${DEST}/setup-linux DESTINATION ${scratch})
        set(setup ${scratch}/setup-linux)
    else()
        file(COPY ${DEST}/setup-windows.exe DESTINATION ${scratch})
        set(setup ${scratch}/setup-windows.exe)
    endif()
    set(args --jobs ${JOBS})
    if(NOT TEST_DOWNLOAD)
        list(APPEND args --toolchain ${LLVM_MINGW_HOST_DIR})
    endif()
    execute_process(COMMAND ${setup} ${args} RESULT_VARIABLE code)
    if(code OR NOT EXISTS ${scratch}/Game.exe)
        message(FATAL_ERROR "the test build made no Game.exe (exit ${code}); ${scratch}/setup.log has the run")
    endif()
    if(EXISTS ${scratch}/kit/work)
        message(FATAL_ERROR "Setup left kit/work behind in ${scratch}")
    endif()
    file(REMOVE_RECURSE ${scratch})
    message(STATUS "      the kit builds Game.exe")
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
