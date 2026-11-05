# ADR 0030: host side of ketplus_computer (settings, per-conversation launch and policy text).
add_library(ketplus_computer_tools STATIC
    src/ComputerAgentInstructions.cpp
    src/ComputerAgentInstructions.h
    src/ComputerToolLaunch.cpp
    src/ComputerToolLaunch.h
)
target_link_libraries(ketplus_computer_tools PUBLIC Qt6::Core)
target_include_directories(ketplus_computer_tools PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_computer_tools)
