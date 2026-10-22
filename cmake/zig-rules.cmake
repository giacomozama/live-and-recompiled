# zig-rules.cmake - the Zig builds' static libraries with `zig ar` and `zig ranlib`. CMAKE_AR is a single program, so
# the archive commands name Zig and the subcommand themselves. Read as CMAKE_USER_MAKE_RULES_OVERRIDE (zig-windows.cmake,
# zig-glibc.cmake): after the platform files, which set these commands for Windows targets, so it wins over them.
foreach(lang C CXX)
    set(CMAKE_${lang}_ARCHIVE_CREATE "\"${ZIG_HOST_DIR}/zig${HOST_EXE}\" ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_${lang}_ARCHIVE_APPEND "\"${ZIG_HOST_DIR}/zig${HOST_EXE}\" ar q <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_${lang}_ARCHIVE_FINISH "\"${ZIG_HOST_DIR}/zig${HOST_EXE}\" ranlib <TARGET>")
endforeach()
