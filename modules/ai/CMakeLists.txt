add_library(ketplus_ai STATIC
    src/agent/AgentMessageFormatter.cpp
    src/agent/AgentMessageFormatter.h
    src/agent/TranscriptStepItems.cpp
    src/agent/TranscriptTurnFiles.cpp
    src/agent/TranscriptTurnFiles.h
    src/agent/ConversationChangesPane.cpp
    src/agent/ConversationChangesPane.h
    src/agent/WorkReportPane.cpp
    src/agent/WorkReportPane.h
    src/agent/WorkReportText.cpp
    src/agent/WorkReportText.h
    src/agent/TranscriptStepItems.h
    src/agent/AgentMessageModel.cpp
    src/agent/AgentMessageModel.h
    src/agent/McpToolResultMedia.cpp
    src/agent/McpToolResultMedia.h
    src/agent/TranscriptEvent.cpp
    src/agent/TranscriptEvent.h
    src/agent/ActivityGroupLoader.cpp
    src/agent/ActivityGroupLoader.h
    src/agent/AgentSessionActivityGroups.cpp
    src/agent/CodexDaemonActivityGroups.cpp
    src/agent/TranscriptActivityGroups.cpp
    src/agent/TranscriptActivityGroups.h
    src/agent/TranscriptFileLocation.cpp
    src/agent/TranscriptFileLocation.h
    src/agent/TranscriptNormalizer.cpp
    src/agent/TranscriptNormalizer.h
    src/agent/TranscriptSanitizer.h
    src/agent/TranscriptPresentationGrouper.cpp
    src/agent/TranscriptPresentationGrouper.h
    src/agent/AgentSessionHistory.cpp
    src/agent/AgentSessionDaemonHistory.cpp
    src/agent/AgentSessionDaemonCatchup.cpp
    src/agent/DaemonAgentNotes.cpp
    src/agent/DaemonAgentNotes.h
    src/agent/DaemonTranscriptNotices.cpp
    src/agent/DaemonTranscriptNotices.h
    src/agent/DaemonTranscriptProjection.cpp
    src/agent/DaemonTranscriptProjection.h
    src/agent/TeamTranscriptBlocks.cpp
    src/agent/TeamTranscriptBlocks.h
    src/agent/AgentSessionManager.cpp
    src/agent/AgentSessionArchive.cpp
    src/agent/AgentSessionReviewers.cpp
    src/agent/AgentSessionWorkReports.cpp
    src/agent/AgentSessionConfiguration.cpp
    src/agent/AgentSessionManager.h
    src/agent/AgentSessionNotifications.cpp
    src/agent/ReasoningHeading.cpp
    src/agent/ReasoningHeading.h
    src/agent/AgentSessionState.cpp
    src/agent/AgentSessionState.h
    src/agent/AgentSessionTurns.cpp
    src/agent/AgentSessionWorktrees.cpp
    src/agent/AgentSessionFork.cpp
    src/agent/ConversationWorktreeStore.cpp
    src/agent/ConversationWorktreeStore.h
    src/agent/AgentTranscriptPaging.cpp
    src/agent/AgentTranscriptPaging.h
    src/agent/AgentTranscriptWindowBudget.cpp
    src/agent/AgentTranscriptWindowBudget.h
    src/agent/TranscriptRowGeometry.cpp
    src/agent/TranscriptRowGeometry.h
    src/agent/TranscriptRowMeasurer.cpp
    src/agent/TranscriptRowMeasurer.h
    src/agent/MermaidLabelContrast.cpp
    src/agent/MermaidLabelContrast.h
    src/agent/TranscriptFileLinks.cpp
    src/agent/TranscriptFileLinks.h
    src/agent/TranscriptNetworkPolicy.cpp
    src/agent/TranscriptNetworkPolicy.h
    src/agent/TranscriptTrace.cpp
    src/agent/TranscriptTrace.h
    src/agent/ChatMarkdown.cpp
    src/agent/ChatMarkdown.h
    src/agent/TranscriptWindowModel.cpp
    src/agent/TranscriptWindowModel.h
    src/agent/TranscriptSyntax.cpp
    src/agent/TranscriptSyntax.h
    src/agent/TranscriptTableStyle.cpp
    src/agent/TranscriptTableStyle.h
    src/agent/AgentWorkspaceMedia.cpp
    src/agent/AgentWorkspaceWorkingChanges.cpp
    src/agent/AgentWorkspaceImages.cpp
    src/agent/AgentWorkspaceMentions.cpp
    src/agent/AgentWorkspaceRenderCache.cpp
    src/agent/AgentWorkspaceSkills.cpp
    src/agent/AgentWorkspaceStreaming.cpp
    src/agent/AgentWorkspaceUsage.cpp
    src/agent/AgentWorkspaceView.cpp
    src/agent/AgentWorkspaceViewTeams.cpp
    src/agent/AgentWorkspaceHeldMessages.cpp
    src/agent/AgentWorkspaceReviewers.cpp
    src/agent/AgentWorkspaceAtlasResults.cpp
    src/agent/TranscriptAtlasLinks.cpp
    src/agent/TranscriptAtlasLinks.h
    src/agent/AgentWorkspaceView.h
    src/agent/CodexAppServer.cpp
    src/agent/CodexAppServer.h
    src/agent/CodexAppServerDaemonEvents.cpp
    src/agent/CodexAppServerDaemonSessions.cpp
    src/agent/CodexAppServerProcess.cpp
    src/agent/CodexDaemonTranscript.cpp
    src/agent/CodexDaemonDiscovery.cpp
    src/agent/CodexRateLimits.cpp
    src/agent/CodexRateLimits.h
    src/agent/ProviderRuntimeStatus.cpp
    src/agent/ProviderRuntimeStatus.h
    src/agent/ProviderSetupGuide.cpp
    src/agent/ProviderSetupGuide.h
    src/agent/FileMentions.cpp
    src/agent/FileMentions.h
    src/agent/FileReveal.cpp
    src/agent/FileReveal.h
)
target_link_libraries(ketplus_ai
    PUBLIC
        Qt6::Network
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        KetQt::Quick
        KetQt::QuickPlugin
        KetQt::Widgets
        Qt6::Sql
        Qt6::Widgets
        ketplus_browser_tools
        ketplus_remote
        ketplus_typography
        KetPlusCM::git
        KetPlusCM::preview
        KetPlusCM::settings_ui
        KetPlusCM::ui
)
target_include_directories(ketplus_ai PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_ai)

