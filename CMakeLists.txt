# Conker: Live & Reloaded (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# no C++ modules here, and clang-scan-deps cannot drive `zig c++` (it is a wrapper, not the compiler): the scan the
# Windows build would otherwise make of every C++ source, dependencies' included, fails
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# the recompiler, the runtime and add_xbe_image()
include(cmake/XbeRecomp.cmake)

# the game's own code: src/conker/common for every image, src/conker/game for this one (add_xbe_image)
set(XBE_GAME_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/conker)
file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS ${XBE_GAME_SRC}/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/conker/game/")
# the prompts' key sprites and the redrawn HUD art decode with SDL's PNG loader
set_source_files_properties(${XBE_GAME_SRC}/common/prompts.cpp ${XBE_GAME_SRC}/common/hud_art.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")

# the redrawn HUD art's pictures: rasterized from the SVGs under src/conker/common/hud_art/svg by tools/hud_art.py
# (resvg, numpy and Pillow) into the build folder, never committed; hud_art.cpp embeds them through the art.inc there.
# A player kit carries them in its prebuilt host library
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(RESVG resvg REQUIRED)
set(HUD_ART_DIR ${CMAKE_BINARY_DIR}/generated/hud_art)
file(GLOB_RECURSE HUD_ART_SVGS CONFIGURE_DEPENDS ${XBE_GAME_SRC}/common/hud_art/svg/*.svg)
add_custom_command(OUTPUT ${HUD_ART_DIR}/art.inc
    COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{PATH}" ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/hud_art.py ${HUD_ART_DIR}
    DEPENDS ${HUD_ART_SVGS} ${CMAKE_CURRENT_SOURCE_DIR}/tools/hud_art.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/sight_glow.py
    COMMENT "Rasterizing the redrawn HUD art"
    VERBATIM)
add_custom_target(hud_art_pictures DEPENDS ${HUD_ART_DIR}/art.inc)

# partitions as xbe_symbols wrote them into src/conker/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XBE_GAME_SRC}/game.toml)
if(EXISTS ${XBE_GAME_SRC}/game.toml)
    file(STRINGS ${XBE_GAME_SRC}/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: write it with xbe_symbols (assets/default.xbe src/runtime/xdk --out src/conker/game.toml), the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# the Microsoft Store package's default.xbe: the disc's code and data, only the header (signature, certificate) is
# different
set(XBE_ALSO_SHA1 68a37f0d03d73dabc86a848df5ec1820dd551254)

# target name = GAME_EXE in tools/build.bat, xbe = GAME_XBE
add_xbe_image(Game default.xbe game.toml ${GAME_PARTITIONS} game)

# the version the settings menu shows (generated/version.h, version_header in cmake/XbeRecomp.cmake)
foreach(host Game_host Game_host_debug)
    if(TARGET ${host})
        add_dependencies(${host} version_header hud_art_pictures)
        target_include_directories(${host} PRIVATE ${CMAKE_BINARY_DIR}/generated)
    endif()
endforeach()

# a player kit (the Zig builds, with the game in assets/): Setup and everything but the game code, for players to build
# from their own copy
if(XBE_ZIG_TARGET AND TARGET Game)
    add_xbe_player_kit(Game "Conker: Live & Reloaded")
endif()
