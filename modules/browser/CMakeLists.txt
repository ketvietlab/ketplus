include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")
add_library(ketplus_browser_tools STATIC ${KETPLUS_BROWSER_TOOLS_SOURCES})
target_link_libraries(ketplus_browser_tools PUBLIC Qt6::Core)
target_include_directories(ketplus_browser_tools PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_browser_tools)
