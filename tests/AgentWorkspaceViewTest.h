#pragma once

#include "ui/Theme.h"

#include <QObject>
#include <QQuickItem>

class AgentWorkspaceViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void manualCompactWindow();
    void builtInBrowserSkillFollowsBrowserTools();
    void pathChangeScansSkillsOffTheGuiThread();
    void loadsQmlAndMapsWorktrees();
    void mentionsOnlyFilesInConversationWorktree();
    void sidebarGroupsConversationsWithoutWorktreeRail();
    void restoringDoesNotCoverChatPane();
    void weeklyLimitFooter();
    void compactFollowsProviderCapabilities();
    void usageLimitsPopover();
    void claudeUsageBudget();
    void usageLimitsFollowConversationProvider();
    void backgroundTaskBar();
    void webSearchLabelsAndAnimation();
    void activityGalleryLoadsInBatches();
    void browserEvidenceStaysVisible_data();
    void browserEvidenceStaysVisible();
    void browserStepsCollapseAroundEvidence_data();
    void browserStepsCollapseAroundEvidence();
    void fileChangesStayCompactInsideExpandedGroup();
    void activityOperationSemantics();
    void semanticCommandLabelsStayExpandable();
    void longCommandTitleStaysInsideColumn();
    void transcriptAtlasLayout();
    void describedCommandsReadAboveTheirCommand();
    void readRowTitleOpensTheFileAtItsLine();
    void retainedCommandNoticeRemainsVisible();
    void retainedFileDiffNoticeRemainsVisible();
    void retainedToolTextNoticeRemainsVisible_data();
    void retainedToolTextNoticeRemainsVisible();
    void transcriptRendererInteractionMatrix();
    void transcriptRendererInteractionMatrix_data();
    void transcriptArtifactSafetyAndExport();
    void messageModelUpdatesIncrementally();
    void expiredQuestionsLeaveNoTranscriptRow();
    void chunksPathologicalAssistantMessages();
    void appendDuringGiantRenderKeepsRows();
    void transcriptModelDeduplicatesPagesAndBoundsPayloads();
    void presentationGroupingIsDerivedAndDeterministic();
    void coalescesStreamingDeltas();
    void normalizesTranscriptV2();
    void normalizesTypedTranscriptCorpus();
    void userMessageShowsTheImagesItWasSentWith();
    void localImageActivityRendersRetainedMediaAfterSourceCleanup();
    void mergesLiveAndReplayTranscriptLifecycle();
    void sanitizesTranscriptPayloads();
    void coversVersionedTranscriptCorpus();
    void streamsLiveCommentary();
    void paginatesConversationHistory();
    void prependingAndStreamingKeepMessageAnchor();
    void prependingAndStreamingKeepMessageAnchor_data();
    void wheelStopsTailFollowing();
    void scrollingWhileStreamingKeepsReadingPosition();
    void scrollingWhileStreamingKeepsReadingPosition_data();
    void boundedWheelStabilizationPreservesContinuedAndReverseInput();
    void notchedWheelGlidesItsFullDistance();
    void prependKeepsVisibleDelegates();
    void windowTrimAboveKeepsReadingRow_data();
    void windowTrimAboveKeepsReadingRow();
    void resizeKeepsRowsFromOverlapping();
    void jumpToLatestFromFarAboveShowsNoScroll();
    void positionIndicatorIgnoresLoadedPages();
    void widthReflowKeepsReadingAnchor();
    void imageAttachmentReservesHeight();
    void messageImageSizeRejectsNonRegularFiles();
    void programmaticScrollAfterWheelIsNotUndone();
    void streamingRefreshDuringWheelKeepsPosition();
    void measuredHeightsAreReusedPerWidth();
    void historyPrefetchChainsThroughLongHistoryWithMomentum();
    void streamingTableKeepsReadingParagraph();
    void streamingTableKeepsReadingParagraph_data();
    void compactionLifecycleAndCommand();
    void answersAgentQuestionsSequentially();
    void questionCardSingleClickAndFreeText();
    void directModeAutoApprovesOnlyInjectedBrowserTools();
    void preservesQuestionDraftsAcrossScope();
    void presentsStateAxesIndependently();
    void linksFileReferencesInInlineCode();
    void mermaidFillsGetReadableLabels();
    void sidebarDividerRemainsResizable();
    void largeWorktreeCatalogDoesNotCreateSidebarRail();
    void exposesUiInteractions();
    void streamsAndKeepsSessionsByWorktree();
    void missingWorktreeKeepsHistoryAndCanStop();
    void missingWorktreeRestoresSavedAndDaemonSessions();
    void restoresMetadataBeforeDaemonDiscovery();
    void selectingColdDaemonConversationShowsChatRestoreState();
    void runningCheckoutsIncludeExecutionWorktrees();
    void configuredTurnsAndAttachments();
    void doesNotShowEmptyJsonWhenTurnFailsAfterError();
    void renamesAndRestoresConversations();
    void persistsConversationConfiguration();
    void reportsConversationContextWindow();
    void reportsPlanLimitsAndDropsWindowsThatAlreadyReset();
    void persistsComposerDraftByConversation();
    void hydratesSavedConversationOnSelection();
    void steersRunningTurn();
    void keepsSteeringDrafts();
    void copiesConversationTranscript();
    void runningIndicatorDoesNotOverlapLastCommand();
    void streamingResponseStaysAnchoredAboveRunStatus();
    void restoresConversationScrollPosition();
    void boundsWarmConversationRenderCache();
    void streamingPreservesReadingPosition();
    void streamingPreservesReadingPosition_data();
    void promotesConversationWhenAgentStarts();
    void animatesRunningConversationStatus();
    void workingIndicatorClockLifecycle();
    void formatsStreamingText();
    void transcriptDayDividersUseJournalDates();
    void transcriptNoticesUseRuntimeState();
    void opensInlineMarkdownLinks();
    void linksInlineCodeOnceFilesAreListed();
    void userMessageBubbleIgnoresUnavailableImage();
    void userBubbleFitsShortTextAndWrapsLongText();
    void compactCodeBlocks();
    void compactCodeBlocks_data();
    void markdownHeadingsAndInlineCodeStayNearBodySize();
    void reopenedRowFormatsItsDocumentOnce();
    void conversationColumnFollowsConfiguredWidth();
    void pastesImagesAndOpensIndividualFiles();
    void composerPreview();
    void composerFormattingSettles();
    void composerPlaceholderTracksCaret();
    void worktreeHeaderKeepsLabelVisible();
    void tooltipsFollowDesignSystem();
    void contextMenuFollowsDesignSystem();
    void conversationMenusOfferFork();
    void selectsModelsInUnifiedPicker();
    void restoresStoredConversationModelInsteadOfCatalogDefault();
    void prefetchesModelsWhenAgentBecomesAvailable();
    void liveCodexSmoke();
    // Transcript UX phase 5.
    void activityGroupRolesUpdateIncrementally();
    void activityGroupToggleKeepsReadingAnchor();
    void streamedActivityJoinsGroupWithoutReset();
    void activityGroupCopyIncludesMembers();
    void copiesMessageSourcesAndCodeBlocks();
    void copiesMouseSelectionWithShortcut();
    void listCodeBlocksStayInTheirItem();
    void fencedCodeBlockSources();
    void conversationCopyWhileRunning();
    void findCountsNavigatesAndExpandsGroups();
    void findSearchesOlderHistoryWithinBound();
    void messagesExposeAccessibleSemantics();
    void keyboardNavigatesMessagesAndGroups();
    void jumpToLatestCountsNewMessages();
    void collapsedGroupDelegateCost();
    void activityGroupKeyStableAcrossPagingAndTrims();
    void anchorKeysResolveToVisibleRows();
    void readerHeldWhenPrependTakesOverGroupLeader();
    void warmSwitchLaysOutTheActiveConversation();
    void approvalCardStaysOutsideCommandGroup();
    void groupExpansionSurvivesPagingAndIsPruned();
    void expandedGroupHangsUnderRail();
    void findIgnoresUnrelatedHistoryStateUpdates();
    void findGoesToLastResultWhenAnchorTrimmed();
    void findResultsRefreshOnModelChanges();
    void newMessageBadgeIgnoresNewerPagesAndMergedActivity();
};

