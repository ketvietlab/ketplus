add_library(ketplus_ai STATIC
    src/agent/AgentMessageFormatter.cpp
    src/agent/AgentMessageFormatter.h
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
    src/agent/DaemonTranscriptProjection.cpp
    src/agent/DaemonTranscriptProjection.h
    src/agent/AgentSessionManager.cpp
    src/agent/AgentSessionConfiguration.cpp
    src/agent/AgentSessionManager.h
    src/agent/AgentSessionNotifications.cpp
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
    src/agent/TranscriptTrace.cpp
    src/agent/TranscriptTrace.h
    src/agent/ChatMarkdown.cpp
    src/agent/ChatMarkdown.h
    src/agent/TranscriptWindowModel.cpp
    src/agent/TranscriptWindowModel.h
    src/agent/TranscriptSyntax.cpp
    src/agent/TranscriptSyntax.h
    src/agent/AgentWorkspaceMedia.cpp
    src/agent/AgentWorkspaceMentions.cpp
    src/agent/AgentWorkspaceRenderCache.cpp
    src/agent/AgentWorkspaceSkills.cpp
    src/agent/AgentWorkspaceStreaming.cpp
    src/agent/AgentWorkspaceUsage.cpp
    src/agent/AgentWorkspaceView.cpp
    src/agent/SymbolImageProvider.cpp
    src/agent/SymbolImageProvider.h
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
    src/agent/FileMentions.cpp
    src/agent/FileMentions.h
    src/agent/FileReveal.cpp
    src/agent/FileReveal.h
)
target_link_libraries(ketplus_ai
    PUBLIC
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        KetQt::Quick
        KetQt::QuickPlugin
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
        BackgroundTaskBar MotionClock AgentComposer AgentComposerAction AgentPermissionMenu AgentSkillsMenu AgentAttachmentChip)
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
set_source_files_properties(
    src/agent/qml/TintedIcon.qml
    PROPERTIES QT_RESOURCE_ALIAS "TintedIcon.qml"
)
set_source_files_properties(src/agent/qml/CompactWindowControl.qml PROPERTIES QT_RESOURCE_ALIAS "CompactWindowControl.qml")
set_source_files_properties(
    src/agent/qml/ModelPicker.qml
    PROPERTIES QT_RESOURCE_ALIAS "ModelPicker.qml"
)
set_source_files_properties(
    src/agent/qml/MessageImageGallery.qml
    PROPERTIES QT_RESOURCE_ALIAS "MessageImageGallery.qml"
)
set_source_files_properties(
    src/agent/qml/TranscriptRemoteGroup.qml
    PROPERTIES QT_RESOURCE_ALIAS "TranscriptRemoteGroup.qml"
)
set_source_files_properties(src/agent/qml/TranscriptActivityHeader.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptActivityHeader.qml")
set_source_files_properties(src/agent/qml/TranscriptChangedFiles.qml PROPERTIES QT_RESOURCE_ALIAS "TranscriptChangedFiles.qml")
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
    src/agent/qml/KetToolTip.qml
    PROPERTIES QT_RESOURCE_ALIAS "KetToolTip.qml"
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
set_source_files_properties(
    "${PROJECT_SOURCE_DIR}/assets/brand/ketplus-mark.png"
    PROPERTIES QT_RESOURCE_ALIAS "icons/ketplus-mark.png"
)

foreach(icon zap shield lock arrow-up send square git-branch bot square-terminal file-code image search copy play)
    set_source_files_properties(src/agent/icons/${icon}.svg
        PROPERTIES QT_RESOURCE_ALIAS "icons/${icon}.svg"
    )
endforeach()
set_source_files_properties(
    "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1/ketplus-logo-light.png"
    PROPERTIES QT_RESOURCE_ALIAS "brand/ketplus-logo-light.png"
)
set_source_files_properties(
    "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1/ketplus-logo-dark.png"
    PROPERTIES QT_RESOURCE_ALIAS "brand/ketplus-logo-dark.png"
)
foreach(icon messages-square folder network unplug settings chevron-down chevron-right square-pen paperclip sparkles)
    set_source_files_properties(src/agent/icons/${icon}.svg
        PROPERTIES QT_RESOURCE_ALIAS "icons/${icon}.svg"
    )
endforeach()
foreach(icon plus rotate-ccw check)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/assets/icons/lucide/${icon}.svg"
        PROPERTIES QT_RESOURCE_ALIAS "icons/${icon}.svg"
    )
