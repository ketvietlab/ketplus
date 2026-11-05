cmake_minimum_required(VERSION 3.24)
project(R03Characterization LANGUAGES CXX)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Characterization baseline requires Release")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Sql Test)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_custom_target(r03_provenance
  COMMAND "${CMAKE_COMMAND}" -DREPO=${REPO} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/BuildProvenance.h
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BuildProvenance.cmake"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/BuildProvenance.h")
add_executable(r03_characterization CharacterizationTest.cpp
  "${REPO}/apps/daemon/src/cloud/CloudEventProjection.cpp"
  "${REPO}/apps/daemon/src/storage/LocalJournal.cpp"
  "${REPO}/apps/daemon/src/storage/LocalJournalActivityGroups.cpp"
  "${REPO}/apps/daemon/src/storage/LocalJournalContexts.cpp"
  "${REPO}/apps/daemon/src/storage/LocalJournalLargeEvents.cpp"
  "${REPO}/apps/daemon/src/protocol/DaemonProtocol.cpp")
add_dependencies(r03_characterization r03_provenance)
target_include_directories(r03_characterization PRIVATE "${REPO}/apps/daemon/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(r03_characterization PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_definitions(r03_characterization PRIVATE R03_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
add_executable(r03_metrics_tests MetricsArtifactTest.cpp)
target_link_libraries(r03_metrics_tests PRIVATE Qt6::Core Qt6::Test)
enable_testing()
add_test(NAME r03_characterization COMMAND r03_characterization)
add_test(NAME r03_metrics_tests COMMAND r03_metrics_tests)
