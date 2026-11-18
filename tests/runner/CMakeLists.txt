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
# pairing, access tokens, heartbeat and job calls. No network beyond 127.0.0.1.
qt_add_executable(ketplus_runner_app_tests
    FakeRelay.cpp FakeRelay.h FakeRelayJobs.cpp RunnerAppDeliveryFilesTest.cpp RunnerAppJobCallsTest.cpp RunnerAppPairingTest.cpp
    RunnerAppRunTest.cpp RunnerAppSecretsTest.cpp RunnerAppStateTest.cpp RunnerAppTest.cpp RunnerAppTest.h)
ketplus_enable_warnings(ketplus_runner_app_tests)
target_link_libraries(ketplus_runner_app_tests PRIVATE
    Qt6::Test Qt6::Network ketplus_runner_app OpenSSL::Crypto)
add_test(NAME ketplus_runner_app_tests COMMAND ketplus_runner_app_tests)

# The runner's job session over fake channel, lease, snapshot receiver, command and clock.
qt_add_executable(ketplus_runner_session_tests
    RunnerSessionDeliveryTest.cpp RunnerSessionHandshakeTest.cpp RunnerSessionOutputTest.cpp
    RunnerSessionSpecTest.cpp RunnerSessionStopTest.cpp RunnerSessionTest.cpp RunnerSessionTest.h)
ketplus_enable_warnings(ketplus_runner_session_tests)
target_link_libraries(ketplus_runner_session_tests PRIVATE Qt6::Test ketplus_runner_session
    ketplus_runner_app)
add_test(NAME ketplus_runner_session_tests COMMAND ketplus_runner_session_tests)

# The runner's Relay socket and peer connection: fakes for every port, then real links on
# 127.0.0.1 (a desktop answerer through an in-process Relay, local WebSocket and HTTP servers).
qt_add_executable(ketplus_runner_peer_tests
    RunnerPeerTest.cpp RunnerPeerTest.h RunnerRelayLinkTest.cpp RunnerPeerConnectorTest.cpp
    RunnerPeerChannelsTest.cpp RunnerPeerLoopbackTest.cpp)
ketplus_enable_warnings(ketplus_runner_peer_tests)
target_link_libraries(ketplus_runner_peer_tests PRIVATE
    Qt6::Test Qt6::Network Qt6::WebSockets ketplus_runner_peer ketplus_runner_pairing
    ketplus_control_crypto_primitives)
add_test(NAME ketplus_runner_peer_tests COMMAND ketplus_runner_peer_tests)
set_tests_properties(ketplus_runner_peer_tests PROPERTIES TIMEOUT 180)

# Job delivery over fake Relay calls, session, journal, pins and clock.
qt_add_executable(ketplus_runner_delivery_tests
    RunnerDeliveryAcquireTest.cpp RunnerDeliveryLeaseTest.cpp RunnerDeliveryRestartTest.cpp
    RunnerDeliveryTest.cpp RunnerDeliveryTest.h)
ketplus_enable_warnings(ketplus_runner_delivery_tests)
target_link_libraries(ketplus_runner_delivery_tests PRIVATE Qt6::Test ketplus_runner_delivery)
add_test(NAME ketplus_runner_delivery_tests COMMAND ketplus_runner_delivery_tests)
ketplus_tests_offscreen()

# The runner's snapshot receiver with real packs from the snapshot library and the system git;
# temporary directories only, no network.
qt_add_executable(ketplus_runner_receiver_tests
    RunnerReceiverApplyTest.cpp RunnerReceiverChunkTest.cpp RunnerReceiverKeepTest.cpp
    RunnerReceiverTest.cpp RunnerReceiverTest.h)
ketplus_enable_warnings(ketplus_runner_receiver_tests)
target_link_libraries(ketplus_runner_receiver_tests PRIVATE
    Qt6::Test ketplus_runner_snapshot_receiver)
add_test(NAME ketplus_runner_receiver_tests COMMAND ketplus_runner_receiver_tests)
