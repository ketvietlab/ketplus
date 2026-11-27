# Where external developer tools (node, npm, Git) live and how npm-installed CLIs are
# started, including Windows' npm `.cmd` shims. Shared by modules/ai and modules/atlas
# (modules/browser has its own node probes for now); Qt Core only.
add_library(ketplus_tool_locations STATIC
    src/ToolLocations.cpp
    src/ToolLocations.h
)
target_link_libraries(ketplus_tool_locations PUBLIC Qt6::Core)
target_include_directories(ketplus_tool_locations PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_tool_locations)
