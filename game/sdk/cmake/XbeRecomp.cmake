# Original Xbox static recompilation: xbe_recomp, the host runtime and add_xbe_image()
include_guard(GLOBAL)

get_filename_component(XBE_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
# build scripts find the SDK's xdk folder and THIRD_PARTY.txt through this
file(TO_NATIVE_PATH "${XBE_SDK_DIR}" XBE_SDK_DIR_NATIVE)
file(WRITE ${CMAKE_BINARY_DIR}/sdk_dir.txt "${XBE_SDK_DIR_NATIVE}\n")

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
# SDL3 for controllers, static so it links into the exe
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
# miniz for Setup, which unpacks the compiler it downloads
FetchContent_Declare(miniz GIT_REPOSITORY https://github.com/richgel999/miniz.git GIT_TAG 3.1.2 GIT_SHALLOW TRUE)
# The Vulkan renderer: the API's headers, volk (loads the Vulkan loader at run time, nothing to link), VMA for memory,
# all three headers only and fetched without their builds (a SOURCE_SUBDIR that isn't there); glslang compiles the HLSL
# both renderers share to SPIR-V
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
FetchContent_MakeAvailable(zydis tomlplusplus imgui SDL3 miniz vulkan_headers volk vma glslang)

# Codegen runs on the host. By default it is this build's own xbe_recomp, which works when the target runs here
# (a native build, or Windows on Windows); cross compiling for another OS, set XBE_CODEGEN_TOOL to a native one's path.
set(XBE_CODEGEN_TOOL "xbe_recomp" CACHE FILEPATH "xbe_recomp used for codegen (a host path when cross compiling)")
# MinGW builds: CodeView debug info and <target>.pdb beside the exe, for Visual Studio or WinDbg. It is set on the
# targets, not the compile flags, so a player kit's compile line (from CMAKE_CXX_FLAGS) stays without it.
option(XBE_PDB "MinGW builds: write <target>.pdb (slower builds, bigger objects)" OFF)

# x86 to C++ recompiler
file(GLOB XBE_RECOMP_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${XBE_RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${XBE_SDK_DIR}/runtime ${XBE_SDK_DIR}/tools/recomp)
target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

# XDK signature scanner that writes a new title's config
add_executable(xbe_symbols ${XBE_SDK_DIR}/tools/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${XBE_SDK_DIR}/runtime)
target_compile_definitions(xbe_symbols PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)

# Setup, the player-side build: finds the game, recompiles it and links it with a prebuilt host (add_xbe_player_kit)
add_executable(xbe_setup ${XBE_SDK_DIR}/tools/setup/main.cpp ${XBE_SDK_DIR}/runtime/kernel/disc.cpp
    ${XBE_SDK_DIR}/runtime/kernel/crypto.cpp)
target_include_directories(xbe_setup PRIVATE ${XBE_SDK_DIR}/runtime)
target_compile_definitions(xbe_setup PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_setup PRIVATE tomlplusplus::tomlplusplus miniz $<$<PLATFORM_ID:Windows>:winhttp>)

set(XBE_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)

# The host's platform: on Windows a Win32 window, XAudio2 and D3D12 beside Vulkan; elsewhere an SDL3 window and audio
# (XBE_SDL) and Vulkan alone. Each side's own sources are left out of the other's build.
file(GLOB XBE_RUNTIME_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/*/*.cpp)
if(WIN32)
    list(APPEND XBE_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(app/main_sdl|app/crash_posix|app/system_info_posix|audio/sdl_audio)\\.cpp$")
    set(XBE_SDL_SOURCES ${XBE_SDK_DIR}/runtime/core/input.cpp)
else()
    list(APPEND XBE_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX
        "/(app/main|app/crash|app/splash|app/handover|app/system_info|audio/xaudio2|gpu/d3d12)\\.cpp$")
    set(XBE_SDL_SOURCES ${XBE_SDK_DIR}/runtime/core/input.cpp ${XBE_SDK_DIR}/runtime/app/main_sdl.cpp
        ${XBE_SDK_DIR}/runtime/audio/sdl_audio.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
endif()
# SDL's headers go to the code that uses it alone (it is linked LINK_ONLY), so the generated sources' compile line stays
# the same
set_source_files_properties(${XBE_SDL_SOURCES} PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")
# Vulkan's and glslang's the same way, to the renderer and ImGui's backend for it; ENABLE_HLSL is glslang's own define
set(XBE_VULKAN_INCLUDES ${vulkan_headers_SOURCE_DIR}/include ${volk_SOURCE_DIR})
set_source_files_properties(${XBE_SDK_DIR}/runtime/gpu/vulkan.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${XBE_VULKAN_INCLUDES};${vma_SOURCE_DIR}/include;${glslang_SOURCE_DIR};${glslang_BINARY_DIR}/include"
    COMPILE_DEFINITIONS "ENABLE_HLSL;IMGUI_IMPL_VULKAN_USE_VOLK" COMPILE_OPTIONS -Wno-nullability-completeness)
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${XBE_VULKAN_INCLUDES}" COMPILE_DEFINITIONS IMGUI_IMPL_VULKAN_USE_VOLK)
# glslang's libraries in the order they link
set(XBE_GLSLANG_LIBS glslang glslang-default-resource-limits)

# how the generated code is compiled, here and by a player kit's Setup
set(XBE_GEN_COMPILE_OPTIONS -O2 -fno-strict-aliasing -Wno-unused-label -Wno-parentheses-equality -Wno-unused-variable)
set(XBE_HOST_DEFINITIONS UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
# what the host links besides SDL; xaudio2 is the Windows SDK's name, mingw's is xaudio2_8
if(WIN32)
    set(XBE_HOST_SYSTEM_LIBS d3d12 dxgi dxguid d3dcompiler ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
else()
    set(XBE_HOST_SYSTEM_LIBS ${CMAKE_DL_LIBS} pthread)
endif()

# One executable per recompiled image: its code from config/<config> (partitions as that config sets them),
# the SDK runtime, XBE_HOST_SOURCES shared by every image of the game, and src/<game_dir> hooks only this image has.
# Expects assets/<xbe> and the config's out = "generated/<game_dir>". At run time the game may be a disc image instead.
function(add_xbe_image target xbe config partitions game_dir)
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/generated/${game_dir})
    set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
    math(EXPR last "${partitions} - 1")
    foreach(i RANGE ${last})
        list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
    endforeach()
    file(GLOB configs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/*.toml ${XBE_SDK_DIR}/xdk/*/*.toml)
    # the xbe's SHA-1, checked on every build and rewritten only when it changed: what follows depends on the game by
    # its content (xbe_sha1.cmake)
    set(xbe_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe})
    if(NOT EXISTS ${xbe_file})
        message(FATAL_ERROR "${xbe_file} is missing: put the game in assets/ (tools/build.sh --iso FILE)")
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
            -P ${XBE_SDK_DIR}/cmake/xbe_sha1.cmake
        BYPRODUCTS ${XBE_SHA1_FILE}
        COMMENT "Checking ${xbe}"
        VERBATIM)
    add_custom_command(
        OUTPUT ${GEN}/codegen.stamp
        BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h
        COMMAND ${codegen} config/${config} --xdk ${XBE_SDK_DIR}/xdk
        COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
        DEPENDS ${XBE_CODEGEN_TOOL} ${configs} ${XBE_SHA1_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Recompiling ${xbe}"
        VERBATIM)
    add_custom_target(recompile_${target} DEPENDS ${GEN}/codegen.stamp)
    add_dependencies(recompile_${target} ${target}_xbe_sha1)
    set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE COMPILE_OPTIONS "${XBE_GEN_COMPILE_OPTIONS}")
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${game_dir}/*.cpp)
    # the xbe's SHA-1 in the executable: it checks the game it finds is the release it was made from
    set(XBE_HASH ${CMAKE_CURRENT_BINARY_DIR}/${target}_xbe.cpp)
    add_custom_command(
        OUTPUT ${XBE_HASH}
        COMMAND ${CMAKE_COMMAND} -DMODE=cpp -DSHA=${XBE_SHA1_FILE} -DOUT=${XBE_HASH}
            -P ${XBE_SDK_DIR}/cmake/xbe_sha1.cmake
        DEPENDS ${XBE_SHA1_FILE} ${XBE_SDK_DIR}/cmake/xbe_sha1.cmake
        VERBATIM)
    # Everything but the title's own code: the runtime, the game's hooks, ImGui and the xbe's hash. It reads only the
    # addresses in image_symbols.h, so it can be built ahead and linked with game code recompiled on another machine
    # (a player-side build, see add_xbe_player_kit).
    add_library(${target}_host STATIC ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${XBE_IMGUI_SOURCES} ${XBE_HASH})
    add_dependencies(${target}_host recompile_${target})
    target_include_directories(${target}_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${XBE_SDK_DIR}/runtime ${GEN})
    target_include_directories(${target}_host SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(${target}_host PUBLIC -fno-strict-aliasing)
    target_compile_definitions(${target}_host PUBLIC ${XBE_HOST_DEFINITIONS})
    target_link_libraries(${target}_host PUBLIC tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> ${XBE_HOST_SYSTEM_LIBS}
        $<$<PLATFORM_ID:Windows>:xaudio2>)
    # the game's hooks see an SDL window (src/game), the runtime's input code hands it the controllers
    if(NOT WIN32)
        target_compile_definitions(${target}_host PRIVATE XBE_SDL)
    endif()
    foreach(lib ${XBE_GLSLANG_LIBS})
        target_link_libraries(${target}_host PUBLIC $<LINK_ONLY:${lib}>)
    endforeach()

    add_executable(${target} WIN32 ${GEN_SOURCES})
    add_dependencies(${target} recompile_${target})
    target_include_directories(${target} PRIVATE ${XBE_SDK_DIR}/runtime ${GEN})
    # whole: an archive member is otherwise only taken for a symbol nothing defines yet, and every function the host
    # replaces already has the recompiled weak definition, so the replacements would be left out
    target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${target}_host>")
    # lld keeps the recompiled weak functions overridable by the host ones
    target_link_options(${target} PRIVATE -fuse-ld=lld -g)
    if(MINGW)
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

# A player kit for an image (MinGW builds): everything a player needs to build the game from their own copy, and none
# of the title's code. Staged in <build>/package/<target>:
#   setup-windows.exe  xbe_setup: finds the game, recompiles, compiles and links <target>.exe beside itself
#   assets/            empty, the player's game goes here
#   kit/kit.toml       what Setup reads: the release's sha1, the commands' flags and libraries
#   kit/bin/           xbe_recomp (the packaging script adds the Linux one, and setup-linux beside setup-windows.exe)
#   kit/config, xdk/   the image's config and the SDK's XDK tables
#   kit/include/       the headers the generated code includes, and image_symbols.h to check the recompile against
#   kit/lib/           the prebuilt host (runtime, game hooks, ImGui), glslang, SDL and the xaudio2 import library
# kit.toml's [toolchain.windows] and [toolchain.linux] (where Setup downloads the llvm-mingw of the version the kit was
# built with for the host it runs on, and its SHA-256) are added by the packaging script; Setup unpacks it into
# kit/toolchain/<host> on the first run, and cxx is relative to that.
# title is the name Setup shows.
function(add_xbe_player_kit target title)
    if(NOT MINGW)
        message(FATAL_ERROR "add_xbe_player_kit needs a MinGW build (llvm-mingw): players build with the same toolchain")
    endif()
    get_target_property(xbe ${target} XBE_FILE)
    get_target_property(sha1_file ${target} XBE_SHA1_FILE)
    get_target_property(config ${target} XBE_CONFIG)
    get_target_property(gen ${target} XBE_GEN)
    set(pkg ${CMAKE_BINARY_DIR}/package/${target})
    set(kit ${pkg}/kit)

    # the compile line of the generated sources, as add_xbe_image's target has it in a Release build
    separate_arguments(cxx_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} -DNDEBUG")
    set(cxxflags ${cxx_flags} ${CMAKE_CXX20_EXTENSION_COMPILE_OPTION})
    foreach(d ${XBE_HOST_DEFINITIONS})
        list(APPEND cxxflags -D${d})
    endforeach()
    list(APPEND cxxflags ${XBE_GEN_COMPILE_OPTIONS})
    # the link line: the host whole (its replacements beat the recompiled weak functions), SDL and its system
    # libraries (SDL3staticTargets.cmake), the host's, CMake's defaults for a C++ program
    separate_arguments(std_libs NATIVE_COMMAND "${CMAKE_CXX_STANDARD_LIBRARIES}")
    set(libs -Wl,--whole-archive lib/lib${target}_host.a -Wl,--no-whole-archive)
    foreach(lib ${XBE_GLSLANG_LIBS})
        list(APPEND libs lib/$<TARGET_FILE_NAME:${lib}>)
    endforeach()
    list(APPEND libs lib/libSDL3.a lib/libxaudio2.a)
    foreach(l m kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8
              ${XBE_HOST_SYSTEM_LIBS})
        list(APPEND libs -l${l})
    endforeach()
    list(APPEND libs ${std_libs})
    set(ldflags -static -mwindows -municode -fuse-ld=lld)
    set(toml "# written by add_xbe_player_kit; Setup reads it\n")
    # the sha1 is filled in at build time, from the game in assets/ then (xbe_sha1.cmake)
    string(APPEND toml "title = \"${title}\"\nexe = \"${target}\"\nxbe = \"${xbe}\"\nsha1 = \"@XBE_SHA1@\"\n")
    get_filename_component(config_name ${config} NAME)
    string(APPEND toml "config = \"config/${config_name}\"\nxdk = \"xdk\"\nrecomp = \"bin/xbe_recomp\"\n")
    string(APPEND toml "cxx = \"bin/x86_64-w64-mingw32-clang++\"\ninclude = [\"include\"]\n")
    foreach(key cxxflags ldflags libs)
        set(items)
        foreach(v ${${key}})
            list(APPEND items "\"${v}\"")
        endforeach()
        list(JOIN items ", " items)
        string(APPEND toml "${key} = [${items}]\n")
    endforeach()
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/${target}_kit.toml.in CONTENT "${toml}")

    set(xaudio2 "")
    foreach(dir ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} ${CMAKE_FIND_ROOT_PATH}/lib)
        if(NOT xaudio2 AND EXISTS ${dir}/libxaudio2_8.a)
            set(xaudio2 ${dir}/libxaudio2_8.a)
        endif()
    endforeach()
    if(NOT xaudio2)
        message(FATAL_ERROR "libxaudio2_8.a not found in the toolchain")
    endif()
    set(glslang_libs)
    foreach(lib ${XBE_GLSLANG_LIBS})
        list(APPEND glslang_libs $<TARGET_FILE:${lib}>)
    endforeach()
    add_custom_target(${target}_kit
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${pkg}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${pkg}/assets ${kit}/bin ${kit}/config ${kit}/include/cpu ${kit}/lib
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xbe_setup> ${pkg}/setup-windows$<TARGET_FILE_SUFFIX:xbe_setup>
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:xbe_recomp> ${kit}/bin/
        COMMAND ${CMAKE_COMMAND} -DMODE=kit -DSHA=${sha1_file} -DIN=${CMAKE_BINARY_DIR}/${target}_kit.toml.in
            -DOUT=${kit}/kit.toml -P ${XBE_SDK_DIR}/cmake/xbe_sha1.cmake
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/${config} ${kit}/config/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${XBE_SDK_DIR}/xdk ${kit}/xdk
        COMMAND ${CMAKE_COMMAND} -E copy ${XBE_SDK_DIR}/runtime/cpu/x86_ops.h ${XBE_SDK_DIR}/runtime/cpu/x86_context.h
            ${kit}/include/cpu/
        COMMAND ${CMAKE_COMMAND} -E copy ${gen}/image_symbols.h ${kit}/include/
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}_host> $<TARGET_FILE:SDL3::SDL3-static> ${kit}/lib/
        COMMAND ${CMAKE_COMMAND} -E copy ${glslang_libs} ${kit}/lib/
        COMMAND ${CMAKE_COMMAND} -E copy ${xaudio2} ${kit}/lib/libxaudio2.a
        COMMAND ${CMAKE_COMMAND} -E copy ${XBE_SDK_DIR}/THIRD_PARTY.txt ${pkg}/
        DEPENDS ${target}_host xbe_setup xbe_recomp SDL3::SDL3-static ${XBE_GLSLANG_LIBS} ${sha1_file}
        COMMENT "Staging the player kit for ${target} in ${pkg}"
        VERBATIM)
endfunction()