namespace agent_workspace_test {

inline ketplus::ThemePalette testPalette() {
    ketplus::ThemePalette palette;
    palette.dark = true;
    palette.appBackground = QStringLiteral("#12161b");
    palette.pageBackground = QStringLiteral("#1b1f24");
    palette.sidebarBackground = QStringLiteral("#171b20");
    palette.panelBackground = QStringLiteral("#20252b");
    palette.panelSubtle = QStringLiteral("#1e2329");
    palette.surfaceRaised = QStringLiteral("#242930");
    palette.surfaceHover = QStringLiteral("#292f37");
    palette.textMain = QStringLiteral("#f2f4f7");
    palette.textSecondary = QStringLiteral("#c5cad3");
    palette.textMuted = QStringLiteral("#858d99");
    palette.textDisabled = QStringLiteral("#5f6670");
    palette.border = QStringLiteral("#292d33");
    palette.borderStrong = QStringLiteral("#3b424c");
    palette.interactiveBackground = QStringLiteral("#22272e");
    palette.interactiveHover = QStringLiteral("#292f37");
    palette.interactiveActive = QStringLiteral("#343b45");
    palette.accent = QStringLiteral("#4f6fff");
    palette.accentHover = QStringLiteral("#6380ff");
    palette.accentActive = QStringLiteral("#8094ff");
    palette.accentSubtle = QStringLiteral("#252d4a");
    palette.accentMuted = QStringLiteral("#354476");
    palette.focus = QStringLiteral("#6b82ff");
    palette.tabActiveText = QStringLiteral("#8094ff");
    palette.accentText = QStringLiteral("#9eb5ff");
    palette.actionPrimary = QStringLiteral("#365fd6");
    palette.actionPrimaryHover = QStringLiteral("#3151c6");
    palette.actionPrimaryActive = QStringLiteral("#2b46b7");
    palette.positive = QStringLiteral("#42b883");
    palette.warning = QStringLiteral("#d9a441");
    palette.danger = QStringLiteral("#ef6461");
    return palette;
}

inline QQuickItem* findVisualItem(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) {
        return item;
    }
    for (auto* child : item->childItems()) {
        if (auto* match = findVisualItem(child, name)) {
            return match;
        }
    }
    return nullptr;
}

} // namespace agent_workspace_test
