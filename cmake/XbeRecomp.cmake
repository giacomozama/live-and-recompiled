# Original Xbox static recompilation: xbe_recomp, the host runtime and add_xbe_image()
include_guard(GLOBAL)

get_filename_component(XBE_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
# the XDK tables: signatures for xbe_symbols, struct layouts for the recompiler and the runtime
set(XBE_XDK_DIR ${XBE_ROOT_DIR}/src/runtime/xdk)

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.93.0 GIT_SHALLOW TRUE)
# Roboto Regular for the overlay, from the static fonts' last release (ImGui ships only Medium)
FetchContent_Declare(roboto URL https://github.com/googlefonts/roboto-2/releases/download/v2.138/roboto-unhinted.zip
    URL_HASH SHA256=70f64c718510a601fbcf752aafe644314dacaeb85474dc689c89787c4a72a728 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# SDL3 for controllers, static so it links into the exe
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
# FLTK for Setup's window: static, Wayland and X11 both (it picks at run time; Pango and Cairo draw on Wayland), and the
# Win32 API on Windows. Its bundled image libraries and libdecor (cairo title bars), no GL, D-Bus, tools or tests, and of
# X11's extensions only the ones Xft needs, so the window asks a desktop for as few libraries as possible
foreach(option FLTK_BUILD_TEST FLTK_BUILD_EXAMPLES FLTK_BUILD_FLUID FLTK_BUILD_FLTK_OPTIONS FLTK_BUILD_FORMS FLTK_BUILD_GL
        FLTK_BUILD_HTML_DOCS FLTK_BUILD_PDF_DOCS FLTK_OPTION_SVG FLTK_OPTION_PRINT_SUPPORT FLTK_BUILD_SHARED_LIBS
        FLTK_USE_SYSTEM_LIBJPEG FLTK_USE_SYSTEM_LIBPNG FLTK_USE_SYSTEM_ZLIB FLTK_USE_XINERAMA FLTK_USE_XFIXES
        FLTK_USE_XCURSOR FLTK_USE_DBUS FLTK_USE_SYSTEM_LIBDECOR FLTK_USE_LIBDECOR_GTK)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
# (Windows has neither: Pango would be looked for in the mingw sysroot and fail the configure)
if(WIN32)
    set(FLTK_BACKEND_WAYLAND OFF CACHE BOOL "" FORCE)
    set(FLTK_USE_PANGO OFF CACHE BOOL "" FORCE)
else()
    set(FLTK_BACKEND_WAYLAND ON CACHE BOOL "" FORCE)
    set(FLTK_USE_PANGO ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(fltk GIT_REPOSITORY https://github.com/fltk/fltk.git GIT_TAG release-1.4.5 GIT_SHALLOW TRUE)
# miniz for Setup, which unpacks the compiler it downloads
FetchContent_Declare(miniz GIT_REPOSITORY https://github.com/richgel999/miniz.git GIT_TAG 3.1.2 GIT_SHALLOW TRUE)
# The Vulkan renderer: the API's headers, volk (loads the Vulkan loader at run time, nothing to link), VMA for memory,
# all three headers only and fetched without their builds (a SOURCE_SUBDIR that isn't there); glslang compiles the HLSL
# the shaders are written in to SPIR-V
FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git GIT_TAG v1.3.296
    GIT_SHALLOW TRUE SOURCE_SUBDIR none)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.3.296.0 GIT_SHALLOW TRUE
    SOURCE_SUBDIR none)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.1.0 GIT_SHALLOW TRUE SOURCE_SUBDIR none)
foreach(option ENABLE_OPT ENABLE_GLSLANG_BINARIES ENABLE_SPVREMAPPER ENABLE_PCH GLSLANG_TESTS GLSLANG_ENABLE_INSTALL
        BUILD_EXTERNAL)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 15.0.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus imgui roboto SDL3 miniz vulkan_headers volk vma glslang fltk)

# Codegen runs on the host. By default it is this build's own xbe_recomp, which works when the target runs here
# (a native build, or Windows on Windows); cross compiling for another OS, set XBE_CODEGEN_TOOL to a native one's path.
set(XBE_CODEGEN_TOOL "xbe_recomp" CACHE FILEPATH "xbe_recomp used for codegen (a host path when cross compiling)")
# Windows builds: CodeView debug info and <target>.pdb beside the exe, for Visual Studio or WinDbg. It is set on the
# targets, not the compile flags, so a player kit's compile line (from CMAKE_CXX_FLAGS) stays without it.
option(XBE_PDB "Windows builds: write <target>.pdb (slower builds, bigger objects)" OFF)

