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

# Runner peer protocol frames: round trips, limits, malformed input and routing; no network.
qt_add_executable(ketplus_runner_protocol_tests
    RunnerProtocolTest.cpp RunnerProtocolLimitsTest.cpp RunnerProtocolTest.h)
ketplus_enable_warnings(ketplus_runner_protocol_tests)
target_link_libraries(ketplus_runner_protocol_tests PRIVATE Qt6::Test ketplus_runner_protocol)
add_test(NAME ketplus_runner_protocol_tests COMMAND ketplus_runner_protocol_tests)

# ketplus-runner against an in-process Relay on loopback: state directory, registration,
# pairing, access tokens and heartbeat. No network beyond 127.0.0.1.
qt_add_executable(ketplus_runner_app_tests
    FakeRelay.cpp FakeRelay.h RunnerAppPairingTest.cpp RunnerAppRunTest.cpp
    RunnerAppSecretsTest.cpp RunnerAppStateTest.cpp RunnerAppTest.cpp RunnerAppTest.h)
ketplus_enable_warnings(ketplus_runner_app_tests)
target_link_libraries(ketplus_runner_app_tests PRIVATE
    Qt6::Test Qt6::Network ketplus_runner_app OpenSSL::Crypto)
add_test(NAME ketplus_runner_app_tests COMMAND ketplus_runner_app_tests)

# The runner's job session over fake channel, lease, snapshot receiver, command and clock.
qt_add_executable(ketplus_runner_session_tests
    RunnerSessionHandshakeTest.cpp RunnerSessionOutputTest.cpp RunnerSessionSpecTest.cpp
    RunnerSessionStopTest.cpp RunnerSessionTest.cpp RunnerSessionTest.h)
ketplus_enable_warnings(ketplus_runner_session_tests)
target_link_libraries(ketplus_runner_session_tests PRIVATE Qt6::Test ketplus_runner_session)
add_test(NAME ketplus_runner_session_tests COMMAND ketplus_runner_session_tests)
ketplus_tests_offscreen()
