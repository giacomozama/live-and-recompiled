# The pad and keyboard selection outline sits a few pixels outside its widget, with the widget's own corner radius:
# it takes that distance on its radius too, so its corners run round the widget's. Idempotent; run on the imgui
# source (cmake -P patch_imgui.cmake <imgui source dir>).
set(file "${CMAKE_ARGV3}/imgui.cpp")
file(READ "${file}" text)
if(NOT text MATCHES "rounding \\+= distance;")
    string(REPLACE "        display_rect.Expand(ImVec2(distance, distance));\n        bool fully_visible"
                   "        display_rect.Expand(ImVec2(distance, distance));\n        rounding += distance;\n        bool fully_visible" patched "${text}")
    if(patched STREQUAL text)
        message(WARNING "patch_imgui: RenderNavCursor not found, the selection outline keeps ImGui's corners")
    else()
        file(WRITE "${file}" "${patched}")
    endif()
endif()
