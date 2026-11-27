cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KetPlusAcpIsolationTests LANGUAGES CXX)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Test)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
get_filename_component(acp_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(ketplus_acp_session_isolation_tests ../AcpSessionIsolationTest.cpp)
if(TARGET ketplus_daemon_core)
    target_link_libraries(ketplus_acp_session_isolation_tests PRIVATE ketplus_daemon_core)
else()
    include("${acp_root}/apps/daemon/sources.cmake")
    list(FILTER KETPLUS_DAEMON_CORE_SOURCES INCLUDE REGEX
        "src/providers/(acp/|compat/|Cursor|Copilot)")
    list(TRANSFORM KETPLUS_DAEMON_CORE_SOURCES PREPEND "${acp_root}/apps/daemon/")
    include("${acp_root}/modules/browser/sources.cmake")
    list(TRANSFORM KETPLUS_BROWSER_TOOLS_SOURCES PREPEND "${acp_root}/modules/browser/")
    target_sources(ketplus_acp_session_isolation_tests PRIVATE
        ${KETPLUS_DAEMON_CORE_SOURCES} ${KETPLUS_BROWSER_TOOLS_SOURCES}
        "${acp_root}/apps/daemon/src/providers/ProviderAdapter.h"
        "${acp_root}/modules/computer/src/ComputerAgentInstructions.cpp"
        "${acp_root}/modules/computer/src/ComputerToolLaunch.cpp")
    target_include_directories(ketplus_acp_session_isolation_tests PRIVATE
        "${acp_root}/apps/daemon/src" "${acp_root}/modules/browser/src"
        "${acp_root}/modules/computer/src")
endif()
target_link_libraries(ketplus_acp_session_isolation_tests PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME ketplus_acp_session_isolation_tests COMMAND ketplus_acp_session_isolation_tests)
set_tests_properties(ketplus_acp_session_isolation_tests PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;KETPLUS_BROWSER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/disabled-browser.json;KETPLUS_COMPUTER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/disabled-computer.json;KETPLUS_COMPUTER_DIR=${CMAKE_CURRENT_BINARY_DIR}/computer")
# Its fixture provider is a Python script; on Windows the script launcher turns it into an
# executable (see tests/daemon/ScriptFixture.h), which the parent directory defines.
if(WIN32)
    if(TARGET ketplus_script_launcher)
        add_dependencies(ketplus_acp_session_isolation_tests ketplus_script_launcher)
        set_tests_properties(ketplus_acp_session_isolation_tests PROPERTIES
            LABELS "windows-script-fixture")
    else()
        set_tests_properties(ketplus_acp_session_isolation_tests PROPERTIES DISABLED TRUE
            LABELS "windows-script-fixture")
    endif()
endif()

# Area label inside the KetPlus build (cmake/TestLabels.cmake); a standalone build has none.
if(COMMAND ketplus_label_tests)
    ketplus_label_tests(daemon)
endif()
