include("${CMAKE_CURRENT_LIST_DIR}/credential-store/FixtureDaemon.cmake")

# Tests whose provider fixtures are `#!/usr/bin/env python3` scripts launched by path
# (FakeCodex.h, FakeClaude.h, FakeCursor.h, FakeCopilot*.h). Windows cannot execute those
# directly, so there each fixture is an .exe copy of ketplus_script_launcher that runs
# "<fixture>.py" with the Python interpreter (ScriptFixture.h). The label lets ctest select them.
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        qt_add_executable(ketplus_script_launcher ScriptLauncher.cpp)
        target_compile_definitions(ketplus_script_launcher PRIVATE
            UNICODE _UNICODE KETPLUS_SCRIPT_INTERPRETER="${Python3_EXECUTABLE}")
        set_target_properties(ketplus_script_launcher PROPERTIES WIN32_EXECUTABLE FALSE)
        # Every test that writes a fixture needs the path (ScriptFixture.h), also those outside
        # ketplus_add_daemon_test.
        add_compile_definitions(KETPLUS_SCRIPT_LAUNCHER="$<TARGET_FILE:ketplus_script_launcher>")
    endif()
endif()
set(KETPLUS_WINDOWS_SCRIPT_FIXTURE_TESTS
    ketplus_context_harness_tests
    ketplus_provider_deletion_tests
    ketplus_daemon_worktree_context_tests
    ketplus_daemon_worktree_activity_tests
    ketplus_daemon_claude_worktree_tests
    ketplus_daemon_lifecycle_tests
    ketplus_turn_acceptance_tests
    ketplus_daemon_claude_adapter_tests
    ketplus_computer_tool_launch_tests
    ketplus_computer_provider_integration_tests
    ketplus_daemon_cursor_adapter_tests
    ketplus_daemon_session_fork_tests
    ketplus_agent_messaging_tests
    ketplus_agent_urgent_delivery_tests
    ketplus_team_routing_tools_tests
    ketplus_team_finding_routing_tests
    ketplus_team_task_tests
    ketplus_team_task_git_tests
    ketplus_team_agent_tools_tests
    ketplus_team_flow_tests
    ketplus_team_title_tests
    ketplus_team_reply_tests
    ketplus_team_review_tests
    ketplus_team_lane_tests
    ketplus_team_budget_tests
    ketplus_team_merge_tests
    ketplus_team_done_tests
    ketplus_team_storage_tests
    ketplus_work_report_tests
    ketplus_awaiting_reply_tests
    ketplus_turn_review_tests
    ketplus_provider_compatibility_tests
    ketplus_daemon_schedule_host_tests
    ketplus_claude_credentials_tests
    ketplus_daemon_upgrade_tests
    ketplus_async_question_lifetime_tests
    ketplus_relay_attachment_host_tests
    ketplus_session_preparation_host_tests
    ketplus_relay_preparation_host_tests
    ketplus_codex_session_isolation_tests)

function(ketplus_add_daemon_test target source)
    qt_add_executable(${target} ${source})
    ketplus_enable_warnings(${target})
    target_link_libraries(${target} PRIVATE Qt6::Test ketplus_daemon_core)
    add_test(NAME ${target} COMMAND ${target})
    if(WIN32 AND target IN_LIST KETPLUS_WINDOWS_SCRIPT_FIXTURE_TESTS)
        set_tests_properties(${target} PROPERTIES LABELS "windows-script-fixture")
        if(TARGET ketplus_script_launcher)
            add_dependencies(${target} ketplus_script_launcher)
        else()
            set_tests_properties(${target} PROPERTIES DISABLED TRUE)
        endif()
    endif()
    set_tests_properties(${target} PROPERTIES ENVIRONMENT
        "KETPLUS_DISABLE_DAEMON_AUTOSTART=1;KETPLUS_BROWSER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/${target}-browser-tools.json;KETPLUS_COMPUTER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/${target}-computer-tools.json;KETPLUS_COMPUTER_DIR=${CMAKE_CURRENT_BINARY_DIR}/${target}-computer")
endfunction()

qt_add_executable(ketplus_daemon_client_upgrade_policy_tests DaemonClientUpgradePolicyTest.cpp)
target_link_libraries(ketplus_daemon_client_upgrade_policy_tests PRIVATE Qt6::Test ketplus_remote)
target_compile_definitions(ketplus_daemon_client_upgrade_policy_tests PRIVATE
    KETPLUS_EXPECT_AUTO_UPGRADE=$<BOOL:${KETPLUS_AUTO_UPGRADE_DAEMON}>)
ketplus_enable_warnings(ketplus_daemon_client_upgrade_policy_tests)
add_test(NAME ketplus_daemon_client_upgrade_policy_tests
    COMMAND ketplus_daemon_client_upgrade_policy_tests)

qt_add_executable(ketplus_machine_backend_tests MachineBackendTest.cpp)
target_link_libraries(ketplus_machine_backend_tests PRIVATE Qt6::Test ketplus_remote)
ketplus_enable_warnings(ketplus_machine_backend_tests)
add_test(NAME ketplus_machine_backend_tests COMMAND ketplus_machine_backend_tests)

ketplus_add_daemon_test(ketplus_headless_daemon_tests HeadlessDaemonTest.cpp)
target_compile_definitions(ketplus_headless_daemon_tests PRIVATE
    KETPLUS_TEST_DAEMON="$<TARGET_FILE:ketplus_fixture_daemon>" KETPLUS_TEST_CTL="$<TARGET_FILE:ketplusctl>")
