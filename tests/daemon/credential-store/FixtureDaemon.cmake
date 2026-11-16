include_guard(GLOBAL)

# Exercise the production daemon entry point with a synthetic store shared across
# fixture process restarts. This executable is never installed or bundled.
add_executable(ketplus_fixture_daemon
    "${PROJECT_SOURCE_DIR}/apps/daemon/src/main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/FileCredentialBackend.cpp")
add_dependencies(ketplus_fixture_daemon ketplus_build_identity)
target_include_directories(ketplus_fixture_daemon PRIVATE
    "${PROJECT_BINARY_DIR}/apps/daemon/generated")
target_link_libraries(ketplus_fixture_daemon PRIVATE ketplus_daemon_core)
ketplus_enable_warnings(ketplus_fixture_daemon)
