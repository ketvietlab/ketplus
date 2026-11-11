import Ket
import Ket as Kit
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: messageDelegate
    readonly property var userRecipe: Kit.KetTheme.userMessageRecipe(KetTheme.dark, KetTheme.zoom)
    readonly property var groupRecipe: Kit.KetTheme.activityGroupRecipe(KetTheme.dark, KetTheme.zoom, false, false, true, false)
    readonly property var codeRecipe: Kit.KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom)
    required property var shell
    required property var host
    required property var preview
    required property var view
    readonly property alias readingTextItem: bodyText
    required property int rowIndex
    // The row in the message model. Named index because every caller already
    // reads it under that name.
    readonly property int index: rowIndex
    required property string kind
    required property string messageKey
    required property string body
    required property var imageSources
    required property var fileStats
    required property var questionData
    required property var questionAnswerData
    required property string timestamp
    required property string meta
    required property string filePath
    required property string subtype
    required property string command
    required property bool partialBody
    required property int bodyPrefixCharacters
    required property bool chunkContinuation
    required property int estimatedBodyLines
    required property string groupKey
    required property string groupRole
    required property string groupSummary
    required property int groupSize
    required property bool groupRunning
    required property int groupFailed
    readonly property bool groupLeader: groupRole === "leader"
    readonly property bool groupExpanded: groupRole.length > 0 && view.groupExpanded(groupKey)
    // An activity row inside an expanded group is drawn compact, with tighter vertical padding,
    // and hangs under a rail indented by groupRailInset.
    readonly property bool groupNested: activityRow && groupRole.length > 0 && groupExpanded
    // The last step of an open group: the mock's steps end with room under them, inside the rail.
    readonly property bool groupEnd: {
        if (!groupNested || !view.messageModel)
            return false;
        // The next row may change in place, without the row count changing.
        view.messageModel.sourceRevision;
        const rows = view.messageModel.count;
        return index + 1 >= rows || view.messageModel.get(index + 1).groupKey !== groupKey;
    }
    readonly property real groupRailInset: groupRecipe.childrenIndent + groupRecipe.childrenPadding + 1
    // A day divider is a block of its own in the mock: the request under it keeps its distance.
    readonly property real dayLead: dayDivider.visible ? dayDivider.height + KetTheme.px(12, KetTheme.zoom) : 0
    readonly property int surfaceInset: activityRow ? 0 : KetTheme.px(4, KetTheme.zoom)
    onYChanged: {
        if (!view.followTail && shell.transcriptWheel.direction !== 0)
            host.scheduleTranscriptRestore();
        if (!shell.restoringTranscript && shell.transcriptAnchor && messageKey === shell.transcriptAnchor.key)
            host.scheduleTranscriptRestore();
    }

    required property bool running
    required property string category
    required property string eventType
    // The provider a steer joined, from its journal event; older steers have none.
    required property string provider
    // ADR 0110: the daemon's code for a turn the provider ended; the row shows its setup guide.
    required property string errorCode
    // The provider's own words and the model of the turn that failed.
    required property string errorDetail
    required property string errorModel
    // Only the failure that ended the latest turn shows the full guide; once the user writes
    // again, the row reads as the notice it was.
    readonly property var providerSetup: {
        if (kind !== "error" || !errorCode || !view.messageModel)
            return null;
        view.messageModel.sourceRevision;
        host.providerRuntime; // the commands the daemon reports for the provider
        const rows = view.messageModel.count;
        for (let next = index + 1; next < rows && next <= index + 50; ++next) {
            const later = view.messageModel.get(next);
            // A later turn, the user's or an agent's, or a newer failure.
            if (later.kind === "user" || (later.kind === "error" && later.errorCode))
                return null;
        }
        const turnModel = errorModel ? shell.modelForId(errorModel) : null;
        const modelName = turnModel && turnModel.displayName ? turnModel.displayName
                        : errorModel || shell.modelLabel || shell.selectedModel || "";
        const guide = host.providerTurnGuide(provider || host.conversationProvider || shell.selectedProvider,
                                             errorCode, errorDetail || body, modelName);
        return guide && guide.id ? guide : null;
    }
    required property string phase
    required property string title
    required property string summary
    required property string target
    required property string status
    required property string cwd
    required property string commandDescription
    required property string fileLocation
    // What history retention reduced in the turn this row ends.
    required property string trimmedDetail
    required property var exitCode
    required property var durationMs
    required property string detailsData
    required property var atlasLinks
    required property string payloadData
    required property string detailsRef
    required property string payloadRef
    readonly property var messageImages: imageSources || []
    readonly property var messageQuestions: questionData || []
    readonly property var messageQuestionAnswers: questionAnswerData || ({})
    // Error banners and event lines sit a block apart in the mock, closer than other notices.
    readonly property bool tightNotice: noticeRow && (kind === "error" || subtype === "event" || subtype === "compaction")
    // A failure and the end of its turn read as one block in the mock: the banner, then the line.
    readonly property bool errorBeforeTurnEnd: {
        if (kind !== "error" || !view.messageModel)
            return false;
        // The next row may change in place, without the row count changing.
        view.messageModel.sourceRevision;
        const rows = view.messageModel.count;
        return index + 1 < rows && view.messageModel.get(index + 1).subtype === "turnEnd";
    }
    // A failed turn whose error banner sits just above it: the banner already says so, and the
    // mock's workspace/error shows no "Failed" line under it.
    readonly property bool failureShownAbove: {
        if (subtype !== "turnEnd" || status !== "failed" || index < 1 || !view.messageModel)
            return false;
        view.messageModel.sourceRevision;
        return view.messageModel.get(index - 1).kind === "error";
    }
    // An event line that opens the transcript ("Switched from…", "Forked from…") starts where a
    // day divider would, right under the top padding.
    readonly property real firstRowLift: index === 0 && tightNotice && kind !== "error" ? KetTheme.px(8, KetTheme.zoom) : 0
    readonly property bool activityRow: category === "activity" && !noticeRow
    readonly property bool noticeRow: subtype !== "question" && subtype !== "approval" && (category === "system" || subtype === "notice" || subtype === "compaction" || kind === "error")
    readonly property bool commandRow: activityRow && subtype === "command"
    readonly property string displayBody: shell.displayMessageBody(kind, body)
    readonly property string userExpansionKey: JSON.stringify(["user-message", view.cacheScope, messageKey])
    property bool userExpanded: false
    function restoreUserExpansion() {
        userExpanded = view.activityState(userExpansionKey)?.expanded === true;
    }
    onUserExpansionKeyChanged: restoreUserExpansion()
    // The reading column plus the 20 px inset each side that rows keep from the pane's edges.
    readonly property real contentColumnWidth: Math.min(width, shell.conversationColumnWidth + KetTheme.px(40, KetTheme.zoom))
    readonly property real contentColumnX: (width - contentColumnWidth) / 2
    // Like Claude: an agent turn has one copy action, under its last answer, for the text the
    // agent wrote (not its commands or files); a request keeps its own copy.
    readonly property int answerEnd: kind === "assistant" ? view.answerEndState(index, view.messageModel ? view.messageModel.count : 0) : 0
    readonly property bool turnAnswered: answerEnd === 1 || (answerEnd === 2 && !host.sessionRunning)
    readonly property bool copyableMessage: answerEnd > 0 || (kind === "user" && !activityRow && !chunkContinuation)
    HoverHandler {
        id: rowHover
    }
    // Per-row chrome (copy buttons and their tool tips) exists only while the row is hovered,
    // keyboard-focused or one of its buttons holds focus; collapsed members never create it.
    property bool chromeFocusHold: false
    // The answer still being written is known by its own row state, not by being last: a
    // steer or queued message may land under it while its text keeps growing.
    readonly property bool codeCopyShown: chromeActive || !(host.sessionRunning && kind === "assistant" && meta.endsWith(" · streaming"))
    readonly property bool chromeActive: (rowHover.hovered || chromeFocusHold || (view.activeFocus && view.keyboardIndex === index))
    // The mock's line under an answer whose turn was stopped.
    readonly property bool answerStopped: answerEnd > 0 && view.answerStopped(index, view.messageModel ? view.messageModel.count : 0)
    Text {
        id: stoppedNote
        objectName: "answerStoppedNote"
        visible: messageDelegate.answerStopped
        x: messageSurface.x
        y: messageSurface.y + messageSurface.height + KetTheme.px(4, KetTheme.zoom)
        height: visible ? implicitHeight + KetTheme.px(4, KetTheme.zoom) : 0
        text: "Stopped before the answer finished"
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.px(12, KetTheme.zoom)
    }
    // As in the mock, the turn's answer always offers Copy answer under it; a prompt's copy
    // comes with the hover or focus.
    Loader {
        active: kind === "user" ? messageDelegate.copyableMessage && messageDelegate.chromeActive : messageDelegate.turnAnswered
        z: 2
        width: KetTheme.px(28, KetTheme.zoom)
        height: KetTheme.px(28, KetTheme.zoom)
        x: kind === "user" ? messageSurface.x - width - 4 : messageSurface.x
        y: kind === "user" ? messageSurface.y + 2 : messageSurface.y + messageSurface.height + stoppedNote.height + KetTheme.px(8, KetTheme.zoom)
        sourceComponent: TranscriptCopyButton {
            objectName: "copyMessageButton"

            label: kind === "user" ? "Copy message" : "Copy answer"
            padding: KetTheme.px(7, KetTheme.zoom)
            idleColor: KetTheme.t.pageBg
            cornerRadius: KetTheme.px(5, KetTheme.zoom)
            focusBorder: true
            onActiveFocusChanged: messageDelegate.chromeFocusHold = activeFocus
            onClicked: host.copyText(kind === "user" ? view.messageCopyText(messageDelegate.index) : view.answerCopyText(messageDelegate.index))
        }
    }
    // Accessibility (U4).
    readonly property string accessibleExcerpt: {
        // Slice before normalizing: bodies can be megabytes and this runs for every row.
        const text = (activityRow ? (groupLeader ? groupSummary : (title.length > 0 ? title : command)) : body).slice(0, 400).replace(/\s+/g, " ").trim();
        return text.length > 120 ? text.slice(0, 117) + "…" : text;
    }
    Accessible.role: Accessible.ListItem
    Accessible.name: kind === "user" ? (subtype === "agentMessage" ? meta + ": " : "You: ") + accessibleExcerpt : activityRow ? (groupLeader ? accessibleExcerpt : "Activity: " + accessibleExcerpt) : (kind === "assistant" ? "Codex: " : "") + accessibleExcerpt
    Accessible.description: activityRow ? [groupLeader ? (groupExpanded ? "expanded" : "collapsed") : "", (groupLeader ? groupRunning : running) ? "running" : status, groupFailed > 0 ? groupFailed + " failed" : ""].filter(part => part && part.length > 0).join(", ") : (running ? "streaming" : "")
    function toggleActivityDetails() {
        if (transcriptActivityLoader.item)
            transcriptActivityLoader.item.toggleDetails();
    }
    Loader {
        active: view.keyboardFocusVisible && view.activeFocus && view.keyboardIndex === messageDelegate.index
        z: 3
        sourceComponent: Rectangle {
            objectName: "keyboardFocusRing"
            x: messageSurface.x - KetTheme.px(6, KetTheme.zoom)
            y: messageSurface.y - 3
            width: messageSurface.width + KetTheme.px(12, KetTheme.zoom)
            height: messageSurface.height + KetTheme.px(6, KetTheme.zoom)
            radius: KetTheme.px(6, KetTheme.zoom)
            color: "transparent"
            border.color: KetTheme.t.colorFocus
            border.width: 2
        }
    }
    readonly property bool findCurrentRow: view.findOpen && view.findCurrentKey.length > 0 && view.findCurrentKey === messageKey
    Loader {
        active: messageDelegate.findCurrentRow && messageDelegate.activityRow
        sourceComponent: Rectangle {
            objectName: "findActivityHighlight"
            x: messageSurface.x - KetTheme.px(6, KetTheme.zoom)
            y: messageSurface.y - 2
            width: messageSurface.width + KetTheme.px(12, KetTheme.zoom)
            height: messageSurface.height + KetTheme.px(4, KetTheme.zoom)
            radius: KetTheme.px(6, KetTheme.zoom)
            color: "transparent"
            border.color: KetTheme.t.accent
            border.width: 1
        }
    }
    width: view.width
    height: messageSurface.y + messageSurface.height + steerNotice.height + surfaceInset - (errorBeforeTurnEnd ? KetTheme.px(14, KetTheme.zoom) : 0) + (activityRow ? KetTheme.px(groupEnd ? 20 : 3, KetTheme.zoom) : kind === "user" ? KetTheme.px(29, KetTheme.zoom) : copyableMessage ? stoppedNote.height + KetTheme.px(44, KetTheme.zoom) : view.rowSpacing)
    // Publish what this row actually is. The geometry model only ever learns a
    // height from the row that laid itself out, so a row it has not seen keeps
    // one fixed constant instead of borrowing another row's size.
    // Both numbers together: the row, and how much of it was body. Their
    // difference is the chrome this row's shape carries, which is how a row that
    // was measured but never realized gets a height.
    readonly property real reportedBodyHeight: bodyText.visible ? bodyText.height : -1
    onHeightChanged: view.publishRowHeight(messageKey, height, reportedBodyHeight, messageDelegate)
    onReportedBodyHeightChanged: view.publishRowHeight(messageKey, height, reportedBodyHeight, messageDelegate)
    Component.onCompleted: {
        restoreUserExpansion();
        view.publishRowHeight(messageKey, height, reportedBodyHeight, messageDelegate);
    }

    // The rail an expanded group's rows hang under. It spans the whole row, so the
    // rails of consecutive members join; the leader's starts below its summary line.
    Rectangle {
        objectName: "transcriptGroupRail"
        visible: messageDelegate.groupNested
        x: messageSurface.x + KetTheme.px(7, KetTheme.zoom)
        y: messageDelegate.groupLeader ? messageSurface.y + messageColumn.y + transcriptActivityLoader.y : 0
        width: Math.max(1, KetTheme.px(2, KetTheme.zoom))
        height: Math.max(0, parent.height - y)
        color: KetTheme.t.panelBorder
    }
    Kit.KetDayDivider {
        id: dayDivider
        objectName: "transcriptDayDivider"
        width: messageDelegate.contentColumnWidth - KetTheme.px(40, KetTheme.zoom)
        x: messageDelegate.contentColumnX + KetTheme.px(20, KetTheme.zoom)
        text: kind === "user" && !chunkContinuation ? view.dayLabel(index, timestamp, view.messageModel ? view.messageModel.count : 0) : ""
        visible: text.length > 0
        height: visible ? implicitHeight : 0
    }
    // The mock's user turn: who sent it and its images sit above the bubble, right-aligned.
    Column {
        id: userLead
        objectName: "userMessageLead"
        readonly property real gap: visible ? KetTheme.px(6, KetTheme.zoom) : 0
        visible: kind === "user" && !messageDelegate.activityRow && (messageSurface.agentMessage || imageGallery.imageCount > 0)
        width: messageSurface.userMaximumWidth
        height: visible ? implicitHeight : 0
        x: messageDelegate.contentColumnX + messageDelegate.contentColumnWidth - width - KetTheme.px(20, KetTheme.zoom)
        y: messageDelegate.surfaceInset + messageDelegate.dayLead
        spacing: KetTheme.px(6, KetTheme.zoom)
        Text {
            id: agentOrigin
            objectName: "agentMessageOrigin"
            visible: messageSurface.agentMessage
            width: parent.width
            text: visible ? meta : ""
            horizontalAlignment: Text.AlignRight
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            elide: Text.ElideRight
            Accessible.name: text
        }
        // Reparenting one gallery between the lead and the bubble stops its taps reaching the
        // thumbnails, so user rows and the rest each declare their own.
        TranscriptMessageImages {
            id: imageGallery
            row: messageDelegate
            sources: kind === "user" ? messageDelegate.messageImages : []
            x: parent.width - width
        }
    }
    Text {
        id: steerNotice
        objectName: "transcriptSteerNotice"
        visible: kind === "user" && eventType === "turn.steered"
        x: messageDelegate.contentColumnX + messageDelegate.contentColumnWidth - width - KetTheme.px(20, KetTheme.zoom)
        y: messageSurface.y + messageSurface.height + KetTheme.px(6, KetTheme.zoom)
        width: Math.min(implicitWidth, messageDelegate.contentColumnWidth - KetTheme.px(40, KetTheme.zoom))
        height: visible ? implicitHeight + KetTheme.px(6, KetTheme.zoom) : 0
        text: "Sent while " + (provider ? shell.providerTitle(provider) : "the agent") + " was working · joined the running turn"
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignRight
    }
    Rectangle {
        id: messageSurface
        objectName: "messageSurface"
        // Banners and turn ends span the reading column, as the mock's KetBanner does.
        readonly property int horizontalPadding: messageDelegate.activityRow || kind === "assistant" || messageDelegate.noticeRow ? 0 : (kind === "user" ? userRecipe.paddingInline : KetTheme.px(18, KetTheme.zoom))
        readonly property bool userMessage: kind === "user"
        // ADR 0027: another conversation's agent wrote this turn's opening message.
        readonly property bool agentMessage: kind === "user" && subtype === "agentMessage"
        readonly property real userMaximumWidth: Math.max(KetTheme.px(160, KetTheme.zoom), Math.min(messageDelegate.contentColumnWidth * userRecipe.maxWidth, messageDelegate.contentColumnWidth - KetTheme.px(63, KetTheme.zoom)))
        x: kind === "user" ? messageDelegate.contentColumnX + messageDelegate.contentColumnWidth - width - KetTheme.px(20, KetTheme.zoom) : messageDelegate.contentColumnX + KetTheme.px(20, KetTheme.zoom)
        y: messageDelegate.surfaceInset + messageDelegate.dayLead + userLead.height + (imagesOnly ? 0 : userLead.gap) - messageDelegate.firstRowLift
        width: kind === "user" ? Math.min(userMaximumWidth, Math.ceil(bodyMetrics.advanceWidth) + KetTheme.px(10, KetTheme.zoom) + horizontalPadding * 2) : messageDelegate.contentColumnWidth - KetTheme.px(40, KetTheme.zoom)
        // An images-only prompt keeps no empty bubble under its thumbnails.
        readonly property bool imagesOnly: userMessage && userLead.visible && messageDelegate.displayBody.trim().length === 0
        visible: !imagesOnly
        height: imagesOnly ? 0 : messageColumn.implicitHeight + (messageDelegate.activityRow ? 0 : kind === "user" ? 2 * userRecipe.paddingBlock : KetTheme.px(messageDelegate.tightNotice ? 8 : 16, KetTheme.zoom))
        // A file edit read as an activity step is a line like any other step, as in the mock;
        // only a standalone file message keeps its raised card.
        readonly property bool fileCard: kind === "file" && !messageDelegate.activityRow
        radius: kind === "user" ? userRecipe.radius : fileCard ? KetTheme.px(7, KetTheme.zoom) : 0
        color: kind === "user" ? userRecipe.background : fileCard ? KetTheme.t.surfaceRaised : "transparent"
        border.color: fileCard ? KetTheme.t.panelBorder : "transparent"

        Column {
            id: messageColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: messageSurface.horizontalPadding
            anchors.rightMargin: messageSurface.horizontalPadding
            anchors.verticalCenter: parent.verticalCenter
            spacing: kind === "file" ? KetTheme.px(4, KetTheme.zoom) : KetTheme.px(7, KetTheme.zoom)

            TranscriptTrimmedDetail {
                width: parent.width
                visible: messageDelegate.subtype === "turnEnd" && messageDelegate.trimmedDetail.length > 0
                height: visible ? implicitHeight : 0
                message: messageDelegate.trimmedDetail
                host: messageDelegate.host
            }
            // The mock's changed-files card closes a turn that edited files, above its end.
            Kit.KetChangedFiles {
                id: turnChanges
                objectName: "transcriptTurnChanges"
                // The source's revision re-reads the turn when a row changes in place.
                readonly property var turn: messageDelegate.subtype === "turnEnd" && view.messageModel ? (view.messageModel.sourceRevision, view.messageModel.turnFiles(messageDelegate.index)) : null
                readonly property var changes: turn && turn.files ? turn.files : []
                width: parent.width
                visible: changes.length > 0
                height: visible ? implicitHeight : 0
                files: changes.map(file => {
                    const shown = String(file.destination || file.path)
                    const split = Math.max(shown.lastIndexOf("/"), shown.lastIndexOf("\\"))
                    return {name: shown.slice(split + 1), path: split > 0 ? shown.slice(0, split) : "", added: file.added || 0, removed: file.removed || 0, known: file.known !== false}
                })
                // A daemon group lists at most 50 files; Review changes shows them all.
                complete: !turn || turn.complete !== false
                title: complete ? "" : changes.length + "+ files changed"
                // As in the mock, while Source Control shows any of the files uncommitted.
                note: host.workingChangesRevision >= 0 && changes.length > 0 && host.hasUncommittedChange(changes.map(file => String(file.destination || file.path))) ? "Not committed" : ""
                onFileActivated: index => host.requestFileDiff(String(changes[index].destination || changes[index].path))
                footer: Kit.KetButton {
                    objectName: "transcriptReviewChanges"
                    text: "Review changes"
                    controlSize: Kit.KetTheme.ControlSize.Small
                    onClicked: host.requestProductNavigation("source-control")
                }
            }
            // ADR 0106: the work report the turn ended with, below its changed files.
            TranscriptWorkReport {
                readonly property var turnReport: messageDelegate.subtype === "turnEnd" && view.messageModel ? (view.messageModel.sourceRevision, view.messageModel.turnReport(messageDelegate.index)) : null
                width: parent.width
                visible: !!turnReport && !!turnReport.items && turnReport.items.length > 0
                height: visible ? implicitHeight : 0
                report: visible ? turnReport : ({})
                onOpenRequested: agentHost.requestWorkReport()
            }
            TranscriptAgentNote {
                width: parent.width
                visible: messageDelegate.subtype === "agentNote"
                height: visible ? implicitHeight : 0
                label: messageDelegate.title
                details: messageDelegate.meta
                message: messageDelegate.displayBody
                state: messageDelegate.status
            }
            TranscriptNotice {
                objectName: "transcriptNotice"
                width: parent.width
                visible: messageDelegate.noticeRow && messageDelegate.subtype !== "agentNote" && !messageDelegate.providerSetup && !messageDelegate.failureShownAbove
                height: visible ? implicitHeight : 0

                title: messageDelegate.title || (kind === "error" ? "Agent error" : meta || "Conversation update")
                message: messageDelegate.displayBody
                status: kind === "error" ? "failed" : messageDelegate.status
                subtype: messageDelegate.subtype
                // As in the mock, a model at capacity offers another model instead of the same one.
                readonly property bool capacity: messageDelegate.subtype === "modelCapacity"
                actionText: kind === "error" && shell.retryableFailure && shell.retryableFailure.key === messageDelegate.messageKey ? (capacity ? "Switch model" : "Retry") : ""
                onActionTriggered: capacity ? shell.openModelPicker() : shell.retryFailedTurn(messageDelegate.messageKey)
            }
            Loader {
                objectName: "transcriptProviderSetup"
                active: !!messageDelegate.providerSetup
                visible: active
                width: parent.width
                height: active && item ? item.implicitHeight : 0
                sourceComponent: AgentProviderSetupCard {
                    shell: messageDelegate.shell
                    setup: messageDelegate.providerSetup
                    messageKey: messageDelegate.messageKey
                }
            }
            Loader {
                active: messageDelegate.subtype === "heldAgentMessage"
                visible: active
                width: parent.width
                height: active && item ? item.implicitHeight : 0
                sourceComponent: HeldMessageAction {

                    messageId: {
                        try { return JSON.parse(messageDelegate.payloadData).messageId || ""; }
                        catch (e) { return ""; }
                    }
                }
            }
            Loader {
                active: messageDelegate.groupLeader
                visible: active
                width: parent.width
                height: active ? groupRecipe.height : 0
                // One muted summary line followed by its chevron, as the mobile transcript
                // reads; the rows open under a rail below it.
                sourceComponent: Basic.AbstractButton {
                    id: groupHeader
                    activeFocusOnTab: false
                    hoverEnabled: true
                    contentItem: Item {}
                    onClicked: view.toggleGroup(messageDelegate.groupKey)
                    background: Rectangle {
                        radius: messageDelegate.groupRecipe.radius
                        color: Kit.KetTheme.activityGroupRecipe(KetTheme.dark, KetTheme.zoom, parent.hovered, parent.down, true, parent.visualFocus).background
                    }
                    objectName: "transcriptGroupHeader"
                    Text {
                        id: groupSummaryText
                        objectName: "transcriptGroupSummary"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - groupChevron.width - KetTheme.px(36, KetTheme.zoom))
                        text: messageDelegate.groupSummary + (messageDelegate.groupRunning ? " · Running" : "")
                        // A failure shows on its own step, as in the mock; the summary stays quiet.
                        color: messageDelegate.groupRecipe.text
                        font.family: KetTheme.fontFamily
                        font.pixelSize: messageDelegate.groupRecipe.fontPixels
                        font.weight: Font.Normal
                        elide: Text.ElideRight
                    }
                    KetIcon {
                        id: groupChevron
                        objectName: "transcriptGroupChevron"
                        anchors.left: groupSummaryText.right
                        anchors.leftMargin: KetTheme.px(4, KetTheme.zoom)
                        anchors.verticalCenter: parent.verticalCenter
                        width: KetTheme.px(14, KetTheme.zoom)
                        height: width
                        name: KetTheme.iconForLucide(messageDelegate.groupExpanded ? "chevron-up" : "chevron-down")
                        color: KetTheme.t.textMuted
                        box: KetTheme.px(14, KetTheme.zoom)
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }

            Loader {
                id: transcriptActivityLoader
                active: messageDelegate.activityRow && subtype !== "question" && subtype !== "approval" && (!messageDelegate.groupLeader || messageDelegate.groupExpanded)
                visible: active
                x: messageDelegate.groupNested ? messageDelegate.groupRailInset : 0
                width: parent.width - x
                height: item ? item.implicitHeight : 0
                sourceComponent: messageDelegate.subtype === "daemonGroup" ? remoteGroupComponent : activityComponent
                Component {
                    id: remoteGroupComponent
                    TranscriptRemoteGroup {
                        width: transcriptActivityLoader.width

                        host: messageDelegate.host
                        view: messageDelegate.view
                        groupKey: messageDelegate.groupKey
                        detailsData: messageDelegate.detailsData
                        detailsRef: messageDelegate.detailsRef
                    }
                }
                Component {
                    id: activityComponent
                    TranscriptActivityRow {
                        objectName: "transcriptActivityRow"
                        width: transcriptActivityLoader.width

                        host: messageDelegate.host
                        stateStore: messageDelegate.view
                        stateKey: JSON.stringify([messageDelegate.view.cacheScope, messageDelegate.messageKey])
                        messageKey: messageDelegate.messageKey
                        subtype: messageDelegate.subtype
                        eventType: messageDelegate.eventType
                        title: messageDelegate.title
                        summary: messageDelegate.summary
                        target: messageDelegate.target
                        status: messageDelegate.status
                        command: messageDelegate.command
                        cwd: messageDelegate.cwd
                        commandDescription: messageDelegate.commandDescription
                        fileLocation: messageDelegate.fileLocation
                        exitCode: messageDelegate.exitCode
                        durationMs: messageDelegate.durationMs
                        body: messageDelegate.body
                        detailsData: messageDelegate.detailsData
                        payloadData: messageDelegate.payloadData
                        detailsRef: messageDelegate.detailsRef
                        payloadRef: messageDelegate.payloadRef
                        fileStats: messageDelegate.fileStats
                        imageSources: messageDelegate.imageSources
                        running: messageDelegate.running
                        compact: messageDelegate.groupNested
                        onImageClicked: function (source) {
                            const all = images.map(image => String(image));
                            const index = all.indexOf(source.toString());
                            preview.showImages(index < 0 ? [source] : all, Math.max(0, index));
                        }
                    }
                }
            }

            TextMetrics {
                id: bodyMetrics
                objectName: "messageBodyMetrics"
                text: kind === "user" ? messageDelegate.displayBody : ""
                font: bodyText.font
            }

            Row {
                height: KetTheme.px(16, KetTheme.zoom)
                spacing: KetTheme.px(7, KetTheme.zoom)
                visible: !messageDelegate.noticeRow && !messageDelegate.activityRow && !messageDelegate.chunkContinuation && kind !== "user" && kind !== "assistant" && subtype !== "question" && subtype !== "approval"
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: KetTheme.px(15, KetTheme.zoom)
                    height: KetTheme.px(15, KetTheme.zoom)
                    name: KetTheme.iconForLucide(subtype === "compaction" ? "bot" : subtype === "webSearch" ? "search" : kind === "tool" ? "square-terminal" : kind === "file" ? "file-code" : "sparkles")
                    color: subtype === "compaction" ? KetTheme.t.textMuted : subtype === "webSearch" ? KetTheme.t.textMuted : kind === "tool" ? KetTheme.t.warning : kind === "file" ? KetTheme.t.tabActiveText : KetTheme.t.accentText
                    box: KetTheme.px(15, KetTheme.zoom)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: meta
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                    font.weight: Font.DemiBold
                }
            }

            Loader {
                active: subtype === "question" || subtype === "approval"
                visible: active
                width: parent.width
                height: item ? item.implicitHeight : 0
                sourceComponent: subtype === "approval" ? approvalCardComponent : messageDelegate.running ? questionCardComponent : closedQuestionComponent
            }

            // A question that is no longer open folds into the mock's note: what was asked
            // and, when it was answered, the answer.
            Component {
                id: closedQuestionComponent
                // The note keeps the mock's block spacing from the activity around it.
                Item {
                    implicitHeight: closedNote.implicitHeight + KetTheme.px(26, KetTheme.zoom)
                    TranscriptAgentNote {
                        id: closedNote
                        y: KetTheme.px(12, KetTheme.zoom)
                        width: parent.width
                        height: implicitHeight
                        objectName: "closedQuestionNote"
                        readonly property bool answered: Object.keys(messageDelegate.messageQuestionAnswers).length > 0
                        iconName: answered ? "check" : "clock"
                        iconColor: answered ? KetTheme.t.success : KetTheme.t.textMuted
                        label: answered ? "Answered" : messageDelegate.status === "deferred" ? "Deferred" : messageDelegate.status === "expired" ? "Question expired" : "Question closed"
                        details: messageDelegate.messageQuestions.map(question => {
                            const asked = question.question || question.header || "";
                            const answer = messageDelegate.messageQuestionAnswers[question.id];
                            const said = answer === undefined || answer === null ? "" : Array.isArray(answer) ? answer.join(", ") : typeof answer === "object" ? (answer.custom_answer || (answer.option_ids || []).join(", ")) : String(answer);
                            return said.length > 0 ? asked + "  ·  " + said : asked;
                        }).filter(line => line.length > 0).join("\n")
                        message: ""
                        state: messageDelegate.status
                    }
                }
            }

            Component {
                id: questionCardComponent
                AgentQuestionCard {
                    lifecycleStatus: messageDelegate.status
                    objectName: "messageQuestionCard"
                    questions: messageDelegate.messageQuestions
                    answerData: messageDelegate.messageQuestionAnswers
                    pending: messageDelegate.running
                    messageKey: messageDelegate.messageKey

                    initialDraftAnswers: shell.questionDraftFor(messageKey).answers || ({})
                    initialOtherQuestions: shell.questionDraftFor(messageKey).otherAnswers || ({})
                    initialQuestionIndex: shell.questionDraftFor(messageKey).questionIndex || 0
                    onDraftChanged: function (answers, questionIndex, otherAnswers) {
                        shell.saveQuestionDraft(messageKey, answers, questionIndex, otherAnswers);
                    }
                    onAnswersSubmitted: function (answers) {
                        shell.clearQuestionDraft(messageKey);
                        host.answerQuestion(messageKey, answers);
                    }
                    onDraftDiscarded: shell.clearQuestionDraft(messageKey)
                }
            }

            Component {
                id: approvalCardComponent
                AgentApprovalCard {
                    objectName: "messageApprovalCard"
                    prompt: messageDelegate.body
                    command: messageDelegate.command
                    tool: messageDelegate.target
                    status: messageDelegate.status
                    pending: messageDelegate.running
                    answerData: messageDelegate.messageQuestionAnswers
                    questions: messageDelegate.messageQuestions

                    onDecisionSubmitted: function (answers) {
                        host.answerQuestion(messageKey, answers);
                    }
                }
            }

            TextEdit {
                id: bodyText
                objectName: "messageBody"
                width: parent.width
                // Claim Copy and Select All before the Edit menu can: a read-only TextEdit does not,
                // so macOS fired the menu item and the selected text never reached the clipboard.
                Keys.onShortcutOverride: event => event.accepted = (event.matches(StandardKey.Copy) && selectedText.length > 0) || event.matches(StandardKey.SelectAll)
                // Export the exact selection; Qt rich fragments can prepend a list paragraph.
                Keys.onPressed: event => {
                    if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                        host.copySelectedText(selectedText);
                        event.accepted = true;
                    }
                }
                property int measuredHeightRevision: 0
                Connections {
                    target: messageDelegate.view
                    function onMeasuredHeightChanged(key) {
                        if (key === messageDelegate.messageKey)
                            ++bodyText.measuredHeightRevision;
                    }
                }
                // Fenced code of this block's Markdown source; copy buttons use it when it lines
                // up with the rendered code frames (indented code blocks do not get a button).
                readonly property real estimatedUnrenderedHeight: {
                    // Prefer the height this row measured at this width for
                    // this body revision; fall back to a glyph estimate.
                    void measuredHeightRevision;
                    const measured = view.measuredBodyHeight(messageKey, body.length, width);
                    if (measured > 0)
                        return measured;
                    const averageGlyphWidth = Math.max(6, font.pixelSize * 0.52);
                    const charactersPerLine = Math.max(20, Math.floor(width / averageGlyphWidth));
                    const wrappedLines = Math.max(estimatedBodyLines, Math.ceil(body.length / charactersPerLine));
                    return Math.max(font.pixelSize * 1.4, wrappedLines * font.pixelSize * 1.4);
                }
                // A non-zero estimate lets ListView skip directly to the
                // retained edge without constructing every rich document.
                // The exact implicit height replaces it when this block enters
                // the viewport; existing anchor repair absorbs that correction.
                readonly property real userCollapsedHeight: font.pixelSize * 1.75 * 20
                readonly property bool userOverlong: kind === "user" && implicitHeight > userCollapsedHeight + 1
                height: userOverlong && !messageDelegate.userExpanded ? Math.min(implicitHeight, userCollapsedHeight) : sourceInitialized ? Math.max(implicitHeight, font.pixelSize * 1.4) : estimatedUnrenderedHeight
                clip: userOverlong && !messageDelegate.userExpanded
                topPadding: codeBlocks.length > 0 && codeBlocks[0].startsDocument ? codeRecipe.padding : 0
                bottomPadding: codeBlocks.length > 0 && codeBlocks[codeBlocks.length - 1].endsDocument ? codeRecipe.padding : 0
                // Keep measured geometry for cached rows, but do not parse a
                // growing Markdown document outside the reading viewport.
                // Source stays authoritative in the message model; returning
                // to the row applies only the newest source and media revision.
                text: ""
                readonly property bool renderInViewport: view.visible && messageDelegate.y + messageDelegate.height >= view.contentY - 128 && messageDelegate.y <= view.contentY + view.height + 128
                // Live streaming tail must keep applying deltas even when the
                // new row briefly falls outside the viewport band after an
                // empty first paint; otherwise meta shows "· streaming" with
                // a frozen blank body until the delegate is recreated.
                readonly property bool renderNeeded: kind !== "assistant" || renderInViewport || selectionStart !== selectionEnd || (view.messageModel.materializing && index === view.messageModel.count - 1) || (host.sessionRunning && index === view.messageModel.count - 1)
                readonly property bool readingAnchorOwnsBody: kind === "assistant" && view.visible && !view.followTail && shell.transcriptAnchor !== null && shell.transcriptAnchor.key === messageKey
                property bool readerSourcePinned: false
                property bool renderedStreaming: false
                function pinReaderIfStreaming() {
                    // A stale streaming label must not freeze a completed row
                    // forever. Pin only while the owning session is actively
                    // producing output; once it completes, revisiting the row
                    // materializes the newest authoritative model source.
                    // Never pin an empty document — that freezes a blank body
                    // for the rest of the turn.
                    if (readingAnchorOwnsBody && host.sessionRunning && sourceInitialized && renderedSource.length > 0 && !readerSourcePinned) {
                        readerSourcePinned = true;
                        shell.traceTranscript("reader-pin", {
                            key: messageKey,
                            pinned: true
                        });
                    }
                }
                onReadingAnchorOwnsBodyChanged: {
                    if (readingAnchorOwnsBody) {
                        pinReaderIfStreaming();
                    } else {
                        if (readerSourcePinned)
                            shell.traceTranscript("reader-pin", {
                                key: messageKey,
                                pinned: false
                            });
                        readerSourcePinned = false;
                        refreshRenderedSource();
                    }
                }
                property bool sourceInitialized: false
                property bool selectionHeld: false
                onSelectedTextChanged: {
                    if (selectionHeld && selectionStart === selectionEnd) {
                        selectionHeld = false;
                        scheduleRenderedSourceRefresh();
                    }
                }
                property string renderedSource: ""
                property string renderedMessageKey: ""
                property bool renderedPartialBody: false
                property int renderedPrefixCharacters: 0
                property int renderedMermaidRevision: -1
                property bool paragraphFormatPending: false
                // Counts completed paragraph formats, so creation can tell whether
                // applying the first source already formatted the document.
                property int paragraphFormats: 0
                property bool applyingRenderedSource: false
                function scheduleRenderedSourceRefresh() {
                    // Restarting is important when an offscreen update already
                    // queued the zero-delay timer: a viewport transition in the
                    // same event-loop turn must move the callback behind the
                    // final ListView geometry, not inherit the stale callback.
                    sourceRefresh.restart();
                }
                Timer {
                    id: sourceRefresh
                    interval: 0
                    onTriggered: bodyText.refreshRenderedSource()
                }
                // Draws the document this row already shows again, with the current Mermaid modes.
                // A reader pinned to what they are reading keeps their text: a diagram arriving or
                // being switched changes how a fence looks, not which text is on screen.
                function rerenderHeldSource() {
                    if (!sourceInitialized)
                        return;
                    renderedMermaidRevision = host.mermaidRevision;
                    applyingRenderedSource = true;
                    try {
                        text = kind === "assistant" && !renderedPartialBody ? host.renderChatMarkdown(renderedSource, messageKey) : renderedSource;
                        if (paragraphFormatPending)
                            updateParagraphFormat();
                    } finally {
                        applyingRenderedSource = false;
                        rememberMeasuredHeight();
                    }
                }
                // How many times a Mermaid block of this row changed between source and diagram.
                property int renderedMermaidModes: 0
                function applyMermaidMode() {
                    ++renderedMermaidModes;
                    if (readerSourcePinned && selectionStart === selectionEnd)
                        rerenderHeldSource();
                    else
                        refreshRenderedSource(true);
                }
                function refreshRenderedSource(force) {
                    // A newly created/reused row needs one initial measurement;
                    // deferring that would introduce zero-height blank rows.
                    pinReaderIfStreaming();
                    const sameMessage = renderedMessageKey === messageKey;
                    const prefixChanged = sourceInitialized && sameMessage && renderedPrefixCharacters !== bodyPrefixCharacters;
                    if (!sourceInitialized && view.messageModel.materializing && index !== view.messageModel.count - 1 && !force)
                        return;
                    // A history page changes the geometry above the current
                    // reading anchor and must be materialized before anchor
                    // restoration, even when the delegate momentarily falls
                    // outside the viewport. Ordinary offscreen stream deltas
                    // remain deferred.
                    if (sourceInitialized && sameMessage && !renderNeeded && !prefixChanged && !force)
                        return;
                    // A diagram finishing under a pinned reader redraws the text they are
                    // reading; the newest source waits for them as before.
                    if (sourceInitialized && sameMessage && readerSourcePinned && selectionStart === selectionEnd && !prefixChanged && !force && renderedMermaidRevision !== host.mermaidRevision && renderedSource.toLowerCase().indexOf("mermaid") >= 0) {
                        rerenderHeldSource();
                        return;
                    }
                    // A closing fence/reference may reinterpret text *above* the
                    // reader. Retain that reader's displayed document through
                    // completion, until Latest or leaving this message releases
                    // the pin. Explicit older-prefix loads are exempt.
                    // An empty pinned document must still accept the first real
                    // body; otherwise "Codex · streaming" shows with no text.
                    if (sourceInitialized && sameMessage && readerSourcePinned && renderedSource.length > 0 && !prefixChanged && !force)
                        return;
                    // Replacing the document drops the reader's selection, so a
                    // streaming answer could not be selected and copied. Hold what
                    // is rendered while text is selected; clearing the selection
                    // applies the newest source.
                    if (sourceInitialized && sameMessage && selectionStart !== selectionEnd && !prefixChanged && !force) {
                        selectionHeld = true;
                        return;
                    }
                    const source = messageDelegate.displayBody;
                    const revision = host.mermaidRevision;
                    const forceRender = force === true;
                    const sourceChanged = !sourceInitialized || !sameMessage || renderedSource !== source || (renderedMermaidRevision !== revision && source.toLowerCase().indexOf("mermaid") >= 0) || forceRender || renderedPartialBody !== partialBody || renderedPrefixCharacters !== bodyPrefixCharacters;
                    if (sourceInitialized && !sameMessage)
                        deselect();
                    renderedSource = source;
                    renderedMessageKey = messageKey;
                    renderedPartialBody = partialBody;
                    renderedPrefixCharacters = bodyPrefixCharacters;
                    renderedStreaming = meta.endsWith(" · streaming");
                    renderedMermaidRevision = revision;
                    sourceInitialized = true;
                    // No reading anchor is captured around the render: the view holds the
                    // reader's row through this row's height change when it is reported.
                    applyingRenderedSource = true;
                    try {
                        if (sourceChanged) {
                            text = kind === "assistant" && !renderedPartialBody ? host.renderChatMarkdown(source, messageKey) : source;
                        }
                        if (paragraphFormatPending)
                            updateParagraphFormat();
                    } finally {
                        applyingRenderedSource = false;
                        rememberMeasuredHeight();
                    }
                }
                function rememberMeasuredHeight() {
                    if (!sourceInitialized || paragraphFormatPending || formattingParagraphs || renderedMessageKey !== messageKey || renderedSource !== messageDelegate.displayBody || width <= 0)
                        return;
                    view.rememberBodyHeight(messageKey, body.length, width, Math.max(implicitHeight, font.pixelSize * 1.4));
                }
                onImplicitHeightChanged: rememberMeasuredHeight()
                onRenderNeededChanged: if (renderNeeded)
                    scheduleRenderedSourceRefresh()
                Connections {
                    target: messageDelegate
                    // A single model update can change several roles. Read them
                    // together after QML has committed the complete role set.
                    function onDisplayBodyChanged() {
                        bodyText.scheduleRenderedSourceRefresh();
                    }
                    function onMessageKeyChanged() {
                        bodyText.scheduleRenderedSourceRefresh();
                    }
                    function onPartialBodyChanged() {
                        bodyText.scheduleRenderedSourceRefresh();
                    }
                    function onBodyPrefixCharactersChanged() {
                        bodyText.scheduleRenderedSourceRefresh();
                    }
                    function onMetaChanged() {
                        bodyText.pinReaderIfStreaming();
                    }
                }
                Connections {
                    target: view.messageModel
                    function onMaterializingChanged() {
                        if (!view.messageModel.materializing && bodyText.renderNeeded)
                            bodyText.scheduleRenderedSourceRefresh();
                    }
                }
                Connections {
                    target: view
                    function onViewportRevisionChanged() {
                        if (bodyText.renderNeeded)
                            bodyText.refreshRenderedSource();
                    }
                }
                visible: !messageDelegate.noticeRow && !messageDelegate.activityRow && subtype !== "question" && subtype !== "approval" && kind !== "file"
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                selectionColor: KetTheme.t.accent
                selectedTextColor: KetTheme.t.actionPrimaryInk
                color: messageSurface.agentMessage ? KetTheme.t.textMain : kind === "user" ? userRecipe.text : kind === "file" ? KetTheme.t.tabActiveText : KetTheme.t.textMain
                wrapMode: TextEdit.Wrap
                textFormat: kind === "assistant" && !renderedPartialBody ? TextEdit.MarkdownText : TextEdit.PlainText
                baseUrl: host.messageBaseUrl
                property bool formattingParagraphs: false
                property var mermaidPlaceholders: []
                // Frames of the Mermaid blocks, source or diagram, and the fence behind each code frame.
                property var mermaidBlocks: []
                property var codeFences: []
                property bool codeFencesIdentity: true
                property var codeBlocks: []
                property string lastActivatedLink: ""
                property double lastLinkActivationTime: 0
                function activateLink(link) {
                    const value = link.toString();
                    const now = Date.now();
                    if (value.length === 0 || (value === lastActivatedLink && now - lastLinkActivationTime < 250))
                        return;
                    lastActivatedLink = value;
                    lastLinkActivationTime = now;
                    host.openRelativeFile(value);
                }
                onContentHeightChanged: shell.traceTranscript("text-height", {
                    messageIndex: index,
                    textHeight: contentHeight
                })
                function updateParagraphFormat() {
                    if (formattingParagraphs)
                        return;
                    if (!sourceInitialized) {
                        paragraphFormatPending = true;
                        return;
                    }
                    if (sourceInitialized && !renderNeeded && !applyingRenderedSource) {
                        paragraphFormatPending = true;
                        return;
                    }
                    paragraphFormatPending = false;
                    shell.traceTranscript("format-start", {
                        messageIndex: index,
                        textHeight: contentHeight
                    });
                    formattingParagraphs = true;
                    try {
                        host.formatMessageDocument(textDocument, font.pixelSize, width);
                        codeBlocks = host.messageCodeBlocks(textDocument);
                        mermaidPlaceholders = host.mermaidPlaceholderPositions(textDocument);
                        const layout = host.mermaidLayout(textDocument, messageKey);
                        mermaidBlocks = layout.blocks;
                        codeFences = layout.codeFences;
                        codeFencesIdentity = layout.identity;
                    } finally {
                        formattingParagraphs = false;
                        ++paragraphFormats;
                        rememberMeasuredHeight();
                        shell.traceTranscript("format-end", {
                            messageIndex: index,
                            textHeight: contentHeight
                        });
                    }
                }
                Connections {
                    target: host
                    function onDocumentStyleChanged() {
                        // Appearance also updates the QML font/width bindings. Format
                        // after they settle so text and overlay geometry use one layout.
                        Qt.callLater(bodyText.updateParagraphFormat);
                    }
                    function onFileLinksChanged() {
                        bodyText.updateParagraphFormat();
                    }
                    function onMermaidChanged() {
                        bodyText.refreshRenderedSource();
                    }
                    function onSessionChanged() {
                        bodyText.pinReaderIfStreaming();
                    }
                }
                Component.onCompleted: {
                    // Applying the first source sets the text, whose change already
                    // formats the document. Formatting it again repeats the whole
                    // pass for every row a conversation switch creates.
                    const formats = paragraphFormats;
                    refreshRenderedSource();
                    if (paragraphFormats === formats)
                        updateParagraphFormat();
                }
                onTextChanged: updateParagraphFormat()
                onTextFormatChanged: updateParagraphFormat()
                onWidthChanged: updateParagraphFormat()
                Repeater {
                    model: bodyText.codeBlocks
                    delegate: Rectangle {
                        objectName: "messageCodeBlockFrame"
                        required property var modelData
                        x: modelData.x
                        y: modelData.y + bodyText.topPadding
                        width: modelData.width
                        height: modelData.height
                        z: -1
                        radius: codeRecipe.radius
                        color: codeRecipe.background
                        border.color: codeRecipe.border
                        border.width: 1
                    }
                }
                Repeater {
                    // A diagram's frame is not a text code block, so it is drawn here, under the text.
                    model: bodyText.mermaidBlocks
                    delegate: Rectangle {
                        objectName: "mermaidDiagramFrame"
                        required property var modelData
                        visible: modelData.rendered
                        x: modelData.x
                        y: modelData.y + bodyText.topPadding
                        width: modelData.width
                        height: modelData.height
                        z: -1
                        radius: codeRecipe.radius
                        color: codeRecipe.background
                        border.color: codeRecipe.border
                        border.width: 1
                    }
                }
                Repeater {
                    model: bodyText.mermaidBlocks
                    delegate: TranscriptMermaidSwitch {
                        id: mermaidSwitch
                        objectName: "mermaidSwitch"
                        required property var modelData

                        rendered: modelData.rendered
                        // Beside the copy button of a source block, in the corner of a diagram.
                        x: modelData.x + modelData.width - width - KetTheme.px(4, KetTheme.zoom) - (rendered ? 0 : KetTheme.px(30, KetTheme.zoom))
                        y: modelData.y + bodyText.topPadding + KetTheme.px(5, KetTheme.zoom)
                        z: 2
                        onModeRequested: rendered => {
                            // The row changes height under the reader's line, so the change runs
                            // under the reading anchor like any other presentation change.
                            messageDelegate.view.runAnchoredMutation(() => {
                                host.setMermaidMode(messageDelegate.messageKey, mermaidSwitch.modelData.ordinal, rendered);
                                bodyText.applyMermaidMode();
                            });
                        }
                    }
                }
                Loader {
                    active: messageDelegate.findCurrentRow && bodyText.visible
                    z: -0.5
                    sourceComponent: Rectangle {
                        id: findHighlight
                        objectName: "findHighlight"
                        readonly property var range: {
                            void bodyText.text;
                            void bodyText.width;
                            void messageDelegate.view.findCurrentStart;
                            return messageDelegate.view.findHighlightRange(bodyText, messageDelegate.body);
                        }
                        visible: range !== null
                        x: range ? range.x : 0
                        y: range ? range.y : 0
                        width: range ? range.width : 0
                        height: range ? range.height : 0
                        radius: KetTheme.px(2, KetTheme.zoom)
                        color: KetTheme.t.accentMuted
                    }
                }
                Repeater {
                    // Every code frame carries its Copy button, as the kit's code block does.
                    // Only the row still being written waits for its chrome, so streaming text
                    // is not re-scanned for fences on every chunk. Buttons appear when the
                    // fences line up with the rendered frames (indented code gets none).
                    model: messageDelegate.codeCopyShown && kind === "assistant" && bodyText.codeBlocks.length > 0 && (bodyText.codeFencesIdentity ? host.codeBlockSources(body).length === bodyText.codeBlocks.length : bodyText.codeFences.length === bodyText.codeBlocks.length) ? bodyText.codeBlocks : []
                    delegate: TranscriptCopyButton {
                        objectName: "copyCodeBlockButton"
                        required property var modelData
                        required property int index
                        x: modelData.x + modelData.width - width - KetTheme.px(4, KetTheme.zoom)
                        y: modelData.y + bodyText.topPadding + KetTheme.px(4, KetTheme.zoom)
                        z: 2
                        width: KetTheme.px(26, KetTheme.zoom)
                        height: KetTheme.px(26, KetTheme.zoom)

                        label: "Copy code"
                        padding: KetTheme.px(6, KetTheme.zoom)
                        idleColor: KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom).background
                        onActiveFocusChanged: messageDelegate.chromeFocusHold = activeFocus
                        onClicked: host.copyText(host.codeBlockSources(messageDelegate.body)[bodyText.codeFencesIdentity ? index : bodyText.codeFences[index]] || "")
                    }
                }
                PointHandler {
                    id: bodyPointHandler
                    acceptedButtons: Qt.LeftButton
                    property point pressPosition
                    onActiveChanged: {
                        if (active) {
                            pressPosition = point.position;
                            return;
                        }
                        if (Math.abs(point.position.x - pressPosition.x) > Qt.styleHints.startDragDistance || Math.abs(point.position.y - pressPosition.y) > Qt.styleHints.startDragDistance)
                            return;
                        const position = bodyText.positionAt(point.position.x, point.position.y);
                        let source = host.messageImageAt(bodyText.textDocument, position);
                        if (source.toString().length === 0)
                            source = host.messageImageAt(bodyText.textDocument, position - 1);
                        if (source.toString().length > 0) {
                            if (source.toString() === "qrc:/agent/mermaid-placeholder.svg")
                                return;
                            // A diagram opens in the image viewer, as in the mock.
                            const diagram = host.mermaidViewerImage(source);
                            if (diagram.source) {
                                preview.showItems([diagram], 0);
                                return;
                            }
                            preview.showImages([source], 0);
                            return;
                        }
                        const link = host.messageLinkAt(bodyText.textDocument, position);
                        if (link.length > 0)
                            bodyText.activateLink(link);
                    }
                }
                Repeater {
                    model: bodyText.mermaidPlaceholders
                    delegate: Rectangle {
                        objectName: "mermaidInstallOverlay"
                        required property var modelData
                        readonly property bool pending: modelData.pending
                        readonly property rect placement: {
                            const width = bodyText.width;
                            const text = bodyText.text;
                            return bodyText.positionToRectangle(modelData.position);
                        }
                        x: placement.x
                        y: placement.y
                        // The box the document reserved for the image, from the same place.
                        width: modelData.width
                        height: modelData.height
                        color: KetTheme.t.panelBgSubtle
                        border.color: KetTheme.t.panelBorder
                        radius: KetTheme.px(6, KetTheme.zoom)
                        Text {
                            objectName: "mermaidPendingLabel"
                            anchors.centerIn: parent
                            visible: parent.pending
                            text: "Loading..."
                            color: KetTheme.t.textSecondary
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                        }
                        Column {
                            anchors.centerIn: parent
                            width: parent.width - KetTheme.px(24, KetTheme.zoom)
                            spacing: KetTheme.px(8, KetTheme.zoom)
                            visible: !parent.pending
                            Text {
                                text: host.mermaidInstalling ? "Installing Mermaid..." : "Mermaid"
                                color: KetTheme.t.textSecondary
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                            }
                            Row {
                                width: parent.width
                                spacing: KetTheme.px(8, KetTheme.zoom)
                                TextEdit {
                                    objectName: "mermaidInstallCommand"
                                    // Claim Copy and Select All before the Edit menu can: a read-only TextEdit does not,
                                    // so macOS fired the menu item and the selected text never reached the clipboard.
                                    Keys.onShortcutOverride: event => event.accepted = (event.matches(StandardKey.Copy) && selectedText.length > 0) || event.matches(StandardKey.SelectAll)
                                    // Export the exact selection; Qt rich fragments can prepend a list paragraph.
                                    Keys.onPressed: event => {
                                        if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                                            host.copySelectedText(selectedText);
                                            event.accepted = true;
                                        }
                                    }
                                    width: parent.width - KetTheme.px(40, KetTheme.zoom)
                                    height: Math.max(KetTheme.px(32, KetTheme.zoom), contentHeight)
                                    verticalAlignment: TextEdit.AlignVCenter
                                    text: "npm install -g @mermaid-js/mermaid-cli"
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    color: KetTheme.t.textMain
                                    font.family: agentHost.monoFontFamily
                                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                                }
                                Basic.ToolButton {
                                    id: mermaidInstallButton
                                    objectName: "mermaidInstallButton"
                                    width: KetTheme.px(32, KetTheme.zoom)
                                    height: KetTheme.px(32, KetTheme.zoom)
                                    padding: KetTheme.px(7, KetTheme.zoom)
                                    enabled: !host.mermaidInstalling
                                    contentItem: Item {
                                        implicitWidth: KetTheme.px(16, KetTheme.zoom)
                                        implicitHeight: implicitWidth
                                        KetIcon {
                                            anchors.centerIn: parent
                                            box: Math.min(parent.width, parent.height)
                                            name: KetTheme.iconForLucide("play")
                                            color: parent.enabled ? KetTheme.t.textMain : KetTheme.t.textDisabled
                                        }
                                    }
                                    background: Rectangle {
                                        color: parent.hovered ? KetTheme.t.surfaceHover : "transparent"
                                        radius: KetTheme.px(4, KetTheme.zoom)
                                    }
                                    onClicked: host.installMermaid()
                                    Accessible.name: "Install Mermaid"
                                    KetToolTip {
                                        objectName: "ketToolTip"
                                        visible: mermaidInstallButton.hovered
                                        text: "Install Mermaid"
                                    }
                                }
                            }
                            Text {
                                width: parent.width
                                visible: host.mermaidInstallError.length > 0 && !host.mermaidInstalling
                                text: host.mermaidInstallError
                                color: KetTheme.t.warning
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                KetToolTip {
                                    objectName: "ketToolTip"
                                    visible: errorHover.hovered
                                    text: host.mermaidInstallError
                                }
                                HoverHandler {
                                    id: errorHover
                                }
                            }
                        }
                    }
                }
                onLinkActivated: function (link) {
                    activateLink(link);
                }
                MouseArea {
                    id: messageLinkHover
                    objectName: "messageBodyLinkHover"
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    hoverEnabled: true
                    readonly property string link: containsMouse && bodyText.sourceInitialized ? host.messageLinkAt(bodyText.textDocument, bodyText.positionAt(mouseX, mouseY)) : ""
                    cursorShape: link.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
                }
                font.family: kind === "tool" && subtype !== "compaction" ? agentHost.monoFontFamily : KetTheme.fontFamily
                font.pixelSize: kind === "tool" ? KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels : KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
            }

            Basic.Button {
                id: userMessageMore
                objectName: "userMessageShowMore"
                visible: bodyText.userOverlong
                text: messageDelegate.userExpanded ? "Show less" : "Show more"
                Accessible.name: text + " user message"
                implicitHeight: visible ? KetTheme.px(26, KetTheme.zoom) : 0
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                background: Item {}
                contentItem: Text {
                    text: userMessageMore.text
                    color: userRecipe.text
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                    font.underline: userMessageMore.hovered || userMessageMore.activeFocus
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    messageDelegate.userExpanded = !messageDelegate.userExpanded;
                    view.rememberActivityState(messageDelegate.userExpansionKey, {
                        expanded: messageDelegate.userExpanded
                    });
                }
            }

            Repeater {
                model: messageDelegate.atlasLinks || []
                delegate: AtlasResultCard {
                    required property string modelData
                    width: messageColumn.width
                    link: modelData
                    host: messageDelegate.host

                }
            }

            TranscriptMessageImages {
                row: messageDelegate
                sources: kind === "user" ? [] : messageDelegate.messageImages
            }

        }
    }
}