endforeach()

set_source_files_properties(src/agent/qml/ChromeMetrics.js PROPERTIES QT_RESOURCE_ALIAS "ChromeMetrics.js")

qt_add_resources(ketplus_ai "ketplus_ai_resources"
    PREFIX "/agent"
    FILES
        src/agent/qml/AgentWorkspace.qml
        src/agent/qml/AgentSidebar.qml
        src/agent/qml/AgentProductNavigation.qml
        src/agent/qml/AgentWorkspaceHeader.qml
        src/agent/qml/AgentRunStatus.qml
        src/agent/qml/AgentScopeNotice.qml
        src/agent/qml/AgentEmptyState.qml
        src/agent/qml/BackgroundTaskBar.qml
        src/agent/qml/MotionClock.qml
        src/agent/qml/AgentComposer.qml
        src/agent/qml/AgentComposerAction.qml
        src/agent/qml/AgentAttachmentChip.qml
        src/agent/qml/AgentPermissionMenu.qml
        src/agent/qml/AgentSkillsMenu.qml
        src/agent/qml/UsageLimitsPopover.qml
        src/agent/qml/ClaudeUsageSection.qml
        src/agent/qml/UsageRingButton.qml
        src/agent/qml/TintedIcon.qml
        src/agent/qml/CompactWindowControl.qml
        src/agent/qml/ModelPicker.qml
        src/agent/qml/MessageImageGallery.qml
        src/agent/qml/TranscriptRemoteGroup.qml
        src/agent/qml/TranscriptActivityRow.qml
        src/agent/qml/TranscriptActivityHeader.qml
        src/agent/qml/TranscriptChangedFiles.qml
        src/agent/qml/TranscriptCopyButton.qml
        src/agent/qml/TranscriptMermaidSwitch.qml
        src/agent/qml/KetToolTip.qml
        src/agent/qml/AgentQuestionCard.qml
        src/agent/qml/AgentApprovalCard.qml
        src/agent/qml/WarmTranscriptHost.qml
        src/agent/qml/ChromeMetrics.js
        src/agent/qml/TranscriptPosition.js
        src/agent/qml/TranscriptRenderers.js
        src/agent/qml/TranscriptScrollController.qml
        src/agent/qml/TranscriptView.qml
        src/agent/qml/TranscriptContextMenu.qml
        src/agent/qml/TranscriptMessage.qml
        src/agent/mermaid-placeholder.svg
        src/agent/mermaid-pending.svg
        src/agent/icons/zap.svg
        src/agent/icons/shield.svg
        src/agent/icons/lock.svg
        src/agent/icons/arrow-up.svg
        src/agent/icons/send.svg
        src/agent/icons/square.svg
        src/agent/icons/git-branch.svg
        src/agent/icons/bot.svg
        src/agent/icons/sparkles.svg
        src/agent/icons/square-terminal.svg
        src/agent/icons/file-code.svg
        src/agent/icons/image.svg
        src/agent/icons/search.svg
        src/agent/icons/copy.svg
        src/agent/icons/play.svg
        src/agent/icons/messages-square.svg
        src/agent/icons/folder.svg
        src/agent/icons/network.svg
        src/agent/icons/unplug.svg
        src/agent/icons/settings.svg
        src/agent/icons/chevron-down.svg
        src/agent/icons/chevron-right.svg
        src/agent/icons/square-pen.svg
        src/agent/icons/paperclip.svg
        "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1/ketplus-logo-light.png"
        "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1/ketplus-logo-dark.png"
        "${PROJECT_SOURCE_DIR}/assets/icons/lucide/plus.svg"
        "${PROJECT_SOURCE_DIR}/assets/icons/lucide/rotate-ccw.svg"
        "${PROJECT_SOURCE_DIR}/assets/icons/lucide/check.svg"
        "${PROJECT_SOURCE_DIR}/assets/brand/ketplus-mark.png"
)
