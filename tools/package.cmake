# package.cmake - build the player release, on Linux or Windows: setup-windows.exe and setup-linux, each with the kit
# (everything but the game's code) inside it.
#
# Players build the game on their own machine from their own copy: Setup takes the game from where it is pointed, downloads the
# pinned Zig for its host, recompiles the game, compiles the result and links it: Game.exe on Windows, the native Game
# on Linux (against glibc). Nothing in the release is the title's code, or the compiler.
#
#   cmake -P tools/package.cmake                    build the kit, test it against the game in game/assets, zip it
#   cmake -DTEST=OFF -P tools/package.cmake         skip the test
#   cmake -DTEST_DOWNLOAD=ON -P tools/package.cmake test with the compiler Setup downloads, not the one in toolchain/
#   cmake -DJOBS=N -P tools/package.cmake           parallel jobs (default: every core)
# tools/package.sh and tools/package.bat run it with the same options as flags.
#
# Output: game/out/release/Conker/ and game/out/release/Conker-<version>.zip
# Everything is built with the pinned Zig: Game.exe and setup-windows.exe for Windows, on either host, and on Linux
# setup-linux and the native game's prebuilt host against glibc 2.31 (the X11, Wayland, Pango, Cairo and audio headers
# SDL and FLTK need are Linux's): a release packaged on Windows has setup-windows.exe alone. The test runs this host's
# Setup: setup-linux on Linux for the native Game, setup-windows.exe on Windows for Game.exe.
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
    xbe_configure(release)  # the Linux codegen tool; on Windows the zig-windows build runs its own
    build(release xbe_recomp)
endif()
xbe_configure(zig-windows)
build(zig-windows Game_kit)
if(NOT CMAKE_HOST_WIN32)
    xbe_configure(zig-glibc)
    build(zig-glibc Game_kit setup)
endif()

set(DEST ${OUT}/${NAME})
message(STATUS "[3/5] staging ${DEST}")
file(REMOVE_RECURSE ${DEST})
file(COPY ${XBE_BUILDS}/zig-windows/package/Game/ DESTINATION ${DEST})
if(NOT CMAKE_HOST_WIN32)
    # the Linux half beside it: kit.toml and the rest are the same in both
    file(COPY ${XBE_BUILDS}/zig-glibc/package/Game/kit/linux.toml DESTINATION ${DEST}/kit)
    file(COPY ${XBE_BUILDS}/zig-glibc/package/Game/kit/lib/linux DESTINATION ${DEST}/kit/lib)
    file(COPY ${XBE_BUILDS}/zig-glibc/package/Game/kit/bin/xbe_recomp DESTINATION ${DEST}/kit/bin)
endif()
if(NOT CMAKE_HOST_WIN32)
    # dynamic against glibc 2.31 like the game: its window needs X11, which a static musl program cannot load
    file(COPY ${XBE_BUILDS}/zig-glibc/setup-linux DESTINATION ${DEST})
endif()
# no compiler in the release: Setup downloads the Zig of the same release for its host on the player's machine, for
# Game.exe on Windows and for the native Game on Linux
function(kit_toolchain os tool version)
    set(toml "\n# the compiler (${tool} ${version}) for the host Setup runs on, downloaded into ")
    string(APPEND toml "work/toolchain/${os}-<host> once\n")
    foreach(host ${ARGN})
        string(APPEND toml "[toolchain.${host}]\nurl = \"${${tool}_URL}/${${tool}_${host}_ARCHIVE}\"\n")
        string(APPEND toml "sha256 = \"${${tool}_${host}_SHA256}\"\n")
    endforeach()
    file(APPEND ${DEST}/kit/${os}.toml "${toml}")
endfunction()
kit_toolchain(windows ZIG ${ZIG_VERSION} windows)
message(STATUS "      setup-windows.exe downloads Zig ${ZIG_VERSION} for Game.exe")
if(EXISTS ${DEST}/kit/linux.toml)
    kit_toolchain(linux ZIG ${ZIG_VERSION} linux)
    message(STATUS "      setup-linux downloads Zig ${ZIG_VERSION} for the native Game")
