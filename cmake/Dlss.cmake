# The DLSS module (src/dlss): a library beside the game that links NVIDIA's NGX SDK, which the renderer loads when DLSS
# is asked for (src/runtime/gpu/dlss.h). The SDK's files come from its release on GitHub, pinned by hash: five headers,
# the static library NGX's API is in, the DLSS library NVIDIA has games ship beside them, and its licence.
# XBE_DLSS_RUNTIME is that library, for whatever puts the game together (the dev scripts, the player kit).
include_guard(GLOBAL)

option(XBE_DLSS "Build the DLSS module (downloads NVIDIA's DLSS SDK)" ON)
if(NOT XBE_DLSS)
    return()
endif()

set(XBE_DLSS_VERSION 310.9.16)
set(XBE_DLSS_DIR ${CMAKE_BINARY_DIR}/_deps/dlss_sdk)

# one file of the SDK at its release tag, downloaded once (again only when its hash no longer matches)
function(xbe_dlss_file path sha256)
    set(to ${XBE_DLSS_DIR}/${path})
    if(EXISTS ${to})
        file(SHA256 ${to} have)
        if(have STREQUAL sha256)
            return()
        endif()
    endif()
    message(STATUS "DLSS SDK: ${path}")
    file(DOWNLOAD https://raw.githubusercontent.com/NVIDIA/DLSS/v${XBE_DLSS_VERSION}/${path} ${to}
        EXPECTED_HASH SHA256=${sha256} STATUS status)
    list(GET status 0 code)
    if(code)
        list(GET status 1 why)
        message(FATAL_ERROR "DLSS SDK: ${path} could not be downloaded (${why}); configure with -DXBE_DLSS=OFF to "
            "build without DLSS")
    endif()
endfunction()

xbe_dlss_file(LICENSE.txt d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0)
xbe_dlss_file(include/nvsdk_ngx_defs.h ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e)
xbe_dlss_file(include/nvsdk_ngx_defs_vk.h 0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031)
xbe_dlss_file(include/nvsdk_ngx_params.h 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc)
xbe_dlss_file(include/nvsdk_ngx_vk.h 2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d)
xbe_dlss_file(include/nvsdk_ngx_helpers_vk.h c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623)

add_library(xbe_dlss SHARED ${XBE_ROOT_DIR}/src/dlss/ngx.cpp)
target_include_directories(xbe_dlss PRIVATE ${XBE_ROOT_DIR}/src/runtime ${vulkan_headers_SOURCE_DIR}/include)
target_include_directories(xbe_dlss SYSTEM PRIVATE ${XBE_DLSS_DIR}/include)
target_compile_definitions(xbe_dlss PRIVATE VK_NO_PROTOTYPES)
# no C++ library (the module's C++ runtime is NGX's, see ngx.cpp): no exceptions or RTTI, linked by the C driver
target_compile_options(xbe_dlss PRIVATE -fno-exceptions -fno-rtti -Wno-nullability-completeness)
set_target_properties(xbe_dlss PROPERTIES LINKER_LANGUAGE C CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_options(xbe_dlss PRIVATE -s)  # NGX's objects come with their debug info

if(WIN32)
    xbe_dlss_file(lib/Windows_x86_64/x64/nvsdk_ngx_d.lib bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c)
    xbe_dlss_file(lib/Windows_x86_64/rel/nvngx_dlss.dll d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e)
    set(XBE_DLSS_RUNTIME ${XBE_DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll)
    # NGX's library is Microsoft's compiler's: its C++ runtime's parts it needs over the UCRT (msvc_runtime.cpp), and
    # the libraries its objects name by default as empty archives, MinGW's linker looking for them by those names
    target_sources(xbe_dlss PRIVATE ${XBE_ROOT_DIR}/src/dlss/msvc_runtime.cpp)
    set(empty ${CMAKE_BINARY_DIR}/dlss_empty_libs)
    file(MAKE_DIRECTORY ${empty})
    foreach(l MSVCRT OLDNAMES msvcprt uuid)
        if(NOT EXISTS ${empty}/lib${l}.a)
            execute_process(COMMAND ${ZIG_HOST_DIR}/zig${HOST_EXE} ar rc ${empty}/lib${l}.a COMMAND_ERROR_IS_FATAL ANY)
        endif()
    endforeach()
    set_target_properties(xbe_dlss PROPERTIES PREFIX "")
    target_link_libraries(xbe_dlss PRIVATE ${XBE_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib advapi32)
    target_link_options(xbe_dlss PRIVATE -L${empty})
else()
    xbe_dlss_file(lib/Linux_x86_64/libnvsdk_ngx.a d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df)
    xbe_dlss_file(lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${XBE_DLSS_VERSION}
        30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6)
    set(XBE_DLSS_RUNTIME ${XBE_DLSS_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${XBE_DLSS_VERSION})
    # NGX's library is GCC's, against libstdc++: the system's, which every distro has (the module needs it no newer
    # than GCC 5's), linked by its file since Zig has none of its own. Only the module's entry point is exported
    find_file(XBE_LIBSTDCXX libstdc++.so.6 PATHS /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT XBE_LIBSTDCXX)
        message(FATAL_ERROR "DLSS: no libstdc++.so.6 on this system to link NVIDIA's library against; configure with "
            "-DXBE_DLSS=OFF to build without DLSS")
    endif()
    set(exports ${CMAKE_BINARY_DIR}/xbe_dlss.map)
    file(WRITE ${exports} "{ global: xbe_dlss_api; local: *; };\n")
    target_link_libraries(xbe_dlss PRIVATE ${XBE_DLSS_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a ${XBE_LIBSTDCXX}
        ${CMAKE_DL_LIBS})
    target_link_options(xbe_dlss PRIVATE -Wl,--version-script=${exports} -Wl,--allow-shlib-undefined)
endif()

# NVIDIA's DLSS library beside the module, as the game finds it
add_custom_command(TARGET xbe_dlss POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_DLSS_RUNTIME} $<TARGET_FILE_DIR:xbe_dlss>/
    VERBATIM)
