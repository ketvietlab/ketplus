function(ketplus_add_daemon_test target source)
    qt_add_executable(${target} ${source})
    ketplus_enable_warnings(${target})
    target_link_libraries(${target} PRIVATE Qt6::Test ketplus_daemon_core)
    add_test(NAME ${target} COMMAND ${target})
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

ketplus_add_daemon_test(ketplus_headless_daemon_tests HeadlessDaemonTest.cpp)
target_compile_definitions(ketplus_headless_daemon_tests PRIVATE
    KETPLUS_TEST_DAEMON="$<TARGET_FILE:ketplus_daemon>" KETPLUS_TEST_CTL="$<TARGET_FILE:ketplusctl>")
add_dependencies(ketplus_headless_daemon_tests ketplus_daemon ketplusctl)
ketplus_add_daemon_test(ketplus_device_authorization_tests DeviceAuthorizationTest.cpp)
target_sources(ketplus_device_authorization_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_remote_control_client_tests RemoteControlClientTest.cpp)
target_sources(ketplus_remote_control_client_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_remote_control_client_tests PRIVATE ketplus_remote_control)

ketplus_add_daemon_test(ketplus_atlas_live_runtime_tests AtlasLiveRuntimeTest.cpp)
ketplus_add_daemon_test(ketplus_atlas_live_tunnel_tests AtlasLiveTunnelTest.cpp)
ketplus_add_daemon_test(ketplus_provider_catalog_cache_tests ProviderCatalogCacheTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_protocol_tests DaemonProtocolTest.cpp)
ketplus_add_daemon_test(ketplus_context_harness_tests ContextHarnessTest.cpp)
ketplus_add_daemon_test(ketplus_provider_deletion_tests ProviderDeletionTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_worktree_context_tests DaemonWorktreeContextTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_claude_worktree_tests ClaudeWorktreeTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_context_relay_tests WorktreeContextRelayTest.cpp)
# Link in-memory credential symbols before the static core; never use the OS key store.
target_sources(ketplus_daemon_context_relay_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
ketplus_add_daemon_test(ketplus_daemon_cloud_privacy_tests cloud-privacy/CloudPrivacyTest.cpp)
target_sources(ketplus_daemon_cloud_privacy_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
# Signed builds may run PCRE2 without JIT; cloud projection must stay linear there too.
set_tests_properties(ketplus_daemon_cloud_privacy_tests PROPERTIES ENVIRONMENT
    "KETPLUS_DISABLE_DAEMON_AUTOSTART=1;KETPLUS_BROWSER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/ketplus_daemon_cloud_privacy_tests-browser-tools.json;KETPLUS_COMPUTER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/ketplus_daemon_cloud_privacy_tests-computer-tools.json;KETPLUS_COMPUTER_DIR=${CMAKE_CURRENT_BINARY_DIR}/ketplus_daemon_cloud_privacy_tests-computer;QT_ENABLE_REGEXP_JIT=0")
ketplus_add_daemon_test(ketplus_daemon_activity_groups_tests DaemonActivityGroupsTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_transcript_window_tests DaemonTranscriptWindowTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_workspace_files_tests DaemonWorkspaceFilesTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_lifecycle_tests DaemonLifecycleTest.cpp)
if(KETPLUS_BUILD_DESKTOP)
    ketplus_add_daemon_test(ketplus_browser_provider_integration_tests BrowserProviderIntegrationTest.cpp)
    target_link_libraries(ketplus_browser_provider_integration_tests PRIVATE ketplus_ai)
endif()
ketplus_add_daemon_test(ketplus_daemon_claude_adapter_tests ClaudeAdapterTest.cpp)
ketplus_add_daemon_test(ketplus_computer_tool_launch_tests ComputerToolLaunchTest.cpp)
ketplus_add_daemon_test(ketplus_computer_provider_integration_tests ComputerProviderIntegrationTest.cpp)
ketplus_add_daemon_test(ketplus_provider_handoff_tests ProviderHandoffTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_cursor_adapter_tests CursorAdapterTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_session_fork_tests SessionForkTest.cpp)
ketplus_add_daemon_test(ketplus_agent_messaging_tests AgentMessagingTest.cpp)
ketplus_add_daemon_test(ketplus_provider_compatibility_tests ProviderCompatibilityTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_credential_store_tests CredentialStoreTest.cpp)
ketplus_add_daemon_test(ketplus_daemon_journal_space_tests JournalSpaceTest.cpp)
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

ketplus_add_daemon_test(ketplus_conversation_attention_daemon_tests ConversationAttentionDaemonTest.cpp)
target_link_libraries(ketplus_conversation_attention_daemon_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_large_event_fetch_tests DesktopTranscriptLargeEventFetchTest.cpp)
target_link_libraries(ketplus_desktop_transcript_large_event_fetch_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_navigation_tests DesktopTranscriptNavigationTest.cpp)
target_link_libraries(ketplus_desktop_transcript_navigation_tests PRIVATE ketplus_ai)
ketplus_add_daemon_test(ketplus_desktop_session_discovery_tests DesktopSessionDiscoveryTest.cpp)
target_link_libraries(ketplus_desktop_session_discovery_tests PRIVATE ketplus_ai)

ketplus_add_daemon_test(ketplus_desktop_transcript_catchup_tests DesktopTranscriptCatchupTest.cpp)
target_link_libraries(ketplus_desktop_transcript_catchup_tests PRIVATE ketplus_ai)
endif()

find_program(KETPLUS_SIDECAR_TEST_NODE node)
if(KETPLUS_SIDECAR_TEST_NODE)
    add_test(NAME ketplus_claude_sidecar_tests COMMAND "${KETPLUS_SIDECAR_TEST_NODE}" --test
        "${PROJECT_SOURCE_DIR}/apps/daemon/providers/claude-sidecar/test/authentication.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/transcript.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/permissions.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/agent-tools.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/usage.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/session-usage.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/compact-window.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/steer.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/fork.test.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/claude-sidecar/background-tasks.test.mjs"
        "${PROJECT_SOURCE_DIR}/apps/daemon/providers/claude-sidecar/test/session-delete.test.mjs")
    add_test(NAME ketplus_codex_contract_tests COMMAND "${KETPLUS_SIDECAR_TEST_NODE}" --test
        "${CMAKE_CURRENT_SOURCE_DIR}/../providers/codex/contract.test.mjs")
endif()
if(KETPLUS_BUILD_DESKTOP)
    ketplus_add_daemon_test(ketplus_provider_transcript_tests ProviderTranscriptTest.cpp)
    target_link_libraries(ketplus_provider_transcript_tests PRIVATE ketplus_ai)
endif()

qt_add_executable(ketplus_atlas_tunnel_fixture AtlasTunnelFixture.cpp)
target_link_libraries(ketplus_atlas_tunnel_fixture PRIVATE ketplus_daemon_core)
ketplus_add_daemon_test(ketplus_codex_items_tests CodexItemsTest.cpp)

ketplus_add_daemon_test(ketplus_daemon_peer_transport_tests PeerTransportTest.cpp)
target_sources(ketplus_daemon_peer_transport_tests PRIVATE cloud-privacy/MemoryCredentials.cpp)
set_tests_properties(ketplus_daemon_peer_transport_tests PROPERTIES TIMEOUT 45)

ketplus_add_daemon_test(ketplus_async_question_lifetime_tests AsyncQuestionLifetimeTest.cpp)

ketplus_tests_offscreen()