set_source_files_properties(
    src/agent/qml/AgentWorkspace.qml
    PROPERTIES QT_RESOURCE_ALIAS "AgentWorkspace.qml"
)
set_source_files_properties(
    src/agent/qml/AgentSidebar.qml
    PROPERTIES QT_RESOURCE_ALIAS "AgentSidebar.qml"
)
set_source_files_properties(
    src/agent/qml/AgentProductNavigation.qml
    PROPERTIES QT_RESOURCE_ALIAS "AgentProductNavigation.qml"
)
foreach(component AgentWorkspaceHeader AgentRunStatus AgentScopeNotice AgentEmptyState
        BackgroundTaskBar AgentComposer AgentComposerAction AgentPermissionMenu AgentSkillsMenu AgentAttachmentChip)
    set_source_files_properties(src/agent/qml/${component}.qml
        PROPERTIES QT_RESOURCE_ALIAS "${component}.qml"
    )
endforeach()
set_source_files_properties(
    src/agent/qml/ClaudeUsageSection.qml
    PROPERTIES QT_RESOURCE_ALIAS "ClaudeUsageSection.qml"
)
set_source_files_properties(
    src/agent/qml/UsageLimitsPopover.qml
    PROPERTIES QT_RESOURCE_ALIAS "UsageLimitsPopover.qml"
)
set_source_files_properties(
    src/agent/qml/UsageRingButton.qml
    PROPERTIES QT_RESOURCE_ALIAS "UsageRingButton.qml"
)
set_source_files_properties(src/agent/qml/CompactWindowControl.qml PROPERTIES QT_RESOURCE_ALIAS "CompactWindowControl.qml")
set_source_files_properties(
    src/agent/qml/ModelPicker.qml
    PROPERTIES QT_RESOURCE_ALIAS "ModelPicker.qml"
)
set_source_files_properties(
    src/agent/qml/ModelProviderTabs.qml
    PROPERTIES QT_RESOURCE_ALIAS "ModelProviderTabs.qml"
)
set_source_files_properties(
    src/agent/qml/MessageImageGallery.qml
    PROPERTIES QT_RESOURCE_ALIAS "MessageImageGallery.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptMessageImages.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptMessageImages.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptRemoteGroup.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptRemoteGroup.qml"
)
set_source_files_properties(src/agent/qml/AtlasResultCard.qml PROPERTIES QT_RESOURCE_ALIAS "AtlasResultCard.qml")
set_source_files_properties(src/agent/qml/HeldMessageAction.qml PROPERTIES QT_RESOURCE_ALIAS "HeldMessageAction.qml")
set_source_files_properties(src/agent/qml/AgentProviderSetupCard.qml PROPERTIES QT_RESOURCE_ALIAS "AgentProviderSetupCard.qml")
set_source_files_properties(src/agent/qml/TranscriptNotice.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptNotice.qml")
set_source_files_properties(src/agent/qml/TranscriptActivityHeader.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptActivityHeader.qml")
set_source_files_properties(src/agent/qml/TranscriptChangedFiles.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptChangedFiles.qml")
set_source_files_properties(src/agent/qml/TranscriptFileStep.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptFileStep.qml")
set_source_files_properties(src/agent/qml/TranscriptFileDiff.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptFileDiff.qml")
set_source_files_properties(src/agent/qml/TranscriptFileMenu.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptFileMenu.qml")
set_source_files_properties(src/agent/qml/TranscriptAgentNote.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptAgentNote.qml")
set_source_files_properties(src/agent/qml/TranscriptTeamBlock.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptTeamBlock.qml")
set_source_files_properties(src/agent/qml/TranscriptWorkReport.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptWorkReport.qml")
set_source_files_properties(src/agent/qml/WorkReportPane.qml PROPERTIES QT_RESOURCE_ALIAS "WorkReportPane.qml")
set_source_files_properties(src/agent/qml/ConversationChangesPane.qml PROPERTIES QT_RESOURCE_ALIAS "ConversationChangesPane.qml")
set_source_files_properties(src/agent/qml/TranscriptTrimmedDetail.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptTrimmedDetail.qml")
set_source_files_properties(
    src/agent/qml/TranscriptActivityRow.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptActivityRow.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptCopyButton.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptCopyButton.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptMermaidSwitch.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptMermaidSwitch.qml"
)
set_source_files_properties(
    src/agent/qml/AgentQuestionCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "AgentQuestionCard.qml"
)

set_source_files_properties(
    src/agent/qml/AgentApprovalCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "AgentApprovalCard.qml"
)
set_source_files_properties(
    src/agent/qml/WarmTranscriptHost.qml
    PROPERTIES QT_RESOURCE_ALIAS "WarmTranscriptHost.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptPosition.js
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptPosition.js"
)
set_source_files_properties(
    src/agent/qml/TranscriptRenderers.js
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptRenderers.js"
)
set_source_files_properties(
    src/agent/qml/TranscriptScrollController.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptScrollController.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptView.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptView.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptContextMenu.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptContextMenu.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptMessage.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptMessage.qml"
)
set_source_files_properties(
    src/agent/mermaid-placeholder.svg
    PROPERTIES QT_RESOURCE_ALIAS "mermaid-placeholder.svg"
)
set_source_files_properties(
    src/agent/mermaid-pending.svg
    PROPERTIES QT_RESOURCE_ALIAS "mermaid-pending.svg"
)
# The approved ketPlus mark (assets/brand, from mockups/qt): BrandLockup.qml, the welcome and
# the Settings appearance preview load it under :/agent/icons.
foreach(variant "" "-64" "-128")
    set_source_files_properties(
        "${PROJECT_SOURCE_DIR}/assets/brand/ketplus-mark${variant}.png"
        PROPERTIES QT_RESOURCE_ALIAS "icons/ketplus-mark${variant}.png"
    )
endforeach()
set_source_files_properties(
    src/agent/qml/BrandLockup.qml
    PROPERTIES QT_RESOURCE_ALIAS "BrandLockup.qml"
)


qt_add_resources(ketplus_ai "ketplus_ai_resources"
    PREFIX "/agent"
    FILES
        src/agent/qml/AgentWorkspace.qml
        src/agent/qml/AgentSidebar.qml
        src/agent/qml/AgentProductNavigation.qml
        src/agent/qml/BrandLockup.qml
        src/agent/qml/AgentWorkspaceHeader.qml
        src/agent/qml/AgentRunStatus.qml
        src/agent/qml/AgentScopeNotice.qml
        src/agent/qml/AgentEmptyState.qml
        src/agent/qml/BackgroundTaskBar.qml
        src/agent/qml/AgentComposer.qml
        src/agent/qml/AgentComposerAction.qml
        src/agent/qml/AgentAttachmentChip.qml
        src/agent/qml/AgentPermissionMenu.qml
        src/agent/qml/AgentSkillsMenu.qml
        src/agent/qml/UsageLimitsPopover.qml
        src/agent/qml/ClaudeUsageSection.qml
        src/agent/qml/UsageRingButton.qml
        src/agent/qml/CompactWindowControl.qml
        src/agent/qml/ModelPicker.qml
        src/agent/qml/ModelProviderTabs.qml
        src/agent/qml/MessageImageGallery.qml
        src/agent/qml/TranscriptMessageImages.qml
        src/agent/qml/TranscriptRemoteGroup.qml
        src/agent/qml/TranscriptNotice.qml
    src/agent/qml/HeldMessageAction.qml
    src/agent/qml/AgentProviderSetupCard.qml
    src/agent/qml/AtlasResultCard.qml
        src/agent/qml/TranscriptActivityRow.qml
        src/agent/qml/TranscriptActivityHeader.qml
        src/agent/qml/TranscriptChangedFiles.qml
        src/agent/qml/TranscriptFileDiff.qml
        src/agent/qml/TranscriptFileMenu.qml
        src/agent/qml/TranscriptFileStep.qml
        src/agent/qml/TranscriptAgentNote.qml
        src/agent/qml/TranscriptTeamBlock.qml
        src/agent/qml/TranscriptWorkReport.qml
        src/agent/qml/WorkReportPane.qml
        src/agent/qml/ConversationChangesPane.qml
        src/agent/qml/TranscriptTrimmedDetail.qml
        src/agent/qml/TranscriptCopyButton.qml
        src/agent/qml/TranscriptMermaidSwitch.qml
        src/agent/qml/AgentQuestionCard.qml
        src/agent/qml/AgentApprovalCard.qml
        src/agent/qml/WarmTranscriptHost.qml
        src/agent/qml/TranscriptPosition.js
        src/agent/qml/TranscriptRenderers.js
        src/agent/qml/TranscriptScrollController.qml
        src/agent/qml/TranscriptView.qml
        src/agent/qml/TranscriptContextMenu.qml
        src/agent/qml/TranscriptMessage.qml
        src/agent/mermaid-placeholder.svg
        src/agent/mermaid-pending.svg



























        "${PROJECT_SOURCE_DIR}/assets/brand/ketplus-mark-128.png"
)
