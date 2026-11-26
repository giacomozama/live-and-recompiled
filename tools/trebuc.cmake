# trebuc.cmake: Microsoft's trebuc32.exe (the Trebuchet MS installer, its licence in THIRD_PARTY.txt), downloaded into
# toolchain/archives when not there. Included by package.cmake, which puts it in the Linux Setup; run-linux.sh runs it
# as cmake -P tools/trebuc.cmake and takes the font out for its run folder
include(${CMAKE_CURRENT_LIST_DIR}/toolchains.cmake)
set(TREBUC_EXE ${TOOLCHAIN_ROOT}/archives/trebuc32.exe)
file(DOWNLOAD https://github.com/pushcx/corefonts/raw/refs/heads/master/trebuc32.exe ${TREBUC_EXE}
    EXPECTED_HASH SHA256=5a690d9bb8510be1b8b4fe49f1f2319651fe51bbe54775ddddd8ef0bd07fdac9 STATUS status)
list(GET status 0 code)
if(code)
    message(FATAL_ERROR "could not download trebuc32.exe: ${status}")
endif()