add_dependencies(ketplus_headless_daemon_tests ketplus_fixture_daemon ketplusctl)
ketplus_add_daemon_test(ketplus_device_authorization_tests DeviceAuthorizationTest.cpp)
ketplus_add_daemon_test(ketplus_atlas_access_tests AtlasAccessTest.cpp)
target_sources(ketplus_device_authorization_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_sources(ketplus_atlas_access_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_remote_control_client_tests RemoteControlClientTest.cpp)
target_sources(ketplus_remote_control_client_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_remote_control_client_tests PRIVATE ketplus_remote_control)
ketplus_add_daemon_test(ketplus_remote_event_feed_tests RelayEventFeedTest.cpp)
ketplus_add_daemon_test(ketplus_remote_event_stream_tests RelayEventStreamTest.cpp)
target_link_libraries(ketplus_remote_event_stream_tests PRIVATE ketplus_remote_control)
target_sources(ketplus_remote_event_feed_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_remote_event_feed_tests PRIVATE ketplus_remote_control)
ketplus_add_daemon_test(ketplus_remote_control_projection_tests RemoteControlProjectionTest.cpp)
target_sources(ketplus_remote_control_projection_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_remote_control_projection_tests PRIVATE ketplus_remote_control)
ketplus_add_daemon_test(ketplus_remote_relay_backend_tests RemoteRelayBackendTest.cpp)
target_sources(ketplus_remote_relay_backend_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_remote_relay_backend_tests PRIVATE ketplus_remote_control)

# ADR 0124 release trust and installer state machine; no provider traffic.
qt_add_executable(ketplus_daemon_release_tests DaemonReleaseTest.cpp)
target_link_libraries(ketplus_daemon_release_tests PRIVATE Qt6::Test ketplus_remote_ssh)
ketplus_enable_warnings(ketplus_daemon_release_tests)
add_test(NAME ketplus_daemon_release_tests COMMAND ketplus_daemon_release_tests)
include(${CMAKE_CURRENT_SOURCE_DIR}/SshInstallTransactionTest.cmake)

qt_add_executable(ketplus_remote_ssh_tunnel_tests SshTunnelTest.cpp)
ketplus_enable_warnings(ketplus_remote_ssh_tunnel_tests)
target_link_libraries(ketplus_remote_ssh_tunnel_tests PRIVATE Qt6::Test ketplus_remote_ssh)
add_test(NAME ketplus_remote_ssh_tunnel_tests COMMAND ketplus_remote_ssh_tunnel_tests)

qt_add_executable(ketplus_remote_known_hosts_tests KnownHostsFileTest.cpp)
ketplus_enable_warnings(ketplus_remote_known_hosts_tests)
target_link_libraries(ketplus_remote_known_hosts_tests PRIVATE Qt6::Test ketplus_remote_ssh)
add_test(NAME ketplus_remote_known_hosts_tests COMMAND ketplus_remote_known_hosts_tests)

qt_add_executable(ketplus_remote_ssh_backend_tests SshMachineBackendTest.cpp)
ketplus_enable_warnings(ketplus_remote_ssh_backend_tests)
target_link_libraries(ketplus_remote_ssh_backend_tests PRIVATE Qt6::Test ketplus_remote_ssh_backend)
add_test(NAME ketplus_remote_ssh_backend_tests COMMAND ketplus_remote_ssh_backend_tests)

# ADR 0119 over SSH with a real ketplus_daemon behind a loopback sshd and FakeCodex.
qt_add_executable(ketplus_remote_ssh_daemon_integration_tests SshDaemonIntegrationTest.cpp)
ketplus_enable_warnings(ketplus_remote_ssh_daemon_integration_tests)
target_link_libraries(ketplus_remote_ssh_daemon_integration_tests PRIVATE
    Qt6::Test ketplus_remote_ssh_backend)
target_compile_definitions(ketplus_remote_ssh_daemon_integration_tests PRIVATE
    KETPLUS_TEST_DAEMON="$<TARGET_FILE:ketplus_fixture_daemon>")
add_dependencies(ketplus_remote_ssh_daemon_integration_tests ketplus_fixture_daemon)
add_test(NAME ketplus_remote_ssh_daemon_integration_tests
    COMMAND ketplus_remote_ssh_daemon_integration_tests)

ketplus_add_daemon_test(ketplus_atlas_live_runtime_tests AtlasLiveRuntimeTest.cpp)
ketplus_add_daemon_test(ketplus_atlas_live_tunnel_tests AtlasLiveTunnelTest.cpp)
ketplus_add_daemon_test(ketplus_provider_catalog_cache_tests ProviderCatalogCacheTest.cpp)
ketplus_add_daemon_test(ketplus_copilot_cli_tests CopilotCliTest.cpp)
target_compile_definitions(ketplus_copilot_cli_tests PRIVATE
    KETPLUS_TEST_DAEMON="$<TARGET_FILE:ketplus_fixture_daemon>")
add_dependencies(ketplus_copilot_cli_tests ketplus_fixture_daemon)
ketplus_add_daemon_test(ketplus_daemon_protocol_tests DaemonProtocolTest.cpp)
if(APPLE)
    # Each event delivery drains an autorelease pool; QCoreApplication on macOS never does.
    ketplus_add_daemon_test(ketplus_daemon_autorelease_pool_tests DaemonAutoreleasePoolTest.cpp)
    target_sources(ketplus_daemon_autorelease_pool_tests PRIVATE
        AutoreleaseProbe.mm AutoreleaseProbe.h)
    target_link_libraries(ketplus_daemon_autorelease_pool_tests PRIVATE "-framework Foundation")
endif()
ketplus_add_daemon_test(ketplus_schedule_rule_tests ScheduleRuleTest.cpp)
ketplus_add_daemon_test(ketplus_schedule_protocol_tests ScheduleProtocolTest.cpp)
ketplus_add_daemon_test(ketplus_schedule_runner_tests ScheduleRunnerTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_media_preview_tests McpMediaPreviewTest.cpp)
ketplus_add_daemon_test(ketplus_context_harness_tests ContextHarnessTest.cpp)
ketplus_add_daemon_test(ketplus_provider_deletion_tests ProviderDeletionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_worktree_context_tests DaemonWorktreeContextTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_worktree_activity_tests DaemonWorktreeActivityTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_claude_worktree_tests ClaudeWorktreeTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_context_relay_tests WorktreeContextRelayTest.cpp)
# Link in-memory credential symbols before the static core; never use the OS key store.
target_sources(ketplus_daemon_context_relay_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_cloud_trust_lifecycle_tests cloud-privacy/CloudTrustLifecycleTest.cpp)
target_sources(ketplus_cloud_trust_lifecycle_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_sender_key_cache_tests cloud-privacy/SenderKeyCacheTest.cpp)
target_sources(ketplus_sender_key_cache_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_daemon_cloud_privacy_tests cloud-privacy/CloudPrivacyTest.cpp)
target_sources(ketplus_daemon_cloud_privacy_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_heartbeat_worktree_catalog_tests cloud-privacy/HeartbeatWorktreeCatalogTest.cpp)
target_sources(ketplus_heartbeat_worktree_catalog_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_daemon_forward_only_upload_tests cloud-privacy/ForwardOnlyUploadTest.cpp)
target_sources(ketplus_daemon_forward_only_upload_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(heartbeat_snapshot_symbol _ZNK7ketplus6daemon18CloudControlClient17worktreeSnapshotsEv)
    add_executable(ketplus_heartbeat_worktree_catalog_benchmark EXCLUDE_FROM_ALL
        ../../benchmarks/HeartbeatWorktreeCatalogBenchmark.cpp
        ../../benchmarks/HeartbeatCatalogCallTiming.cpp
        cloud-privacy/MemoryCredentials.cpp)
    target_include_directories(ketplus_heartbeat_worktree_catalog_benchmark PRIVATE
        cloud-privacy)
    target_link_libraries(ketplus_heartbeat_worktree_catalog_benchmark PRIVATE ketplus_daemon_core)
    target_compile_definitions(ketplus_heartbeat_worktree_catalog_benchmark PRIVATE
        KETPLUS_SNAPSHOT_SYMBOL="${heartbeat_snapshot_symbol}")
    target_link_options(ketplus_heartbeat_worktree_catalog_benchmark PRIVATE
        "LINKER:--wrap=${heartbeat_snapshot_symbol}")
    ketplus_enable_warnings(ketplus_heartbeat_worktree_catalog_benchmark)
endif()

ketplus_add_daemon_test(ketplus_runner_relay_tests RunnerRelayTest.cpp)
target_sources(ketplus_runner_relay_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_runner_pairing_daemon_tests RunnerPairingDaemonTest.cpp)
target_sources(ketplus_runner_pairing_daemon_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_remote_job_relay_tests RemoteJobRelayTest.cpp)
target_sources(ketplus_remote_job_relay_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_remote_job_record_tests RemoteJobRecordTest.cpp)
ketplus_add_daemon_test(ketplus_remote_job_flow_tests RemoteJobFlowTest.cpp)
ketplus_add_daemon_test(ketplus_remote_job_store_tests RemoteJobStoreTest.cpp)
ketplus_add_daemon_test(ketplus_remote_job_adapters_tests RemoteJobAdaptersTest.cpp)
target_sources(ketplus_remote_job_adapters_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_remote_job_specs_tests RemoteJobSpecsTest.cpp)
ketplus_add_daemon_test(ketplus_remote_job_ledger_tests RemoteJobLedgerTest.cpp)
ketplus_add_daemon_test(ketplus_remote_job_steps_tests RemoteJobStepsTest.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_wiring_tests RemoteJobsWiringTest.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_restore_tests RemoteJobsRestoreTest.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_submit_tests RemoteJobsSubmitTest.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_relay_socket_tests RemoteJobsRelaySocketTest.cpp)
target_sources(ketplus_remote_jobs_relay_socket_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_service_tests RemoteJobsServiceTest.cpp)
target_sources(ketplus_remote_jobs_service_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
# Real remote job parts against a Relay and a runner the test plays.
ketplus_add_daemon_test(ketplus_remote_jobs_end_to_end_tests RemoteJobsEndToEndTest.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_stop_tests RemoteJobsStopTest.cpp)
ketplus_add_daemon_test(ketplus_remote_jobs_snapshot_tests RemoteJobsSnapshotTest.cpp)
ketplus_add_daemon_test(ketplus_remote_job_entry_tests RemoteJobEntryTest.cpp)
foreach(suite end_to_end stop snapshot)
    set(target ketplus_remote_jobs_${suite}_tests)
    target_sources(${target} PRIVATE RemoteJobsRunnerEnd.cpp RemoteJobsRunnerEnd.h)
    target_link_libraries(${target} PRIVATE
        ketplus_runner_pairing ketplus_control_crypto_primitives)
endforeach()
qt_add_executable(ketplus_peer_output_tests PeerOutputTest.cpp)
ketplus_enable_warnings(ketplus_peer_output_tests)
target_link_libraries(ketplus_peer_output_tests PRIVATE Qt6::Test ketplus_daemon_runner_peer)
add_test(NAME ketplus_peer_output_tests COMMAND ketplus_peer_output_tests)
# The Runner suite runs against the runner's own job session, which Windows does not build.
set(KETPLUS_PEER_SESSION_SUITES Link Job Output)
if(NOT WIN32)
    list(APPEND KETPLUS_PEER_SESSION_SUITES Runner)
endif()
foreach(suite IN LISTS KETPLUS_PEER_SESSION_SUITES)
    string(TOLOWER ${suite} name)
    set(target ketplus_peer_session_${name}_tests)
    qt_add_executable(${target}
        PeerSession${suite}Test.cpp PeerSessionFakes.cpp PeerSessionFakes.h)
    ketplus_enable_warnings(${target})
    target_link_libraries(${target} PRIVATE Qt6::Test ketplus_daemon_runner_peer)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
# Against the runner's real job session.
if(NOT WIN32)
    target_link_libraries(ketplus_peer_session_runner_tests PRIVATE ketplus_runner_session)
endif()
function(ketplus_add_runner_sync_test target source)
    qt_add_executable(${target} ${source})
    ketplus_enable_warnings(${target})
    target_link_libraries(${target} PRIVATE Qt6::Test ketplus_daemon_runner_sync)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${target} COMMAND ${target})
endfunction()
ketplus_add_runner_sync_test(ketplus_snapshot_keys_tests SnapshotKeysTest.cpp)
ketplus_add_runner_sync_test(ketplus_snapshot_preparation_tests SnapshotPreparationTest.cpp)
ketplus_add_runner_sync_test(ketplus_snapshot_retention_tests SnapshotRetentionTest.cpp)
ketplus_add_runner_sync_test(ketplus_snapshot_sender_tests SnapshotSenderTest.cpp)
ketplus_add_runner_sync_test(ketplus_snapshot_sync_integration_tests SnapshotSyncIntegrationTest.cpp)
target_sources(ketplus_snapshot_keys_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
qt_add_executable(ketplus_job_peer_clock_tests JobPeerClockTest.cpp)
ketplus_enable_warnings(ketplus_job_peer_clock_tests)
target_link_libraries(ketplus_job_peer_clock_tests PRIVATE Qt6::Test ketplus_daemon_runner_peer)
add_test(NAME ketplus_job_peer_clock_tests COMMAND ketplus_job_peer_clock_tests)
ketplus_add_daemon_test(ketplus_remote_output_spool_tests RemoteOutputSpoolTest.cpp)
target_link_libraries(ketplus_remote_output_spool_tests PRIVATE ketplus_daemon_runner_output)
# Signed builds may run PCRE2 without JIT; cloud projection must stay linear there too.
set_tests_properties(ketplus_daemon_cloud_privacy_tests PROPERTIES ENVIRONMENT
    "KETPLUS_DISABLE_DAEMON_AUTOSTART=1;KETPLUS_BROWSER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/ketplus_daemon_cloud_privacy_tests-browser-tools.json;KETPLUS_COMPUTER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/ketplus_daemon_cloud_privacy_tests-computer-tools.json;KETPLUS_COMPUTER_DIR=${CMAKE_CURRENT_BINARY_DIR}/ketplus_daemon_cloud_privacy_tests-computer;QT_ENABLE_REGEXP_JIT=0")
ketplus_add_daemon_test(ketplus_daemon_activity_groups_tests DaemonActivityGroupsTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_transcript_window_tests DaemonTranscriptWindowTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_workspace_files_tests DaemonWorkspaceFilesTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_lifecycle_tests DaemonLifecycleTest.cpp)
ketplus_add_daemon_test(ketplus_turn_acceptance_tests TurnAcceptanceTest.cpp)
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ketplus_add_daemon_test(ketplus_managed_task_host_tests ManagedTaskHostTest.cpp)
    ketplus_add_daemon_test(ketplus_managed_attention_recovery_tests ManagedTaskAttentionRecoveryTest.cpp)
    if(APPLE)
        add_dependencies(ketplus_managed_attention_recovery_tests ketplus_job_executor)
    endif()
    ketplus_add_daemon_test(ketplus_managed_task_ipc_tests ManagedTaskIpcTest.cpp)
    ketplus_add_daemon_test(ketplus_managed_task_origin_host_tests ManagedTaskOriginHostTest.cpp)
    add_dependencies(ketplus_managed_task_host_tests ketplus_job_executor)
    add_dependencies(ketplus_managed_task_ipc_tests ketplus_job_executor)
    add_dependencies(ketplus_managed_task_origin_host_tests ketplus_job_executor)
    ketplus_add_daemon_test(ketplus_managed_task_remote_host_tests ManagedTaskRemoteHostTest.cpp)
    add_dependencies(ketplus_managed_task_remote_host_tests ketplus_job_executor)
    # The session host joined to real remote jobs, a Relay and a runner the test plays.
    ketplus_add_daemon_test(ketplus_managed_task_remote_jobs_tests ManagedTaskRemoteJobsTest.cpp)
    target_sources(ketplus_managed_task_remote_jobs_tests PRIVATE
        ManagedTaskRemoteJoined.h RemoteJobsRunnerEnd.cpp RemoteJobsRunnerEnd.h)
    target_link_libraries(ketplus_managed_task_remote_jobs_tests PRIVATE
        ketplus_runner_pairing ketplus_control_crypto_primitives)
    add_dependencies(ketplus_managed_task_remote_jobs_tests ketplus_job_executor)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ketplus_add_daemon_test(ketplus_linux_job_executor_tests LinuxJobExecutorTest.cpp)
    target_compile_definitions(ketplus_linux_job_executor_tests PRIVATE
        KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>")
    add_dependencies(ketplus_linux_job_executor_tests ketplus_job_executor)
endif()
if(KETPLUS_BUILD_DESKTOP)
ketplus_add_daemon_test(ketplus_desktop_start_retention_tests DesktopStartRetentionTest.cpp)
target_link_libraries(ketplus_desktop_start_retention_tests PRIVATE ketplus_ai)

    ketplus_add_daemon_test(ketplus_browser_provider_integration_tests BrowserProviderIntegrationTest.cpp)
    target_link_libraries(ketplus_browser_provider_integration_tests PRIVATE ketplus_ai)
endif()
ketplus_add_daemon_test(ketplus_daemon_claude_adapter_tests ClaudeAdapterTest.cpp)
ketplus_add_daemon_test(ketplus_computer_tool_launch_tests ComputerToolLaunchTest.cpp)
qt_add_executable(ketplus_tool_locations_tests ToolLocationsTest.cpp)
ketplus_enable_warnings(ketplus_tool_locations_tests)
target_link_libraries(ketplus_tool_locations_tests PRIVATE Qt6::Test ketplus_tool_locations)
add_test(NAME ketplus_tool_locations_tests COMMAND ketplus_tool_locations_tests)
ketplus_add_daemon_test(ketplus_computer_provider_integration_tests ComputerProviderIntegrationTest.cpp)
ketplus_add_daemon_test(ketplus_provider_handoff_tests ProviderHandoffTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_copilot_adapter_tests CopilotAdapterTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_copilot_tools_tests CopilotToolsTest.cpp)
ketplus_add_daemon_test(ketplus_copilot_catalog_tests CopilotCatalogTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_acp_adapter_tests AcpAdapterTest.cpp)
ketplus_add_daemon_test(ketplus_codex_session_isolation_tests CodexSessionIsolationTest.cpp)
ketplus_add_daemon_test(ketplus_codex_executable_update_tests CodexExecutableUpdateTest.cpp)
ketplus_add_daemon_test(ketplus_codex_offline_refresh_tests CodexOfflineRefreshTest.cpp)
target_compile_definitions(ketplus_codex_offline_refresh_tests PRIVATE
    KETPLUS_CODEX_OFFLINE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../providers/codex/adapter-offline-fixture.py")
ketplus_add_daemon_test(ketplus_codex_bounded_refresh_tests CodexBoundedRefreshTest.cpp)
target_compile_definitions(ketplus_codex_bounded_refresh_tests PRIVATE
    KETPLUS_CODEX_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../providers/codex/adapter-live-fixture.py")
ketplus_add_daemon_test(ketplus_cursor_bounded_refresh_tests CursorBoundedRefreshTest.cpp)
target_compile_definitions(ketplus_cursor_bounded_refresh_tests PRIVATE
    KETPLUS_CODEX_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../providers/codex/adapter-live-fixture.py"
    KETPLUS_CURSOR_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../providers/cursor/adapter-bounded-fixture.py")
ketplus_add_daemon_test(ketplus_copilot_bounded_refresh_tests CopilotBoundedRefreshTest.cpp)
target_compile_definitions(ketplus_copilot_bounded_refresh_tests PRIVATE
    KETPLUS_CODEX_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../providers/codex/adapter-live-fixture.py"
    KETPLUS_ACP_LIVE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../providers/cursor/adapter-bounded-fixture.py")
ketplus_add_daemon_test(ketplus_daemon_cursor_adapter_tests CursorAdapterTest.cpp)
ketplus_add_daemon_test(ketplus_cursor_catalog_bounds_tests CursorCatalogBoundsTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_session_fork_tests SessionForkTest.cpp)
ketplus_add_daemon_test(ketplus_agent_messaging_tests AgentMessagingTest.cpp)
add_subdirectory(agent-background-tools)
add_subdirectory(acp-isolation)
ketplus_add_daemon_test(ketplus_agent_urgent_delivery_tests AgentUrgentDeliveryTest.cpp)
ketplus_add_daemon_test(ketplus_team_store_tests TeamStoreTest.cpp)
ketplus_add_daemon_test(ketplus_team_risk_policy_tests TeamRiskPolicyTest.cpp)
ketplus_add_daemon_test(ketplus_team_routing_tools_tests TeamRoutingToolsTest.cpp)
ketplus_add_daemon_test(ketplus_team_review_policy_tests TeamReviewPolicyTest.cpp)
ketplus_add_daemon_test(ketplus_team_finding_resolution_tests TeamFindingResolutionTest.cpp)
ketplus_add_daemon_test(ketplus_team_finding_routing_tests TeamFindingRoutingTest.cpp)
ketplus_add_daemon_test(ketplus_team_protocol_tests TeamProtocolTest.cpp)
# T-2: team.provider.set through the provider handoff, against the fake providers.
ketplus_add_daemon_test(ketplus_team_provider_switch_tests TeamProviderSwitchTest.cpp)
ketplus_add_daemon_test(ketplus_team_task_tests TeamTaskTest.cpp)
ketplus_add_daemon_test(ketplus_team_task_git_tests TeamTaskGitTest.cpp)
ketplus_add_daemon_test(ketplus_team_agent_tools_tests TeamAgentToolsTest.cpp)
# T-36: team_tasks rows, history pages and whole entries for agents.
ketplus_add_daemon_test(ketplus_team_agent_tasks_tests TeamAgentTasksTest.cpp)
ketplus_add_daemon_test(ketplus_team_flow_tests TeamFlowTest.cpp)
ketplus_add_daemon_test(ketplus_team_goal_tests TeamGoalTest.cpp)
ketplus_add_daemon_test(ketplus_team_title_tests TeamTitleTest.cpp)
ketplus_add_daemon_test(ketplus_team_reply_tests TeamReplyTest.cpp)
ketplus_add_daemon_test(ketplus_team_stall_tests TeamStallTest.cpp)
ketplus_add_daemon_test(ketplus_team_wait_tests TeamWaitTest.cpp)
ketplus_add_daemon_test(ketplus_team_digest_actor_tests TeamDigestActorTest.cpp)
ketplus_add_daemon_test(ketplus_team_review_tests TeamReviewTest.cpp)
ketplus_add_daemon_test(ketplus_team_review_place_tests TeamReviewPlaceTest.cpp)
ketplus_add_daemon_test(ketplus_team_verdict_tests TeamVerdictTest.cpp)
ketplus_add_daemon_test(ketplus_team_reviewers_tests TeamReviewersTest.cpp)
ketplus_add_daemon_test(ketplus_team_lane_tests TeamLaneTest.cpp)
ketplus_add_daemon_test(ketplus_team_payload_tests TeamPayloadTest.cpp)
ketplus_add_daemon_test(ketplus_team_decision_tests TeamDecisionTest.cpp)
# ADR 0114, open point 2: the global automatic-turn budget and each team's own.
ketplus_add_daemon_test(ketplus_team_budget_tests TeamBudgetTest.cpp)
# T-1: a fresh native context for turns a team starts, and the briefing it begins with.
ketplus_add_daemon_test(ketplus_team_context_reset_tests TeamContextResetTest.cpp)
# T-56: a team's language, its Updates, decisions and digests in it.
ketplus_add_daemon_test(ketplus_team_language_tests TeamLanguageTest.cpp)
ketplus_add_daemon_test(ketplus_team_merge_tests TeamMergeTest.cpp)
# ADR 0118: done by a merged pull request or by the leader.
ketplus_add_daemon_test(ketplus_team_done_tests TeamDoneTest.cpp)
# ADR 0114 §10: the disk warning and the removal of a done task's worktree.
ketplus_add_daemon_test(ketplus_team_storage_tests TeamStorageTest.cpp)
# T-90 (ADR 0123): the team's repository changes; a task works in a repository of its own.
ketplus_add_daemon_test(ketplus_team_repository_tests TeamRepositoryTest.cpp)
ketplus_add_daemon_test(ketplus_team_branch_name_tests TeamBranchNameTest.cpp)
# T-38: tracked changes only, allowed paths, and a head only from the task's own branch.
ketplus_add_daemon_test(ketplus_team_attribution_tests TeamAttributionTest.cpp)
# ADR 0116: team commands from paired phones, checked against the contract's fixture.
ketplus_add_daemon_test(ketplus_team_remote_tests TeamRemoteTest.cpp)
target_compile_definitions(ketplus_team_remote_tests PRIVATE
    KETPLUS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
ketplus_add_daemon_test(ketplus_team_remote_cloud_tests TeamRemoteCloudTest.cpp)
target_sources(ketplus_team_remote_cloud_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_compile_definitions(ketplus_team_remote_cloud_tests PRIVATE
    KETPLUS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
ketplus_add_daemon_test(ketplus_work_report_tests WorkReportTest.cpp)
ketplus_add_daemon_test(ketplus_awaiting_reply_tests AwaitingReplyTest.cpp)
ketplus_add_daemon_test(ketplus_tag_catalog_tests TagCatalogTest.cpp)
ketplus_add_daemon_test(ketplus_turn_review_tests TurnReviewTest.cpp)
ketplus_add_daemon_test(ketplus_provider_compatibility_tests ProviderCompatibilityTest.cpp)
ketplus_add_daemon_test(ketplus_provider_readiness_tests ProviderReadinessTest.cpp)
ketplus_add_daemon_test(ketplus_provider_response_id_tests ProviderResponseIdTest.cpp)
ketplus_add_daemon_test(ketplus_provider_version_probe_tests ProviderVersionProbeTest.cpp)
ketplus_add_daemon_test(ketplus_windows_script_launch_tests WindowsScriptLaunchTest.cpp)
ketplus_add_daemon_test(ketplus_cursor_invocation_tests CursorInvocationTest.cpp)
ketplus_add_daemon_test(ketplus_windows_platform_tests WindowsPlatformTest.cpp)
ketplus_add_daemon_test(ketplus_provider_framing_tests ProviderFramingTest.cpp)
ketplus_add_daemon_test(ketplus_provider_malformed_frame_tests ProviderMalformedFrameTest.cpp)
ketplus_add_daemon_test(ketplus_provider_stop_while_starting_tests ProviderStopWhileStartingTest.cpp)
ketplus_add_daemon_test(ketplus_provider_stderr_flood_tests ProviderStderrFloodTest.cpp)
ketplus_add_daemon_test(ketplus_provider_line_reader_tests ProviderLineReaderTest.cpp)
ketplus_add_daemon_test(ketplus_credential_batch_tests cloud-privacy/CredentialBatchTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_credential_store_tests CredentialStoreTest.cpp)
set(CREDENTIAL_STORAGE_LIBRARY ketplus_control_crypto)
# ketplus_control_crypto_primitives alone, then against CloudCrypto's existing API.
qt_add_executable(ketplus_control_crypto_primitives_tests ControlCryptoPrimitivesTest.cpp)
ketplus_enable_warnings(ketplus_control_crypto_primitives_tests)
target_link_libraries(ketplus_control_crypto_primitives_tests PRIVATE
    Qt6::Test ketplus_control_crypto_primitives)
add_test(NAME ketplus_control_crypto_primitives_tests COMMAND ketplus_control_crypto_primitives_tests)
qt_add_executable(ketplus_control_crypto_parity_tests ControlCryptoParityTest.cpp
    cloud-privacy/MemoryCredentials.cpp)
ketplus_enable_warnings(ketplus_control_crypto_parity_tests)
target_link_libraries(ketplus_control_crypto_parity_tests PRIVATE Qt6::Test ketplus_control_crypto)
add_test(NAME ketplus_control_crypto_parity_tests COMMAND ketplus_control_crypto_parity_tests)
include(credential-store/NativeTests.cmake)
ketplus_add_daemon_test(ketplus_daemon_worktree_revision_cache_tests WorktreeRevisionCacheTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_journal_space_tests JournalSpaceTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_journal_startup_tests JournalStartupTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_journal_migration_recovery_tests
    JournalMigrationRecoveryTest.cpp)
if(WIN32)
    # It migrates the same 60k-row journal 13 times and reads it back after each. In a Debug
    # build the SQLite driver of Qt's debug libraries is unoptimised (about 3.4x slower than
    # Release: 228 s against 38 s), so the default 240 s CTest limit and 300 s QtTest function
    # limit are too short there. The migration itself is not slower on NTFS.
    set_tests_properties(ketplus_daemon_journal_migration_recovery_tests PROPERTIES TIMEOUT 900
        ENVIRONMENT_MODIFICATION "QTEST_FUNCTION_TIMEOUT=set:900000")
endif()
ketplus_add_daemon_test(ketplus_daemon_schedule_storage_tests ScheduleStorageTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_schedule_host_tests ScheduleHostTest.cpp)
# Reads SQLite's own statement counters through the driver's handle; needs the same SQLite.
find_package(SQLite3)
if(TARGET SQLite::SQLite3)
    ketplus_add_daemon_test(ketplus_daemon_journal_query_audit_tests JournalQueryAuditTest.cpp)
    target_link_libraries(ketplus_daemon_journal_query_audit_tests PRIVATE SQLite::SQLite3)
endif()
ketplus_add_daemon_test(ketplus_daemon_machine_challenge_tests MachineChallengeTest.cpp)
target_sources(ketplus_daemon_machine_challenge_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_compile_definitions(ketplus_daemon_machine_challenge_tests PRIVATE
    KETPLUS_CHALLENGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/daemon-challenge-v2.json")
ketplus_add_daemon_test(ketplus_daemon_usage_retention_tests JournalUsageRetentionTest.cpp)
if(WIN32) # Passes in about 6 minutes there (SQLite commits are slow on NTFS).
    set_tests_properties(ketplus_daemon_usage_retention_tests PROPERTIES TIMEOUT 900)
endif()
ketplus_add_daemon_test(ketplus_daemon_garbage_collection_tests JournalGarbageCollectionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_metadata_retention_tests JournalMetadataRetentionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_cloud_cache_retention_tests JournalCloudCacheRetentionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_attachment_retention_tests StagedAttachmentRetentionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_ack_retention_tests JournalAcknowledgementRetentionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_retention_policy_tests JournalRetentionPolicyTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_size_policy_tests JournalSizePolicyTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_storage_jobs_tests JournalStorageJobsTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_detail_compactor_tests ReplayDetailCompactorTest.cpp)
target_include_directories(ketplus_daemon_detail_compactor_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/modules/ai/src")
ketplus_add_daemon_test(ketplus_claude_account_usage_tests ClaudeAccountUsageTest.cpp)
ketplus_add_daemon_test(ketplus_claude_credentials_tests ClaudeCredentialsTest.cpp)
target_link_libraries(ketplus_claude_credentials_tests PRIVATE ketplus_remote)
ketplus_add_daemon_test(ketplus_daemon_relay_wakeups_tests RelayWakeupsTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_browser_sign_in_tests BrowserSignInTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_upgrade_tests DaemonUpgradeTest.cpp)

if(KETPLUS_BUILD_DESKTOP)
ketplus_add_daemon_test(ketplus_desktop_activity_groups_tests DesktopActivityGroupsTest.cpp)
target_include_directories(ketplus_desktop_activity_groups_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(ketplus_desktop_activity_groups_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_daemon_client_tests DesktopDaemonClientTest.cpp)
target_link_libraries(ketplus_desktop_daemon_client_tests PRIVATE ketplus_ai)
add_dependencies(ketplus_desktop_daemon_client_tests ketplus_daemon)

ketplus_add_daemon_test(ketplus_desktop_transcript_window_tests DesktopTranscriptWindowTest.cpp)
target_link_libraries(ketplus_desktop_transcript_window_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_internal_steps_tests DesktopTranscriptInternalStepsTest.cpp)
target_link_libraries(ketplus_desktop_transcript_internal_steps_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_conversation_attention_daemon_tests ConversationAttentionDaemonTest.cpp)
target_link_libraries(ketplus_conversation_attention_daemon_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_large_event_fetch_tests DesktopTranscriptLargeEventFetchTest.cpp)
target_link_libraries(ketplus_desktop_transcript_large_event_fetch_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_navigation_tests DesktopTranscriptNavigationTest.cpp)
target_link_libraries(ketplus_desktop_transcript_navigation_tests PRIVATE ketplus_ai)
ketplus_add_daemon_test(ketplus_desktop_session_discovery_tests DesktopSessionDiscoveryTest.cpp)
target_sources(ketplus_desktop_session_discovery_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_desktop_session_discovery_tests PRIVATE ketplus_ai)
ketplus_add_daemon_test(ketplus_desktop_reviewer_tests DesktopReviewerTest.cpp)
target_link_libraries(ketplus_desktop_reviewer_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_catchup_tests DesktopTranscriptCatchupTest.cpp)
target_link_libraries(ketplus_desktop_transcript_catchup_tests PRIVATE ketplus_ai)
endif()

find_program(KETPLUS_SIDECAR_TEST_NODE node)
if(KETPLUS_SIDECAR_TEST_NODE)
    set(KETPLUS_SIDECAR_DAEMON_TESTS "${PROJECT_SOURCE_DIR}/apps/daemon/providers/claude-sidecar/test")
    set(KETPLUS_SIDECAR_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar")
    set(KETPLUS_SIDECAR_PORTABLE_FILES
        "${KETPLUS_SIDECAR_DAEMON_TESTS}/authentication.test.mjs"
        "${KETPLUS_SIDECAR_DAEMON_TESTS}/input.test.mjs"
        "${KETPLUS_SIDECAR_DAEMON_TESTS}/process-barrier.test.mjs"
        "${KETPLUS_SIDECAR_DAEMON_TESTS}/session-delete.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/session-usage.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/managed-transport.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/background-sdk-offline.test.mjs")
    # These hang or fail on Windows under `node --test` (they spawn POSIX helpers and assume
    # POSIX paths and shells), so there they form a separate, disabled test.
    set(KETPLUS_SIDECAR_POSIX_FILES
        "${KETPLUS_SIDECAR_TESTS}/transcript.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/permissions.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/agent-tools.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/usage.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/compact-window.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/steer.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/fork.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/background-tasks.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/managed-background.test.mjs"
        "${KETPLUS_SIDECAR_TESTS}/model-catalog.test.mjs"
        "${KETPLUS_SIDECAR_DAEMON_TESTS}/review-guard.test.mjs"
        "${KETPLUS_SIDECAR_DAEMON_TESTS}/review-profile.test.mjs")
    if(WIN32)
        add_test(NAME ketplus_claude_sidecar_tests COMMAND "${KETPLUS_SIDECAR_TEST_NODE}" --test
            ${KETPLUS_SIDECAR_PORTABLE_FILES})
        add_test(NAME ketplus_claude_sidecar_posix_tests COMMAND "${KETPLUS_SIDECAR_TEST_NODE}" --test
            ${KETPLUS_SIDECAR_POSIX_FILES})
        set_tests_properties(ketplus_claude_sidecar_tests PROPERTIES LABELS "windows-script-fixture")
        set_tests_properties(ketplus_claude_sidecar_posix_tests PROPERTIES DISABLED TRUE
            LABELS "windows-script-fixture")
    else()
        add_test(NAME ketplus_claude_sidecar_tests COMMAND "${KETPLUS_SIDECAR_TEST_NODE}" --test
            ${KETPLUS_SIDECAR_PORTABLE_FILES} ${KETPLUS_SIDECAR_POSIX_FILES})
    endif()
    add_test(NAME ketplus_codex_contract_tests COMMAND "${KETPLUS_SIDECAR_TEST_NODE}" --test
        "${CMAKE_CURRENT_SOURCE_DIR}/../providers/codex/contract.test.mjs")
endif()
if(KETPLUS_BUILD_DESKTOP)
    ketplus_add_daemon_test(ketplus_provider_transcript_tests ProviderTranscriptTest.cpp)
    target_link_libraries(ketplus_provider_transcript_tests PRIVATE ketplus_ai)
    ketplus_add_daemon_test(ketplus_provider_setup_guide_tests ProviderSetupGuideTest.cpp)
    target_link_libraries(ketplus_provider_setup_guide_tests PRIVATE ketplus_ai)
endif()

qt_add_executable(ketplus_atlas_tunnel_fixture AtlasTunnelFixture.cpp)
target_link_libraries(ketplus_atlas_tunnel_fixture PRIVATE ketplus_daemon_core)
ketplus_enable_warnings(ketplus_atlas_tunnel_fixture)
ketplus_add_daemon_test(ketplus_codex_items_tests CodexItemsTest.cpp)

ketplus_add_daemon_test(ketplus_daemon_peer_transport_tests PeerTransportTest.cpp)
target_sources(ketplus_daemon_peer_transport_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
set_tests_properties(ketplus_daemon_peer_transport_tests PROPERTIES TIMEOUT 45)
ketplus_add_daemon_test(ketplus_daemon_relay_peer_link_tests RelayPeerLinkTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_push_signal_tests PushSignalsTest.cpp)
# ketplus_peer_link alone, without the session host: offerer against answerer over loopback.
qt_add_executable(ketplus_peer_link_tests PeerLinkTest.cpp)
ketplus_enable_warnings(ketplus_peer_link_tests)
target_link_libraries(ketplus_peer_link_tests PRIVATE Qt6::Test ketplus_peer_link)
add_test(NAME ketplus_peer_link_tests COMMAND ketplus_peer_link_tests)
set_tests_properties(ketplus_peer_link_tests PROPERTIES TIMEOUT 90)
# The desktop's runner peer links: fakes for every port, then the runner's connector against
# them over real links on this machine.
# The runner's connector lives in apps/runner, which Windows does not build, so these run on
# Linux and macOS only.
if(NOT WIN32)
    qt_add_executable(ketplus_daemon_runner_link_tests
        runner-link/RunnerLinkTest.cpp runner-link/RunnerLinkTest.h
        runner-link/RunnerLinkOfferTest.cpp runner-link/RunnerLinkLifecycleTest.cpp
        runner-link/RunnerLinkChannelsTest.cpp runner-link/RunnerLinkLoopbackTest.cpp)
    ketplus_enable_warnings(ketplus_daemon_runner_link_tests)
    target_link_libraries(ketplus_daemon_runner_link_tests PRIVATE
        Qt6::Test ketplus_daemon_runner_link ketplus_runner_peer ketplus_runner_pairing
        ketplus_control_crypto_primitives)
    add_test(NAME ketplus_daemon_runner_link_tests COMMAND ketplus_daemon_runner_link_tests)
    set_tests_properties(ketplus_daemon_runner_link_tests PROPERTIES TIMEOUT 180)
endif()

ketplus_add_daemon_test(ketplus_async_question_lifetime_tests AsyncQuestionLifetimeTest.cpp)

ketplus_tests_offscreen()
ketplus_label_tests(daemon)
# #468: the daemon's team texts, read by the desktop's transcript blocks.
ketplus_add_daemon_test(ketplus_team_transcript_contract_tests TeamTranscriptContractTest.cpp)
target_sources(ketplus_team_transcript_contract_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/ai/src/agent/TeamTranscriptBlocks.cpp)
target_include_directories(ketplus_team_transcript_contract_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/ai/src)

ketplus_add_daemon_test(ketplus_relay_attachment_host_tests RelayAttachmentHostTest.cpp)
target_sources(ketplus_relay_attachment_host_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)

ketplus_add_daemon_test(ketplus_session_preparation_store_tests SessionPreparationStoreTest.cpp)
ketplus_add_daemon_test(ketplus_session_preparation_host_tests SessionPreparationHostTest.cpp)
ketplus_add_daemon_test(ketplus_relay_preparation_host_tests RelayPreparationHostTest.cpp)
target_sources(ketplus_relay_preparation_host_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
add_subdirectory(background-output)
