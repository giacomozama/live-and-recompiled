# version.cmake - writes OUT, a header with XBE_VERSION: `git describe` of the checkout (the release archives' name,
# tools/package.cmake), "dev" without git. Run every build; the header is rewritten only when the version changes, so
# only what includes it recompiles.
execute_process(COMMAND git -C ${ROOT} describe --tags --always --dirty OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE code ERROR_QUIET)
if(code OR NOT version)
    set(version dev)
endif()
set(content "#pragma once\n#define XBE_VERSION \"${version}\"\n")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT old STREQUAL content)
    file(WRITE ${OUT} "${content}")
endif()
