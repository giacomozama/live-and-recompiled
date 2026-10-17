# package.cmake - build the player release, on Linux or Windows: Setup.exe, setup-linux (Linux hosts) and a kit with
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
# A Windows host can't build the static Linux binaries, so a release packaged there has no setup-linux; Linux players
# run its Setup.exe with Proton instead.
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
set(LINUX_PARTS ON)
if(CMAKE_HOST_WIN32)
    set(LINUX_PARTS OFF)
endif()

function(build dir)
    execute_process(COMMAND ${CMAKE_COMMAND} --build ${XBE_BUILDS}/${dir} --target ${ARGN} -j ${JOBS}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

message(STATUS "[1/5] toolchain")
llvm_mingw_fetch(${LLVM_MINGW_HOST})

message(STATUS "[2/5] kit")
if(NOT EXISTS ${XBE_GAME}/assets/default.xbe)
    message(FATAL_ERROR "no game in game/assets: put the supported release there first (see README.md)")
endif()
if(LINUX_PARTS)
    xbe_configure(release)
    build(release xbe_recomp)
    xbe_configure(linux-kit)
    build(linux-kit xbe_recomp xbe_setup)
endif()
xbe_configure(llvm-mingw)
build(llvm-mingw Game_kit)

set(DEST ${OUT}/${NAME})
message(STATUS "[3/5] staging ${DEST}")
file(REMOVE_RECURSE ${DEST})
file(COPY ${XBE_BUILDS}/llvm-mingw/package/Game/ DESTINATION ${DEST})
file(COPY ${XBE_GAME}/README.md DESTINATION ${DEST})
if(LINUX_PARTS)
    file(COPY ${XBE_BUILDS}/linux-kit/xbe_setup DESTINATION ${DEST})
    file(RENAME ${DEST}/xbe_setup ${DEST}/setup-linux)
    file(COPY ${XBE_BUILDS}/linux-kit/xbe_recomp DESTINATION ${DEST}/kit/bin)
else()
    message(WARNING "packaged on Windows: the release has no setup-linux, Linux players run Setup.exe with Proton. "
        "Package on Linux for a release with both.")
endif()
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
    if(LINUX_PARTS)
        file(COPY ${DEST}/setup-linux DESTINATION ${scratch})
        set(setup ${scratch}/setup-linux)
    else()
        file(COPY ${DEST}/Setup.exe DESTINATION ${scratch})
        set(setup ${scratch}/Setup.exe)
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
