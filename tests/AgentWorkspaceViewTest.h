#pragma once

#include "ui/Theme.h"

#include <QObject>
#include <QQuickItem>

class AgentWorkspaceViewTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void markdownTableAfterCodeKeepsHeaderCells();
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
    void heldMessageRequiresExplicitClickAndFencesDuplicates();
    void heldMessageNoticeRendersInTranscript();
    void atlasResultsUseExplicitCompletedLinks();
    void atlasResultCardFencesChangedWorktrees();
    void webSearchLabelsAndAnimation();
    void activityGalleryLoadsInBatches();
    void browserEvidenceStaysVisible_data();
    void browserEvidenceStaysVisible();
    void activityIconsFollowTheMock_data();
    void activityIconsFollowTheMock();
    void browserStepsReadAsTheMock_data();
    void browserStepsReadAsTheMock();
    void browserStepsCollapseAroundEvidence_data();
    void browserStepsCollapseAroundEvidence();
    void fileChangesStayCompactInsideExpandedGroup();
    void fileStepsOpenTheirDiffInline();
    void claudeEditOpensItsDiffInline();
    void sentAgentMessageReadsAsTheMockNote();
    void settledApprovalsReadAsTheirOutcome();
    void capacityErrorOffersAnotherModel();
    void trimmedTurnOpensStorageSettings();
    void compactionReadsAsAnEventLine();
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
    void sidebarMatchesMockAtEveryZoom();
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
    void userTurnLeadsWithOriginAndImages();
    void imageViewerOpensTheRowsImages();
    void transcriptNoticesUseRuntimeState();
    void turnEndListsTheTurnsChangedFiles();
    void retryResendsTheLatestFailedPrompt();
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
    void welcomeSitsInTheViewport();
    void welcomeScrollsToEveryStarter_data();
    void welcomeScrollsToEveryStarter();
    void composerPreview();
    void composerFormattingSettles();
    void composerPlaceholderTracksCaret();
    void worktreeHeaderKeepsLabelVisible();
    void permissionMenuFitsSmallZoomedWorkspace();
    void tooltipsFollowDesignSystem();
    void measurementTimerStopsAndRestartsForNewRows();
    void transcriptFrameAnimationsStopOnTheirOwn();
    void minimizedWindowReleasesTheWorkingClock();
    void agentQmlUsesBoundedAnimationsAndKitPrimitives();
    void contextMenuFollowsDesignSystem();
    void conversationMenusOfferFork();
    void reviewerPickerFollowsTheActiveConversation();
    void reviewerLabelCollapsesBeforeTagsSqueezeTheTitle_data();
    void reviewerLabelCollapsesBeforeTagsSqueezeTheTitle();
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
    void activityStepsReadAsTheMock();
    void findIgnoresUnrelatedHistoryStateUpdates();
    void findGoesToLastResultWhenAnchorTrimmed();
    void findResultsRefreshOnModelChanges();
    void newMessageBadgeIgnoresNewerPagesAndMergedActivity();
};

namespace agent_workspace_test {

inline ketplus::ThemePalette testPalette() { return ketplus::currentThemePalette(); }

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

// Pooled delegates keep hidden copies; only a visible item is on screen.
inline QQuickItem* findVisibleItem(QQuickItem* item, const QString& name) {
    if (!item->isVisible())
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (auto* child : item->childItems()) {
        if (auto* match = findVisibleItem(child, name))
            return match;
    }
    return nullptr;
}

} // namespace agent_workspace_test
