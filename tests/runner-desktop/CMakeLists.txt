# The real ketplus-runner against the desktop daemon's real remote job parts, over a fake Relay
# and real WebRTC on this machine. Linux with job containment, a Debug build with
# KETPLUS_ENABLE_REMOTE_DEMO=ON, and KETPLUS_RUN_RUNNER_DESKTOP_E2E=1; it skips otherwise.
# Label runner_desktop_e2e; not part of the fast set.
qt_add_executable(ketplus_runner_desktop_e2e_tests
    DesktopEnd.cpp DesktopEnd.h
    RunnerDesktopRig.cpp RunnerDesktopRig.h
    RunnerDesktopTest.cpp
    RunnerProcess.cpp RunnerProcess.h
    SkippableJobClock.cpp SkippableJobClock.h
    "${PROJECT_SOURCE_DIR}/tests/daemon/cloud-privacy/MemoryCredentials.cpp")
ketplus_enable_warnings(ketplus_runner_desktop_e2e_tests)
target_include_directories(ketplus_runner_desktop_e2e_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/daemon")
target_link_libraries(ketplus_runner_desktop_e2e_tests PRIVATE
    Qt6::Test ketplus_daemon_core ketplus_runner_fake_relay ketplus_runner_exec_native)
target_compile_definitions(ketplus_runner_desktop_e2e_tests PRIVATE
    KETPLUS_RUNNER_PATH="$<TARGET_FILE:ketplus-runner>"
    KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>"
    KETPLUS_E2E_ROOT="${CMAKE_CURRENT_BINARY_DIR}/e2e"
    KETPLUS_PRODUCT_VERSION="${PROJECT_VERSION}")
add_dependencies(ketplus_runner_desktop_e2e_tests ketplus-runner ketplus_job_executor)
add_test(NAME ketplus_runner_desktop_e2e_tests COMMAND ketplus_runner_desktop_e2e_tests)
set_tests_properties(ketplus_runner_desktop_e2e_tests PROPERTIES
    LABELS runner_desktop_e2e TIMEOUT 300 RUN_SERIAL TRUE)
