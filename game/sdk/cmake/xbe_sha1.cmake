# xbe_sha1.cmake: the xbe's SHA-1, worked out at build time, so a different game in assets/ is noticed by its content
# (switching a link to an older file leaves every modification time as it was). Run with cmake -P:
#   -DMODE=stamp -DXBE=<xbe> -DOUT=<file>                  the SHA-1 into <file>, rewritten only when it changed
#   -DMODE=cpp   -DSHA=<file> -DOUT=<cpp>                  kImageSha1, the release the executable accepts
#   -DMODE=kit   -DSHA=<file> -DIN=<toml> -DOUT=<toml>     a player kit's kit.toml with @XBE_SHA1@ filled in
if(MODE STREQUAL "stamp")
    if(NOT EXISTS "${XBE}")
        message(FATAL_ERROR "${XBE} is missing: put the game in assets/ (tools/build.sh --iso FILE)")
    endif()
    file(SHA1 "${XBE}" sha)
    set(old "")
    if(EXISTS "${OUT}")
        file(READ "${OUT}" old)
    endif()
    if(NOT old STREQUAL sha)
        file(WRITE "${OUT}" "${sha}")
    endif()
elseif(MODE STREQUAL "cpp")
    file(READ "${SHA}" sha)
    file(CONFIGURE OUTPUT "${OUT}" CONTENT "extern const char kImageSha1[] = \"${sha}\";\n")
elseif(MODE STREQUAL "kit")
    file(READ "${SHA}" sha)
    file(READ "${IN}" toml)
    string(REPLACE "@XBE_SHA1@" "${sha}" toml "${toml}")
    file(WRITE "${OUT}" "${toml}")
else()
    message(FATAL_ERROR "xbe_sha1.cmake: MODE is stamp, cpp or kit")
endif()
