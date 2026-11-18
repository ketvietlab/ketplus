# The runner's retry delay, shared by its Relay calls and its Relay socket.
add_library(ketplus_runner_backoff STATIC src/run/Backoff.cpp src/run/Backoff.h)
target_link_libraries(ketplus_runner_backoff PUBLIC Qt6::Core)
target_include_directories(ketplus_runner_backoff PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_backoff)

# ketplus-runner: the process on a build node. Identity, registration, pairing, access tokens
# and heartbeat; its keys live in its own state directory, never in an OS credential store.
add_library(ketplus_runner_app STATIC
    src/Environment.h
    src/ExitCode.h
    src/RunnerRules.cpp
    src/RunnerRules.h
    src/SystemEnvironment.cpp
    src/commands/Cli.cpp
    src/commands/CommandSupport.cpp
    src/commands/CommandSupport.h
    src/commands/Commands.h
    src/commands/ManageCommands.cpp
    src/commands/PairCommand.cpp
    src/commands/RegisterCommand.cpp
    src/flow/PairingFlow.cpp
    src/flow/PairingFlow.h
    src/flow/PendingPins.cpp
    src/flow/PendingPins.h
    src/relay/RelayClient.cpp
    src/relay/RelayClient.h
    src/relay/RelayLeaseCalls.cpp
    src/relay/RelayLeaseCalls.h
    src/run/AccessTokens.cpp
    src/run/AccessTokens.h
    src/run/RunLoop.cpp
    src/run/RunLoop.h
    src/state/AcceptedJobsFile.cpp
    src/state/AcceptedJobsFile.h
    src/state/JobJournalFile.cpp
    src/state/JobJournalFile.h
    src/state/PinTrust.cpp
    src/state/PinTrust.h
    src/state/RunnerState.cpp
    src/state/RunnerState.h
    src/state/StateDirectory.cpp
    src/state/StateDirectory.h
)
target_link_libraries(ketplus_runner_app
    PUBLIC Qt6::Core Qt6::Network ketplus_runner_backoff ketplus_runner_pairing
    ketplus_runner_protocol ketplus_runner_delivery ketplus_control_crypto_primitives
    PRIVATE ketplus_product_version OpenSSL::Crypto)
target_include_directories(ketplus_runner_app PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
# The same Relay selection as the daemon: only a Debug build with the remote demo reads
# KETPLUS_CONTROL_URL from the environment.
target_compile_definitions(ketplus_runner_app PRIVATE
    $<$<AND:$<BOOL:${KETPLUS_ENABLE_REMOTE_DEMO}>,$<CONFIG:Debug>>:KETPLUS_REMOTE_DEMO_AVAILABLE=1>
)
ketplus_enable_warnings(ketplus_runner_app)

# One job on the peer connection, as a state machine over interfaces: no socket, process or
# clock of its own.
add_library(ketplus_runner_session STATIC
    src/session/JobSession.h
    src/session/JobSessionChannel.cpp
    src/session/JobSessionOutput.cpp
    src/session/JobSessionResult.cpp
    src/session/JobSessionSpec.cpp
    src/session/JobSessionStop.cpp
    src/session/OutputBuffer.cpp
    src/session/OutputBuffer.h
    src/session/SessionPorts.h
)
target_link_libraries(ketplus_runner_session PUBLIC Qt6::Core ketplus_runner_protocol)
target_include_directories(ketplus_runner_session PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_session)

# The session's SnapshotReceiver over the state directory: partial packs, mirrors and
# workspaces, with disk and Git work on one worker thread.
add_library(ketplus_runner_snapshot_receiver STATIC
    src/snapshot/ReceiverChunks.cpp
    src/snapshot/ReceiverOffer.cpp
    src/snapshot/ReceiverStore.cpp
    src/snapshot/ReceiverStore.h
    src/snapshot/ReceiverWork.cpp
    src/snapshot/ReceiverWork.h
    src/snapshot/SnapshotStoreReceiver.h
)
target_link_libraries(ketplus_runner_snapshot_receiver
    PUBLIC Qt6::Core ketplus_runner_app ketplus_runner_session ketplus_runner_snapshot
    ketplus_runner_protocol)
ketplus_enable_warnings(ketplus_runner_snapshot_receiver)

# The runner's Relay socket and its peer connection to a desktop: pure logic over the
# interfaces in PeerPorts.h, plus the Qt and WebRTC adapters.
add_library(ketplus_runner_peer STATIC
    src/peer/PeerChannels.cpp
    src/peer/PeerChannels.h
    src/peer/PeerConnector.cpp
    src/peer/PeerConnector.h
    src/peer/PeerLog.cpp
    src/peer/PeerLog.h
    src/peer/PeerPorts.h
    src/peer/PeerSignals.cpp
    src/peer/PeerSignals.h
    src/peer/QtPeerClock.cpp
    src/peer/QtPeerClock.h
    src/peer/QtRelaySocket.cpp
    src/peer/QtRelaySocket.h
    src/peer/RelayLink.cpp
    src/peer/RelayLink.h
    src/peer/RelayWakeups.cpp
    src/peer/RelayWakeups.h
    src/peer/WebRtcTransport.cpp
    src/peer/WebRtcTransport.h
)
target_link_libraries(ketplus_runner_peer
    PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets ketplus_runner_backoff ketplus_runner_pairing
    ketplus_runner_session ketplus_peer_link
    PRIVATE ketplus_control_crypto_primitives)
target_include_directories(ketplus_runner_peer PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_peer)

# Job delivery: acquire, lease, renewal and journal, as a state machine over interfaces like
# the session.
add_library(ketplus_runner_delivery STATIC
    src/delivery/DeliveryAcquire.cpp
    src/delivery/DeliveryLease.cpp
    src/delivery/DeliveryPorts.h
    src/delivery/DeliveryRecords.cpp
    src/delivery/DeliveryRecords.h
    src/delivery/JobDelivery.h
    src/delivery/GuardedAcceptedJobs.h
    src/delivery/JobSessionLink.cpp
    src/delivery/JobSessionLink.h
)
target_link_libraries(ketplus_runner_delivery
    PUBLIC Qt6::Core ketplus_runner_backoff ketplus_runner_session)
target_include_directories(ketplus_runner_delivery PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_delivery)

add_executable(ketplus-runner src/main.cpp)
# The receiver brings ketplus_runner_app and ketplus_runner_session with it.
target_link_libraries(ketplus-runner PRIVATE ketplus_runner_snapshot_receiver
    ketplus_runner_delivery)
ketplus_enable_warnings(ketplus-runner)

# Runners are Linux nodes; the macOS build exists for tests only and ships no runner.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(TARGETS ketplus-runner RUNTIME DESTINATION bin COMPONENT Runtime)
    include("${PROJECT_SOURCE_DIR}/packaging/linux/RuntimePlugins.cmake")
    qt_generate_deploy_app_script(TARGET ketplus-runner OUTPUT_SCRIPT ketplus_runner_deploy
        NO_TRANSLATIONS EXCLUDE_PLUGIN_TYPES platforms platformthemes styles
        EXCLUDE_PLUGINS qxcb qoffscreen ${ketplus_linux_excluded_runtime_plugins})
    install(SCRIPT "${ketplus_runner_deploy}" COMPONENT Runtime)
endif()
