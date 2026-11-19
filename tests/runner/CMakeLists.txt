# Windows builds only the snapshot library and its test: the rest of the runner is Linux-only
# (apps/runner is not built there), and WorktreeSnapshotTest uses POSIX fixtures.
if(WIN32)
    qt_add_executable(ketplus_runner_snapshot_windows_tests WorktreeSnapshotWindowsTest.cpp)
    ketplus_enable_warnings(ketplus_runner_snapshot_windows_tests)
    target_link_libraries(ketplus_runner_snapshot_windows_tests PRIVATE Qt6::Test
        ketplus_runner_snapshot)
    add_test(NAME ketplus_runner_snapshot_windows_tests
        COMMAND ketplus_runner_snapshot_windows_tests)
    return()
endif()

# Runner worktree snapshots against the system git; temporary repositories only, no network.
qt_add_executable(ketplus_runner_snapshot_tests WorktreeSnapshotTest.cpp)
ketplus_enable_warnings(ketplus_runner_snapshot_tests)
target_link_libraries(ketplus_runner_snapshot_tests PRIVATE Qt6::Test ketplus_runner_snapshot)
add_test(NAME ketplus_runner_snapshot_tests COMMAND ketplus_runner_snapshot_tests)
qt_add_executable(ketplus_runner_snapshot_store_tests SnapshotStoreTest.cpp)
ketplus_enable_warnings(ketplus_runner_snapshot_store_tests)
target_link_libraries(ketplus_runner_snapshot_store_tests PRIVATE Qt6::Test ketplus_runner_snapshot)
add_test(NAME ketplus_runner_snapshot_store_tests COMMAND ketplus_runner_snapshot_store_tests)

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
    FakeRelay.cpp FakeRelay.h FakeRelayJobs.cpp RunnerAppDeliveryFilesTest.cpp RunnerAppLoopHost.cpp RunnerAppJobCallsTest.cpp RunnerAppPairingTest.cpp
    RunnerAppRunTest.cpp RunnerAppSecretsTest.cpp RunnerAppStateTest.cpp RunnerAppTest.cpp RunnerAppTest.h)
ketplus_enable_warnings(ketplus_runner_app_tests)
target_link_libraries(ketplus_runner_app_tests PRIVATE
    Qt6::Test Qt6::Network ketplus_runner_run OpenSSL::Crypto)
add_test(NAME ketplus_runner_app_tests COMMAND ketplus_runner_app_tests)

# `ketplus-runner run`: the run loop over a fake Relay, socket, peer transport, command
# runner and clock, with the real job pieces, journal, accepted jobs and pins.
qt_add_executable(ketplus_runner_run_tests
    RunnerRunCleanupTest.cpp RunnerRunJobTest.cpp RunnerRunLoopbackTest.cpp RunnerRunSnapshotTest.cpp RunnerRunStartTest.cpp RunnerRunStopTest.cpp
    RunnerRunTest.cpp RunnerRunTest.h)
ketplus_enable_warnings(ketplus_runner_run_tests)
target_link_libraries(ketplus_runner_run_tests PRIVATE
    Qt6::Test ketplus_runner_run ketplus_runner_snapshot ketplus_control_crypto_primitives)
