cmake_minimum_required(VERSION 3.24)
project(R03Characterization LANGUAGES CXX)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Characterization baseline requires Release")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Sql Test)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${REPO}/cmake/Warnings.cmake")
add_custom_target(r03_provenance
  COMMAND "${CMAKE_COMMAND}" -DREPO=${REPO} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/BuildProvenance.h
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BuildProvenance.cmake"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/BuildProvenance.h")
# The journal's storage sources come from the daemon's own list, so this baseline builds the
# journal the daemon ships.
include("${REPO}/apps/daemon/sources.cmake")
set(R03_JOURNAL_SOURCES ${KETPLUS_DAEMON_CORE_SOURCES})
list(FILTER R03_JOURNAL_SOURCES INCLUDE REGEX "^src/storage/.*\\.cpp$")
list(TRANSFORM R03_JOURNAL_SOURCES PREPEND "${REPO}/apps/daemon/")
add_executable(r03_characterization CharacterizationTest.cpp ${R03_JOURNAL_SOURCES}
  "${REPO}/apps/daemon/src/cloud/CloudEventProjection.cpp"
  "${REPO}/apps/daemon/src/protocol/DaemonProtocol.cpp")
add_dependencies(r03_characterization r03_provenance)
target_include_directories(r03_characterization PRIVATE "${REPO}/apps/daemon/src" "${REPO}/modules/ai/src"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(r03_characterization PRIVATE Qt6::Core Qt6::Gui Qt6::Sql Qt6::Test)
target_compile_definitions(r03_characterization PRIVATE R03_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
add_executable(r03_metrics_tests MetricsArtifactTest.cpp)
target_link_libraries(r03_metrics_tests PRIVATE Qt6::Core Qt6::Test)
ketplus_enable_warnings(r03_characterization)
ketplus_enable_warnings(r03_metrics_tests)
enable_testing()
add_test(NAME r03_characterization COMMAND r03_characterization)
add_test(NAME r03_metrics_tests COMMAND r03_metrics_tests)
