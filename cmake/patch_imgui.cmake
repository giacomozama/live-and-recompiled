# Two changes to the ImGui source (idempotent; run as cmake -P patch_imgui.cmake <imgui source dir>):
#  - The pad and keyboard selection outline sits a few pixels outside its widget, with the widget's own corner radius:
#    it takes that distance on its radius too, so its corners run round the widget's.
#  - A frame's border (FrameBorderSize) is two lines: ImGuiCol_Border on the frame's edge and ImGuiCol_BorderShadow in
#    a ring just outside it (ImGui offsets that one a pixel down and right instead).
set(file "${CMAKE_ARGV3}/imgui.cpp")
file(READ "${file}" text)
set(patched "${text}")
if(NOT patched MATCHES "rounding \\+= distance;")
    string(REPLACE "        display_rect.Expand(ImVec2(distance, distance));\n        bool fully_visible"
                   "        display_rect.Expand(ImVec2(distance, distance));\n        rounding += distance;\n        bool fully_visible" patched "${patched}")
    if(patched STREQUAL text)
        message(WARNING "patch_imgui: RenderNavCursor not found, the selection outline keeps ImGui's corners")
    endif()
endif()
set(before "${patched}")
if(NOT patched MATCHES "p_min - ImVec2\\(1, 1\\)")
    string(REPLACE "AddRect(p_min + ImVec2(1, 1), p_max + ImVec2(1, 1), GetColorU32(ImGuiCol_BorderShadow), rounding, border_size);"
                   "AddRect(p_min - ImVec2(1, 1), p_max + ImVec2(1, 1), GetColorU32(ImGuiCol_BorderShadow), rounding + 1.0f, 0, border_size);" patched "${patched}")
    string(REPLACE "AddRect(p_min, p_max, GetColorU32(ImGuiCol_Border), rounding, border_size);"
                   "AddRect(p_min, p_max, GetColorU32(ImGuiCol_Border), rounding, 0, border_size);" patched "${patched}")
    if(patched STREQUAL before)
        message(WARNING "patch_imgui: RenderFrameBorder not found, frame borders keep ImGui's shadow offset")
    endif()
endif()
if(NOT patched STREQUAL text)
    file(WRITE "${file}" "${patched}")
endif()