add_test(NAME ketplus_runner_run_tests COMMAND ketplus_runner_run_tests)
set_tests_properties(ketplus_runner_run_tests PROPERTIES TIMEOUT 180)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ketplus_job_executor)
    # Real jobs through the run loop; they run only with KETPLUS_RUN_LINUX_JOB_TESTS=1.
    target_sources(ketplus_runner_run_tests PRIVATE RunnerRunLinuxTest.cpp)
    target_compile_definitions(ketplus_runner_run_tests PRIVATE
        KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>")
    add_dependencies(ketplus_runner_run_tests ketplus_job_executor)
endif()

# The runner's job session over fake channel, lease, snapshot receiver, command and clock.
qt_add_executable(ketplus_runner_session_tests
    RunnerSessionDeliveryTest.cpp RunnerSessionHandshakeTest.cpp
    RunnerSessionLostOutputTest.cpp RunnerSessionOutputTest.cpp
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

# The runner's command executor: launch plans, mounts, task ids and results as tables, job
# directories in temporary directories, and the command runner over a fake executor.
qt_add_executable(ketplus_runner_exec_plan_tests RunnerExecPlanTest.cpp)
ketplus_enable_warnings(ketplus_runner_exec_plan_tests)
target_link_libraries(ketplus_runner_exec_plan_tests PRIVATE Qt6::Test ketplus_runner_exec)
add_test(NAME ketplus_runner_exec_plan_tests COMMAND ketplus_runner_exec_plan_tests)

qt_add_executable(ketplus_runner_exec_tests
    RunnerExecCleanupTest.cpp RunnerExecLinuxPolicyTest.cpp RunnerExecMountsTest.cpp
    RunnerExecRunnerTest.cpp RunnerExecTest.cpp RunnerExecTest.h)
ketplus_enable_warnings(ketplus_runner_exec_tests)
target_link_libraries(ketplus_runner_exec_tests PRIVATE Qt6::Test ketplus_runner_exec)
add_test(NAME ketplus_runner_exec_tests COMMAND ketplus_runner_exec_tests)

# The command runner over the real job executor of this build, on macOS and Linux: environment,
# cwd, output, kill and restart cleanup with real processes in temporary directories. On Linux it
# needs a prepared user session and runs only with KETPLUS_RUN_LINUX_JOB_TESTS=1.
if((APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND TARGET ketplus_runner_exec_native)
    qt_add_executable(ketplus_runner_exec_native_tests RunnerExecNativeTest.cpp)
    ketplus_enable_warnings(ketplus_runner_exec_native_tests)
    target_link_libraries(ketplus_runner_exec_native_tests PRIVATE
        Qt6::Test ketplus_runner_exec_native)
    target_compile_definitions(ketplus_runner_exec_native_tests PRIVATE
        KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>")
    add_dependencies(ketplus_runner_exec_native_tests ketplus_job_executor)
    add_test(NAME ketplus_runner_exec_native_tests COMMAND ketplus_runner_exec_native_tests)
    set_tests_properties(ketplus_runner_exec_native_tests PROPERTIES TIMEOUT 300)
endif()

# Linux job containment with the real executor: the policy, the job's mounts seen from inside and
# outside, stop and restart cleanup. Runs only with KETPLUS_RUN_LINUX_JOB_TESTS=1.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ketplus_runner_exec_native)
    qt_add_executable(ketplus_runner_linux_job_tests RunnerLinuxJobTest.cpp)
    ketplus_enable_warnings(ketplus_runner_linux_job_tests)
    target_link_libraries(ketplus_runner_linux_job_tests PRIVATE
        Qt6::Test ketplus_runner_exec_native)
    target_compile_definitions(ketplus_runner_linux_job_tests PRIVATE
        KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>")
    add_dependencies(ketplus_runner_linux_job_tests ketplus_job_executor)
    add_test(NAME ketplus_runner_linux_job_tests COMMAND ketplus_runner_linux_job_tests)
    set_tests_properties(ketplus_runner_linux_job_tests PROPERTIES TIMEOUT 900)
endif()

# Real jobs through `run` as the runner's main wires it (RunnerAssembly::create), with the job
# executor beside the test binary, the fake Relay over HTTP and a paired desktop the test plays;
# then a runner killed during its job and started again. Runs only with
# KETPLUS_RUN_LINUX_JOB_TESTS=1.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ketplus_job_executor)
    qt_add_executable(ketplus_runner_assembly_linux_tests
        FakeRelay.cpp FakeRelay.h FakeRelayJobs.cpp RunnerAssemblyLinuxTest.cpp
        RunnerAssemblyRig.cpp RunnerAssemblyRig.h)
    ketplus_enable_warnings(ketplus_runner_assembly_linux_tests)
    target_link_libraries(ketplus_runner_assembly_linux_tests PRIVATE
        Qt6::Test Qt6::Network ketplus_runner_run ketplus_runner_snapshot
        ketplus_control_crypto_primitives OpenSSL::Crypto)
    add_dependencies(ketplus_runner_assembly_linux_tests ketplus_job_executor)
    add_test(NAME ketplus_runner_assembly_linux_tests COMMAND ketplus_runner_assembly_linux_tests)
    set_tests_properties(ketplus_runner_assembly_linux_tests PROPERTIES TIMEOUT 900)
endif()

# The test Relay as a program, for the loopback test package of packaging/linux/runner. Never
# installed or packaged.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    qt_add_executable(ketplus_runner_fake_relay_server
        FakeRelay.cpp FakeRelay.h FakeRelayJobs.cpp FakeRelayServer.cpp)
    ketplus_enable_warnings(ketplus_runner_fake_relay_server)
    target_link_libraries(ketplus_runner_fake_relay_server PRIVATE
        Qt6::Network ketplus_runner_run OpenSSL::Crypto)
endif()
