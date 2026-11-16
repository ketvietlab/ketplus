cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KetPlusAgentBackgroundToolsTests LANGUAGES CXX)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Test)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(agent_background_root "${CMAKE_CURRENT_LIST_DIR}/../../..")
add_executable(ketplus_agent_background_tools_tests ../AgentBackgroundToolsTest.cpp)
if(TARGET ketplus_daemon_core)
    target_link_libraries(ketplus_agent_background_tools_tests PRIVATE ketplus_daemon_core)
else()
    # A narrow offline target, also usable when a full daemon build is unavailable.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/KetPlusProductVersion.h"
        "#pragma once\n#define KETPLUS_PRODUCT_VERSION \"test\"\n")
    target_sources(ketplus_agent_background_tools_tests PRIVATE
        "${agent_background_root}/apps/daemon/src/agents/AgentMcpServer.cpp"
        "${agent_background_root}/apps/daemon/src/agents/AgentTeamTools.cpp"
        "${agent_background_root}/apps/daemon/src/agents/AgentInstructions.cpp"
        "${agent_background_root}/apps/daemon/src/protocol/DaemonProtocol.cpp")
    target_include_directories(ketplus_agent_background_tools_tests PRIVATE
        "${agent_background_root}/apps/daemon/src" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
target_link_libraries(ketplus_agent_background_tools_tests PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME ketplus_agent_background_tools_tests COMMAND ketplus_agent_background_tools_tests)
