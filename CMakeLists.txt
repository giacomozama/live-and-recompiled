# Conker: Live & Reloaded (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the recompiler, the runtime and add_xbe_image()
include(cmake/XbeRecomp.cmake)

# the game's own code: src/conker/common for every image, src/conker/game for this one (add_xbe_image)
set(XBE_GAME_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/conker)
file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS ${XBE_GAME_SRC}/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/conker/game/")

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

# a player kit (the Zig builds, with the game in assets/): Setup and everything but the game code, for players to build
# from their own copy
if(XBE_ZIG_TARGET AND TARGET Game)
    add_xbe_player_kit(Game "Conker: Live & Reloaded")
endif()