# x86 to C++ recompiler
file(GLOB XBE_RECOMP_SOURCES CONFIGURE_DEPENDS ${XBE_ROOT_DIR}/src/recomp/*.cpp)
add_executable(xbe_recomp ${XBE_RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${XBE_ROOT_DIR}/src/runtime ${XBE_ROOT_DIR}/src/recomp)
target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

# XDK signature scanner that writes a new title's config
add_executable(xbe_symbols ${XBE_ROOT_DIR}/src/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${XBE_ROOT_DIR}/src/runtime)
target_compile_definitions(xbe_symbols PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)

# Setup, the player-side build: finds the game, recompiles it and links it with a prebuilt host (add_xbe_player_kit),
# from a window or the command line
add_executable(setup ${XBE_ROOT_DIR}/src/setup/main.cpp ${XBE_ROOT_DIR}/src/setup/gui.cpp
    ${XBE_ROOT_DIR}/src/setup/store.cpp
    ${XBE_ROOT_DIR}/src/runtime/kernel/disc.cpp
    ${XBE_ROOT_DIR}/src/runtime/kernel/crypto.cpp)
# built under its final name: setup-windows.exe, setup-linux
if(WIN32)
    set_target_properties(setup PROPERTIES OUTPUT_NAME setup-windows)
else()
    set_target_properties(setup PROPERTIES OUTPUT_NAME setup-linux)
endif()
target_include_directories(setup PRIVATE ${XBE_ROOT_DIR}/src/runtime)
target_compile_definitions(setup PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(setup PRIVATE tomlplusplus::tomlplusplus miniz fltk $<$<PLATFORM_ID:Windows>:winhttp>)
# Zig builds use its own libc headers, so the X11 and Wayland ones FLTK includes are found after them, as for SDL (cmake/zig-glibc.cmake)
if(XBE_ZIG_TARGET AND NOT WIN32)
    target_compile_options(fltk PRIVATE -idirafter /usr/include)
    target_compile_options(setup PRIVATE -idirafter /usr/include)
    # the system's X11, Wayland and Cairo libraries were built against a newer glibc than the one Zig links against, the player's has theirs
    target_link_options(setup PRIVATE -Wl,--allow-shlib-undefined)
endif()

set(XBE_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
# the overlay's font, Roboto Regular, as bytes in the executable (src/runtime/app/ui_font.cpp)
set(XBE_UI_FONT ${CMAKE_BINARY_DIR}/xbe_ui_font.cpp)
file(READ ${roboto_SOURCE_DIR}/Roboto-Regular.ttf XBE_UI_FONT_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," XBE_UI_FONT_HEX "${XBE_UI_FONT_HEX}")
file(CONFIGURE OUTPUT ${XBE_UI_FONT} @ONLY CONTENT "// generated from Roboto-Regular.ttf
#include <cstddef>
namespace app {
extern const unsigned char kUiFont[] = {${XBE_UI_FONT_HEX}};
extern const size_t kUiFontSize = sizeof(kUiFont);
}
")
list(APPEND XBE_IMGUI_SOURCES ${XBE_UI_FONT})

# The host's platform: on Windows a Win32 window and XAudio2; elsewhere an SDL3 window and audio (XBE_SDL). Vulkan draws
# on both. Each side's own sources are left out of the other's build.
file(GLOB XBE_RUNTIME_SOURCES CONFIGURE_DEPENDS ${XBE_ROOT_DIR}/src/runtime/*/*.cpp)
if(WIN32)
    list(APPEND XBE_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(app/main_sdl|app/crash_posix|app/system_info_posix|audio/sdl_audio)\\.cpp$")
    set(XBE_SDL_SOURCES ${XBE_ROOT_DIR}/src/runtime/core/input.cpp)
else()
    list(APPEND XBE_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX
        "/(app/main|app/crash|app/splash|app/handover|app/system_info|audio/xaudio2)\\.cpp$")
    set(XBE_SDL_SOURCES ${XBE_ROOT_DIR}/src/runtime/core/input.cpp ${XBE_ROOT_DIR}/src/runtime/app/main_sdl.cpp
        ${XBE_ROOT_DIR}/src/runtime/audio/sdl_audio.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
endif()
# SDL's headers go to the code that uses it alone (it is linked LINK_ONLY), so the generated sources' compile line stays
# the same
set_source_files_properties(${XBE_SDL_SOURCES} PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")
# Vulkan's and glslang's the same way, to the renderer and ImGui's backend for it; ENABLE_HLSL is glslang's own define
set(XBE_VULKAN_INCLUDES ${vulkan_headers_SOURCE_DIR}/include ${volk_SOURCE_DIR})
set_source_files_properties(${XBE_ROOT_DIR}/src/runtime/gpu/vulkan.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${XBE_VULKAN_INCLUDES};${vma_SOURCE_DIR}/include;${glslang_SOURCE_DIR};${glslang_BINARY_DIR}/include"
    COMPILE_DEFINITIONS "ENABLE_HLSL;IMGUI_IMPL_VULKAN_USE_VOLK" COMPILE_OPTIONS -Wno-nullability-completeness)
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${XBE_VULKAN_INCLUDES}" COMPILE_DEFINITIONS IMGUI_IMPL_VULKAN_USE_VOLK)
# glslang's libraries in the order they link
set(XBE_GLSLANG_LIBS glslang glslang-default-resource-limits)

# a whole archive as --whole-archive ... --no-whole-archive, the way a player kit's link line has it. CMake's own
# WHOLE_ARCHIVE wraps it in --push-state and --pop-state when lld takes them, which zig c++ rejects
set(CMAKE_LINK_LIBRARY_USING_XBE_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
set(CMAKE_LINK_LIBRARY_USING_XBE_WHOLE_ARCHIVE_SUPPORTED TRUE)
set(CMAKE_LINK_LIBRARY_XBE_WHOLE_ARCHIVE_ATTRIBUTES LIBRARY_TYPE=STATIC DEDUPLICATION=YES OVERRIDE=DEFAULT)

# how the generated code is compiled, here and by a player kit's Setup
set(XBE_GEN_COMPILE_OPTIONS -O2 -fno-strict-aliasing -Wno-unused-label -Wno-parentheses-equality -Wno-unused-variable)
set(XBE_HOST_DEFINITIONS UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
# what the host links besides SDL; xaudio2 is the Windows SDK's name, mingw's is xaudio2_8
if(NOT XBE_XAUDIO2)
    set(XBE_XAUDIO2 xaudio2)
endif()
if(WIN32)
    set(XBE_HOST_SYSTEM_LIBS dxguid ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
else()
    set(XBE_HOST_SYSTEM_LIBS ${CMAKE_DL_LIBS} pthread)
endif()

# One executable per recompiled image: its code from <XBE_GAME_SRC>/<config> (partitions as that config sets them),
# the runtime, XBE_HOST_SOURCES shared by every image of the game, and <XBE_GAME_SRC>/<game_dir> hooks only this
# image has. XBE_GAME_SRC (set by the project) is the game's source folder, also its include root.
# Expects assets/<xbe> and the config's out = "generated/<game_dir>". At run time the game may be a disc image instead.
function(add_xbe_image target xbe config partitions game_dir)
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/generated/${game_dir})
    set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
    math(EXPR last "${partitions} - 1")
    foreach(i RANGE ${last})
        list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
    endforeach()
    file(GLOB configs CONFIGURE_DEPENDS ${XBE_GAME_SRC}/*.toml ${XBE_XDK_DIR}/*/*.toml)
    # the xbe's SHA-1, checked on every build and rewritten only when it changed: what follows depends on the game by
    # its content (xbe_sha1.cmake)
    set(xbe_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe})
    # without the game only the tools are configured (tools/build.sh --store unpacks the package with setup-linux), and
    # assets/ changing configures again: the game's targets appear once the game is there
    if(NOT EXISTS ${xbe_file})
        message(WARNING "${xbe_file} is missing: only the tools are configured (xbe_recomp, xbe_symbols, setup). Put "
            "the game in assets/ (tools/build.sh --iso, --store or --game).")
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets)
        return()
    endif()
    set(XBE_SHA1_FILE ${CMAKE_CURRENT_BINARY_DIR}/${target}_xbe.sha1)
    # the build's own recompiler, by its target: CMake only puts a target's path in a command itself when not cross
    # compiling, and a toolchain file setting CMAKE_SYSTEM_NAME counts as cross compiling even on that system
    set(codegen ${XBE_CODEGEN_TOOL})
    if(TARGET ${XBE_CODEGEN_TOOL})
        set(codegen $<TARGET_FILE:${XBE_CODEGEN_TOOL}>)
    endif()
    add_custom_target(${target}_xbe_sha1
        COMMAND ${CMAKE_COMMAND} -DMODE=stamp -DXBE=${xbe_file} -DOUT=${XBE_SHA1_FILE}
            -P ${XBE_ROOT_DIR}/cmake/xbe_sha1.cmake
        BYPRODUCTS ${XBE_SHA1_FILE}
        COMMENT "Checking ${xbe}"
        VERBATIM)
    add_custom_command(
        OUTPUT ${GEN}/codegen.stamp
        BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h
        COMMAND ${codegen} ${XBE_GAME_SRC}/${config} --xdk ${XBE_XDK_DIR}
        COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
        DEPENDS ${XBE_CODEGEN_TOOL} ${configs} ${XBE_SHA1_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Recompiling ${xbe}"
        VERBATIM)
    add_custom_target(recompile_${target} DEPENDS ${GEN}/codegen.stamp)
    add_dependencies(recompile_${target} ${target}_xbe_sha1)
    set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE COMPILE_OPTIONS "${XBE_GEN_COMPILE_OPTIONS}")
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${XBE_GAME_SRC}/${game_dir}/*.cpp)
    # the xbe's SHA-1 in the executable: it checks the game it finds is the release it was made from
    set(XBE_HASH ${CMAKE_CURRENT_BINARY_DIR}/${target}_xbe.cpp)
    # XBE_ALSO_SHA1 (set by the project): releases whose code and data are the same and only the header differs, as
    # the list separator is the shell's they travel comma-separated
    string(REPLACE ";" "," XBE_ALSO_SHA1_ARG "${XBE_ALSO_SHA1}")
    add_custom_command(
        OUTPUT ${XBE_HASH}
        COMMAND ${CMAKE_COMMAND} -DMODE=cpp -DSHA=${XBE_SHA1_FILE} -DALSO=${XBE_ALSO_SHA1_ARG} -DOUT=${XBE_HASH}
            -P ${XBE_ROOT_DIR}/cmake/xbe_sha1.cmake
        DEPENDS ${XBE_SHA1_FILE} ${XBE_ROOT_DIR}/cmake/xbe_sha1.cmake
        VERBATIM)
    # Everything but the title's own code: the runtime, the game's hooks, ImGui and the xbe's hash. It reads only the
    # addresses in image_symbols.h, so it can be built ahead and linked with game code recompiled on another machine
    # (a player-side build, see add_xbe_player_kit).
    add_library(${target}_host STATIC ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${XBE_IMGUI_SOURCES} ${XBE_HASH})
    add_dependencies(${target}_host recompile_${target})
    target_include_directories(${target}_host PUBLIC ${XBE_GAME_SRC} ${XBE_ROOT_DIR}/src/runtime ${GEN})
    target_include_directories(${target}_host SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(${target}_host PUBLIC -fno-strict-aliasing)
    target_compile_definitions(${target}_host PUBLIC ${XBE_HOST_DEFINITIONS})
    target_link_libraries(${target}_host PUBLIC tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> ${XBE_HOST_SYSTEM_LIBS}
        $<$<PLATFORM_ID:Windows>:${XBE_XAUDIO2}>)
    # the game's hooks see an SDL window (src/conker/game), the runtime's input code hands it the controllers
    if(NOT WIN32)
        target_compile_definitions(${target}_host PRIVATE XBE_SDL)
    endif()
    foreach(lib ${XBE_GLSLANG_LIBS})
        target_link_libraries(${target}_host PUBLIC $<LINK_ONLY:${lib}>)
    endforeach()

    add_executable(${target} WIN32 ${GEN_SOURCES})
    add_dependencies(${target} recompile_${target})
    target_include_directories(${target} PRIVATE ${XBE_ROOT_DIR}/src/runtime ${GEN})
    # whole: an archive member is otherwise only taken for a symbol nothing defines yet, and every function the host
    # replaces already has the recompiled weak definition, so the replacements would be left out
    target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:XBE_WHOLE_ARCHIVE,${target}_host>")
    # lld keeps the recompiled weak functions overridable by the host ones
    target_link_options(${target} PRIVATE -fuse-ld=lld -g)
    if(WIN32)
        target_link_options(${target} PRIVATE -municode)  # the host's entry is wWinMain
        if(XBE_PDB)
            target_compile_options(${target}_host PRIVATE -g -gcodeview)
            target_compile_options(${target} PRIVATE -g -gcodeview)
            target_link_options(${target} PRIVATE -Wl,--pdb=)
        endif()
    endif()
    set_target_properties(${target} PROPERTIES XBE_FILE ${xbe} XBE_SHA1_FILE ${XBE_SHA1_FILE} XBE_CONFIG ${config}
        XBE_GEN ${GEN})
endfunction()

# A player kit for an image (the Zig builds, cmake/zig-windows.cmake and cmake/zig-glibc.cmake): everything a player needs
# to build the game from their own copy, and none of the title's code. Staged in <build>/package/<target>, one half of
# it per build (tools/package.cmake puts the two together):
#   setup-windows.exe  Setup: finds the game, recompiles, compiles and links <target>.exe (or <target>) beside it
#   kit/kit.toml       what Setup reads first: the release's sha1, the config, the recompiler, the headers
#   kit/<os>.toml      how to build for that OS (windows: <target>.exe, linux: the native <target>): the compiler, its
#                      flags and the libraries
#   kit/bin/           xbe_recomp (the packaging script adds the Linux one, and setup-linux beside setup-windows.exe)
#   kit/config, xdk/   the image's config and the XDK tables
#   kit/include/       the headers the generated code includes, and image_symbols.h to check the recompile against
#   kit/lib/<os>/      the prebuilt host (runtime, game hooks, ImGui), glslang and SDL
# <os>.toml's [toolchain.windows] and [toolchain.linux] (where Setup downloads the compiler of the version the kit was
# built with, Zig, for the host it runs on, and its SHA-256) are added by the packaging script; Setup
# unpacks it into kit/toolchain/<os>-<host> on the first run, and cxx is relative to that.
# title is the name Setup shows.
function(add_xbe_player_kit target title)
    if(NOT XBE_ZIG_TARGET)
        message(FATAL_ERROR "add_xbe_player_kit needs a Zig build (cmake/zig-windows.cmake or cmake/zig-glibc.cmake): "
            "players build with the same toolchain")
    elseif(WIN32)
        set(os windows)
    else()
        set(os linux)
    endif()
    get_target_property(xbe ${target} XBE_FILE)
    get_target_property(sha1_file ${target} XBE_SHA1_FILE)
    get_target_property(config ${target} XBE_CONFIG)
    get_target_property(gen ${target} XBE_GEN)
    set(pkg ${CMAKE_BINARY_DIR}/package/${target})
    set(kit ${pkg}/kit)
    string(REPLACE ";" "," XBE_ALSO_SHA1_ARG "${XBE_ALSO_SHA1}")
    set(lib lib/${os})

    # the compile line of the generated sources, as add_xbe_image's target has it in a Release build
    separate_arguments(cxx_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} -DNDEBUG")
    set(cxxflags ${cxx_flags} ${CMAKE_CXX20_EXTENSION_COMPILE_OPTION})
    foreach(d ${XBE_HOST_DEFINITIONS})
        list(APPEND cxxflags -D${d})
    endforeach()
    list(APPEND cxxflags ${XBE_GEN_COMPILE_OPTIONS})
    # the link line: the host whole (its replacements beat the recompiled weak functions), glslang, SDL and its system
    # libraries (SDL3staticTargets.cmake), the host's, CMake's defaults for a C++ program
    separate_arguments(std_libs NATIVE_COMMAND "${CMAKE_CXX_STANDARD_LIBRARIES}")
    set(libs -Wl,--whole-archive ${lib}/lib${target}_host.a -Wl,--no-whole-archive)
    foreach(l ${XBE_GLSLANG_LIBS})
        list(APPEND libs ${lib}/$<TARGET_FILE_NAME:${l}>)
    endforeach()
    list(APPEND libs ${lib}/libSDL3.a)
    if(os STREQUAL "windows")
        set(sdl_libs m kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8)
        # Zig makes the import library of xaudio2_8 itself, and the flags carry its -target as on Linux
        list(APPEND sdl_libs ${XBE_XAUDIO2})
        separate_arguments(ldflags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
        list(APPEND ldflags -mwindows -municode -fuse-ld=lld)
        set(cxx zig)
        set(driver c++)
    else()
        set(sdl_libs m)
        # the compile flags too, as CMake links: without zig's -target the game would ask for this host's glibc
        separate_arguments(ldflags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
        list(APPEND ldflags -fuse-ld=lld -pthread)
        set(cxx zig)
        set(driver c++)  # zig's own first argument, before every compile and link
    endif()
    foreach(l ${sdl_libs} ${XBE_HOST_SYSTEM_LIBS})
        list(APPEND libs -l${l})
    endforeach()
    list(APPEND libs ${std_libs})
    set(toml "# written by add_xbe_player_kit; Setup reads it\n")
    # the sha1 is filled in at build time, from the game in assets/ then (xbe_sha1.cmake)
    string(APPEND toml "title = \"${title}\"\nexe = \"${target}\"\nxbe = \"${xbe}\"\nsha1 = \"@XBE_SHA1@\"\nalso_sha1 = [@XBE_ALSO_SHA1@]\n")
    get_filename_component(config_name ${config} NAME)
    string(APPEND toml "config = \"config/${config_name}\"\nxdk = \"xdk\"\nrecomp = \"bin/xbe_recomp\"\n")
    string(APPEND toml "include = [\"include\"]\n")
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/${target}_kit.toml.in CONTENT "${toml}")
    set(toml "# written by add_xbe_player_kit: building ${target} for ${os}; Setup reads it\ncxx = \"${cxx}\"\n")
    foreach(key driver cxxflags ldflags libs)
        set(items)
        foreach(v ${${key}})
            list(APPEND items "\"${v}\"")
        endforeach()
        list(JOIN items ", " items)
        string(APPEND toml "${key} = [${items}]\n")
    endforeach()
    file(GENERATE OUTPUT ${kit}/${os}.toml CONTENT "${toml}")

    set(extra)
    if(os STREQUAL "windows")
        set(extra COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:setup> ${pkg}/)
    endif()
    set(glslang_libs)
    foreach(l ${XBE_GLSLANG_LIBS})
        list(APPEND glslang_libs $<TARGET_FILE:${l}>)
    endforeach()
    # <os>.toml is written at configure time, so the rest of the folder is cleared around it
    add_custom_target(${target}_kit
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${pkg}/assets ${pkg}/THIRD_PARTY.txt ${kit}/bin ${kit}/config ${kit}/xdk
            ${kit}/include ${kit}/lib ${kit}/kit.toml
        COMMAND ${CMAKE_COMMAND} -E make_directory ${kit}/bin ${kit}/config ${kit}/include/cpu
            ${kit}/${lib}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xbe_recomp> ${kit}/bin/
        COMMAND ${CMAKE_COMMAND} -DMODE=kit -DSHA=${sha1_file} -DALSO=${XBE_ALSO_SHA1_ARG} -DIN=${CMAKE_BINARY_DIR}/${target}_kit.toml.in
            -DOUT=${kit}/kit.toml -P ${XBE_ROOT_DIR}/cmake/xbe_sha1.cmake
        COMMAND ${CMAKE_COMMAND} -E copy ${XBE_GAME_SRC}/${config} ${kit}/config/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${XBE_XDK_DIR} ${kit}/xdk
        COMMAND ${CMAKE_COMMAND} -E copy ${XBE_ROOT_DIR}/src/runtime/cpu/x86_ops.h ${XBE_ROOT_DIR}/src/runtime/cpu/x86_context.h
            ${kit}/include/cpu/
        COMMAND ${CMAKE_COMMAND} -E copy ${gen}/image_symbols.h ${kit}/include/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}_host> $<TARGET_FILE:SDL3::SDL3-static> ${kit}/${lib}/
        COMMAND ${CMAKE_COMMAND} -E copy ${glslang_libs} ${kit}/${lib}/
        ${extra}
        COMMAND ${CMAKE_COMMAND} -E copy ${XBE_ROOT_DIR}/THIRD_PARTY.txt ${pkg}/
        DEPENDS ${target}_host setup xbe_recomp SDL3::SDL3-static ${XBE_GLSLANG_LIBS} ${sha1_file}
        COMMENT "Staging the ${os} player kit for ${target} in ${pkg}"
        VERBATIM)
endfunction()
