cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KetPlusBackgroundOutputTests LANGUAGES CXX)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Core Sql Test)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(KETPLUS_BACKGROUND_OUTPUT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
add_executable(ketplus_background_output_tests
    ../BackgroundTaskOutputTest.cpp
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskOutput.cpp"
)
target_include_directories(ketplus_background_output_tests PRIVATE
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src"
)
target_link_libraries(ketplus_background_output_tests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
if(COMMAND ketplus_enable_warnings)
    ketplus_enable_warnings(ketplus_background_output_tests)
endif()
add_test(NAME ketplus_background_output_tests COMMAND ketplus_background_output_tests)
