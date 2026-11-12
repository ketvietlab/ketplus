if(APPLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ketplus_macos_signing_cli_tests
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/packaging/test_signing_cli.py")
endif()

function(ketplus_add_gui_test target)
    qt_add_executable(${target} ${ARGN})
    ketplus_enable_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;KETPLUS_DISABLE_DAEMON_AUTOSTART=1;KETPLUS_BROWSER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/${target}-browser-tools.json;KETPLUS_COMPUTER_TOOLS_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/${target}-computer-tools.json;KETPLUS_COMPUTER_DIR=${CMAKE_CURRENT_BINARY_DIR}/${target}-computer")
endfunction()

ketplus_add_gui_test(ketplus_codex_browser_tools_tests CodexBrowserToolsTest.cpp)
target_link_libraries(ketplus_codex_browser_tools_tests PRIVATE Qt6::Test ketplus_browser_tools)

ketplus_add_gui_test(ketplus_bundled_browser_runtime_tests BundledBrowserRuntimeTest.cpp)
target_link_libraries(ketplus_bundled_browser_runtime_tests PRIVATE Qt6::Test ketplus_browser_tools)

ketplus_add_gui_test(ketplus_spotlight_tests SpotlightTest.cpp)
ketplus_add_gui_test(ketplus_repository_metadata_tests RepositoryMetadataTest.cpp)
target_link_libraries(ketplus_repository_metadata_tests PRIVATE Qt6::Test ketplus_desktop_support)

ketplus_add_gui_test(ketplus_conversation_worktree_tests ConversationWorktreeStoreTest.cpp)
target_link_libraries(ketplus_conversation_worktree_tests PRIVATE Qt6::Test ketplus_ai)
target_link_libraries(ketplus_spotlight_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)


ketplus_add_gui_test(ketplus_atlas_tests AtlasTest.cpp)
target_link_libraries(ketplus_atlas_tests PRIVATE Qt6::Test ketplus_atlas)

ketplus_add_gui_test(ketplus_atlas_workspace_tests AtlasWorkspaceTest.cpp)
target_link_libraries(ketplus_atlas_workspace_tests PRIVATE
    Qt6::Test
    ketplus_desktop_shell
)

ketplus_add_gui_test(ketplus_desktop_zoom_tests DesktopZoomTest.cpp)
target_link_libraries(ketplus_desktop_zoom_tests PRIVATE Qt6::Test ketplus_desktop_shell)
ketplus_add_gui_test(ketplus_settings_storage_page_tests SettingsStoragePageTest.cpp)
target_link_libraries(ketplus_settings_storage_page_tests PRIVATE Qt6::Test ketplus_desktop_shell)
ketplus_add_gui_test(ketplus_settings_teams_page_tests SettingsTeamsPageTest.cpp)
target_link_libraries(ketplus_settings_teams_page_tests PRIVATE Qt6::Test ketplus_desktop_shell)
ketplus_add_gui_test(ketplus_settings_computer_page_tests SettingsComputerPageTest.cpp)
target_link_libraries(ketplus_settings_computer_page_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_interface_font_tests InterfaceFontTest.cpp)
target_link_libraries(ketplus_interface_font_tests PRIVATE
    Qt6::Test
    ketplus_ai
    ketplus_typography
)

ketplus_add_gui_test(ketplus_agent_tests
    AgentWorkspaceHistoryTest.cpp
    TranscriptInteractionTest.cpp
    TranscriptFileStepsTest.cpp
    TranscriptAgentNoteTest.cpp
    TranscriptTeamBlockTest.cpp
    TranscriptWorkReportTest.cpp
    TranscriptProviderErrorTest.cpp
    AgentProviderSetupTest.cpp
    TranscriptTrimmedDetailTest.cpp
    AgentWorkspaceInteractionTest.cpp
    AgentUsageCostDetailsTest.cpp
    AgentWorkspaceAtlasResultTest.cpp
    AgentWorkspaceRenderingTest.cpp
    AgentWorkspaceMotionTest.cpp
    AgentCompactWindowTest.cpp
    AgentWorkspaceScrollTest.cpp
    AgentWorkspaceSessionTest.cpp
    AgentWorkspaceMissingWorktreeTest.cpp
    AgentConversationForkMenuTest.cpp
    AgentReviewerPickerTest.cpp
    AgentTeamHeaderTest.cpp
    AgentWorkspaceViewTest.cpp
    AgentWorkspaceViewTest.h
    TranscriptPhase5Test.cpp
)
target_link_libraries(ketplus_agent_tests PRIVATE
    Qt6::Quick
    Qt6::Test
    ketplus_ai
    KetPlusCM::ui
)

ketplus_add_gui_test(ketplus_agent_shell_tests AgentWorkspaceShellTest.cpp)
target_link_libraries(ketplus_agent_shell_tests PRIVATE
    Qt6::Test
    ketplus_desktop_shell
)

ketplus_add_gui_test(ketplus_agent_streaming_tests AgentWorkspaceStreamingTest.cpp)
target_link_libraries(ketplus_agent_streaming_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_transcript_window_budget_tests AgentTranscriptWindowBudgetTest.cpp)
target_link_libraries(ketplus_transcript_window_budget_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_transcript_row_measurement_tests TranscriptRowMeasurementTest.cpp)
target_link_libraries(ketplus_transcript_row_measurement_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_transcript_row_geometry_tests TranscriptRowGeometryTest.cpp)
ketplus_add_gui_test(ketplus_transcript_window_model_tests TranscriptWindowModelTest.cpp)
target_link_libraries(ketplus_transcript_row_geometry_tests PRIVATE Qt6::Test ketplus_ai)
target_link_libraries(ketplus_transcript_window_model_tests PRIVATE Qt6::Test Qt6::Gui ketplus_ai)

ketplus_add_gui_test(ketplus_transcript_activity_groups_tests TranscriptActivityGroupsTest.cpp)
target_link_libraries(ketplus_transcript_activity_groups_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_transcript_safety_tests TranscriptSafetyTest.cpp)
target_link_libraries(ketplus_transcript_safety_tests PRIVATE Qt6::Test Qt6::Network Qt6::Quick ketplus_ai)

ketplus_add_gui_test(ketplus_symbol_icon_tests SymbolIconTest.cpp)
target_link_libraries(ketplus_symbol_icon_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_agent_viewport_tests AgentTranscriptViewportTest.cpp)
target_link_libraries(ketplus_agent_viewport_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_claude_credentials_settings_tests ClaudeCredentialsSettingsTest.cpp)
target_link_libraries(ketplus_claude_credentials_settings_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_working_line_tests WorkingLineTest.cpp)
target_link_libraries(ketplus_working_line_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_conversation_menu_tests ConversationMenuTest.cpp)
target_link_libraries(ketplus_conversation_menu_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_sidebar_teams_tests SidebarTeamsTest.cpp)
target_link_libraries(ketplus_sidebar_teams_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_team_dialogs_tests TeamDialogsTest.cpp)
target_link_libraries(ketplus_team_dialogs_tests PRIVATE Qt6::Test ketplus_desktop_shell)
ketplus_add_gui_test(ketplus_team_page_tests TeamPageTest.cpp)
ketplus_add_gui_test(ketplus_team_goal_form_tests TeamGoalFormTest.cpp)
target_link_libraries(ketplus_team_goal_form_tests PRIVATE Qt6::Test ketplus_desktop_shell)
ketplus_add_gui_test(ketplus_team_commands_tests TeamCommandsTest.cpp)
target_link_libraries(ketplus_team_page_tests PRIVATE Qt6::Test ketplus_desktop_shell)
target_link_libraries(ketplus_team_commands_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_settings_integration_tests MainWindowSettingsTest.cpp)
target_link_libraries(ketplus_settings_integration_tests PRIVATE
    Qt6::Test
    ketplus_desktop_shell
)

ketplus_add_gui_test(ketplus_editor_features_tests MainWindowEditorFeaturesTest.cpp)
ketplus_add_gui_test(ketplus_transcript_file_opening_tests TranscriptFileOpeningTest.cpp)
target_link_libraries(ketplus_transcript_file_opening_tests PRIVATE Qt6::Test ketplus_desktop_shell)
target_link_libraries(ketplus_editor_features_tests PRIVATE
    Qt6::Test
    ketplus_desktop_shell
)

ketplus_add_gui_test(ketplus_rate_limits_tests CodexRateLimitsTest.cpp)
target_link_libraries(ketplus_rate_limits_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_chat_markdown_tests ChatMarkdownTest.cpp)
target_link_libraries(ketplus_chat_markdown_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_add_gui_test(ketplus_chat_mermaid_tests ChatMermaidTest.cpp)
target_link_libraries(ketplus_chat_mermaid_tests PRIVATE
    Qt6::Test
    ketplus_ai
    KetPlusCM::preview
)

ketplus_add_gui_test(ketplus_workspace_tests ExplorerPanelTest.cpp)
target_link_libraries(ketplus_workspace_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
    KetPlusCM::workspace
)

ketplus_add_gui_test(ketplus_conversation_attention_tests ConversationAttentionTest.cpp)
target_link_libraries(ketplus_conversation_attention_tests PRIVATE
    Qt6::Test ketplus_ai ketplus_desktop_support
)

ketplus_add_gui_test(ketplus_remote_workspace_tests RemoteControlViewTest.cpp)
target_include_directories(ketplus_remote_workspace_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ketplus_remote_workspace_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

ketplus_add_gui_test(ketplus_machine_composition_tests MachineCompositionTest.cpp
    FakeMachineBackend.h MachineHosts.h)
target_link_libraries(ketplus_machine_composition_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

ketplus_add_gui_test(ketplus_machine_conversation_tests MachineConversationTest.cpp
    FakeMachineBackend.h MachineHosts.h)
target_link_libraries(ketplus_machine_conversation_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

ketplus_add_gui_test(ketplus_machine_paging_tests MachineTranscriptPagingTest.cpp
    FakeMachineBackend.h MachineHosts.h)
target_link_libraries(ketplus_machine_paging_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

ketplus_add_gui_test(ketplus_machine_window_tests MachineWindowTest.cpp FakeMachineBackend.h)
target_include_directories(ketplus_machine_window_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ketplus_machine_window_tests PRIVATE
    Qt6::Test
    ketplus_desktop_shell
)

# #419 end to end: the window over a real daemon behind a loopback sshd, and over the
# loopback relay (tests/daemon/SyntheticRelay.h). Needs sshd for the SSH case.
ketplus_add_gui_test(ketplus_machine_end_to_end_tests MachineEndToEndTest.cpp)
target_include_directories(ketplus_machine_end_to_end_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon)
# The Relay case keeps credentials in memory, never in the login keychain.
target_sources(ketplus_machine_end_to_end_tests PRIVATE daemon/cloud-privacy/MemoryCredentials.cpp)
target_link_libraries(ketplus_machine_end_to_end_tests PRIVATE
    Qt6::Test
    ketplus_desktop_shell
    ketplus_remote_ssh_backend
    ketplus_remote_control
)
target_compile_definitions(ketplus_machine_end_to_end_tests PRIVATE
    KETPLUS_TEST_DAEMON="$<TARGET_FILE:ketplus_daemon>")
add_dependencies(ketplus_machine_end_to_end_tests ketplus_daemon)

ketplus_add_gui_test(ketplus_linux_machines_tests LinuxMachinesSectionTest.cpp)
target_include_directories(ketplus_linux_machines_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ketplus_linux_machines_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

# Connections' SSH route against SshMachineBackend with a fake daemon behind a fake link.
ketplus_add_gui_test(ketplus_linux_ssh_machines_tests LinuxSshMachinesTest.cpp)
target_include_directories(ketplus_linux_ssh_machines_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ketplus_linux_ssh_machines_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

# Connections' Relay route against fake Relay links: state, entitlement, presence, pending.
ketplus_add_gui_test(ketplus_linux_relay_machines_tests LinuxRelayMachinesTest.cpp)
target_link_libraries(ketplus_linux_relay_machines_tests PRIVATE
    Qt6::Test
    ketplus_desktop_support
)

add_subdirectory(daemon)

if(APPLE)
    add_test(
        NAME ketplus_macos_bundle_layout_tests
        COMMAND "${CMAKE_COMMAND}"
            "-DKETPLUS_APP_BUNDLE=$<TARGET_BUNDLE_DIR:ketplus>"
            "-DKETPLUS_PRODUCT_VERSION=${PROJECT_VERSION}"
            "-DKETPLUS_CONTROL_URL=${KETPLUS_CONTROL_URL}"
            -P "${PROJECT_SOURCE_DIR}/packaging/macos/VerifyBundle.cmake"
    )
endif()

ketplus_add_gui_test(ketplus_execution_worktree_ui_tests ExecutionWorktreeDialogTest.cpp)
target_link_libraries(ketplus_execution_worktree_ui_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_desktop_document_modes_tests DesktopDocumentModesTest.cpp)
target_link_libraries(ketplus_desktop_document_modes_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_worktree_picker_tests WorktreePickerTest.cpp)
target_link_libraries(ketplus_worktree_picker_tests PRIVATE Qt6::Test ketplus_desktop_shell)

ketplus_add_gui_test(ketplus_activity_group_loader_tests ActivityGroupLoaderTest.cpp)
target_link_libraries(ketplus_activity_group_loader_tests PRIVATE Qt6::Test ketplus_ai)

ketplus_tests_offscreen()
