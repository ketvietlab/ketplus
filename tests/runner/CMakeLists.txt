# Runner worktree snapshots against the system git; temporary repositories only, no network.
qt_add_executable(ketplus_runner_snapshot_tests WorktreeSnapshotTest.cpp)
ketplus_enable_warnings(ketplus_runner_snapshot_tests)
target_link_libraries(ketplus_runner_snapshot_tests PRIVATE Qt6::Test ketplus_runner_snapshot)
add_test(NAME ketplus_runner_snapshot_tests COMMAND ketplus_runner_snapshot_tests)

# Runner pairing library alone: no daemon or desktop code, no network.
qt_add_executable(ketplus_runner_pairing_tests RunnerPairingTest.cpp)
ketplus_enable_warnings(ketplus_runner_pairing_tests)
target_link_libraries(ketplus_runner_pairing_tests PRIVATE Qt6::Test ketplus_runner_pairing)
add_test(NAME ketplus_runner_pairing_tests COMMAND ketplus_runner_pairing_tests)
ketplus_tests_offscreen()