endif()

# the kit goes inside each Setup: a zip of this host's files, appended to the executable and then a footer of
# "XBEKIT01" and the zip's size in 16 decimal digits (Setup unpacks it into its working folder on the first run)
function(embed_kit os setup recomp)
    set(stage ${OUT}/.kit-${os})
    file(REMOVE_RECURSE ${stage})
    file(MAKE_DIRECTORY ${stage}/bin ${stage}/lib)
    foreach(f kit.toml ${os}.toml)
        file(COPY ${DEST}/kit/${f} DESTINATION ${stage})
    endforeach()
    foreach(d config xdk include)
        file(COPY ${DEST}/kit/${d} DESTINATION ${stage})
    endforeach()
    file(COPY ${DEST}/kit/lib/${os} DESTINATION ${stage}/lib)
    file(COPY ${DEST}/kit/bin/${recomp} DESTINATION ${stage}/bin)
    set(zip ${OUT}/.kit-${os}.zip)
    file(REMOVE ${zip})
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf ${zip} --format=zip kit.toml ${os}.toml config xdk include lib bin
        WORKING_DIRECTORY ${stage} COMMAND_ERROR_IS_FATAL ANY)
    file(SIZE ${zip} size)
    string(LENGTH "${size}" digits)
    math(EXPR pad "16 - ${digits}")
    string(REPEAT "0" ${pad} zeros)
    file(WRITE ${OUT}/.kit-footer "XBEKIT01${zeros}${size}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E cat ${DEST}/${setup} ${zip} ${OUT}/.kit-footer
        OUTPUT_FILE ${DEST}/.${setup} COMMAND_ERROR_IS_FATAL ANY)
    file(RENAME ${DEST}/.${setup} ${DEST}/${setup})
    if(NOT os STREQUAL "windows")
        file(CHMOD ${DEST}/${setup} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif()
    file(REMOVE_RECURSE ${stage})
    file(REMOVE ${zip} ${OUT}/.kit-footer)
endfunction()
message(STATUS "      the kit inside each Setup")
embed_kit(windows setup-windows.exe xbe_recomp.exe)
if(EXISTS ${DEST}/kit/linux.toml)
    embed_kit(linux setup-linux xbe_recomp)
endif()
file(REMOVE_RECURSE ${DEST}/kit)

message(STATUS "[4/5] test")
if(TEST)
    # a player's steps on this host: the release's Setup and recompiler, the game's default.xbe, and this host's
    # compiler from toolchain/ (or, with TEST_DOWNLOAD, the one Setup downloads)
    string(RANDOM LENGTH 6 tag)
    set(scratch ${OUT}/.test-${tag})
    file(MAKE_DIRECTORY ${scratch}/assets)
    file(COPY_FILE ${XBE_GAME}/assets/default.xbe ${scratch}/assets/default.xbe)
    if(NOT CMAKE_HOST_WIN32)
        file(COPY ${DEST}/setup-linux DESTINATION ${scratch})
        set(setup ${scratch}/setup-linux)
        set(game Game)
        set(toolchain ${ZIG_HOST_DIR})
    else()
        file(COPY ${DEST}/setup-windows.exe DESTINATION ${scratch})
        set(setup ${scratch}/setup-windows.exe)
        set(game Game.exe)
        set(toolchain ${ZIG_HOST_DIR})
    endif()
    set(args --source ${scratch}/assets --jobs ${JOBS})
    if(NOT TEST_DOWNLOAD)
        list(APPEND args --toolchain ${toolchain})
    endif()
    execute_process(COMMAND ${setup} ${args} RESULT_VARIABLE code)
    if(code OR NOT EXISTS ${scratch}/game/${game})
        message(FATAL_ERROR "the test build made no game/${game} (exit ${code}); ${scratch}/setup.log has the run")
    endif()
    if(EXISTS ${scratch}/work/generated)
        message(FATAL_ERROR "Setup left its generated code behind in ${scratch}/work")
    endif()
    message(STATUS "      the kit builds ${game}")
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
