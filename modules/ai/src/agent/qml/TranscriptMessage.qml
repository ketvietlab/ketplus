import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: messageDelegate
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
    readonly property real groupRailInset: Metrics.px(22, shell.t)
    readonly property int surfaceInset: activityRow ? 0 : Metrics.px(4, shell.t)
    onYChanged: {
        if (!view.followTail && shell.transcriptWheel.direction !== 0)
            host.scheduleTranscriptRestore();
        if (!shell.restoringTranscript && shell.transcriptAnchor && messageKey === shell.transcriptAnchor.key)
            host.scheduleTranscriptRestore();
    }

    required property bool running
    required property string category
    required property string eventType
    required property string phase
    required property string title
    required property string summary
    required property string target
    required property string status
    required property string cwd
    required property string commandDescription
    required property string fileLocation
    required property var exitCode
    required property var durationMs
    required property string detailsData
    required property string payloadData
    required property string detailsRef
    required property string payloadRef
    readonly property var messageImages: imageSources || []
    readonly property var messageQuestions: questionData || []
    readonly property var messageQuestionAnswers: questionAnswerData || ({})
    readonly property bool activityRow: category === "activity"
    readonly property bool commandRow: activityRow && subtype === "command"
    readonly property string displayBody: shell.displayMessageBody(kind, body)
    readonly property string userExpansionKey: JSON.stringify(["user-message", view.cacheScope, messageKey])
    property bool userExpanded: false
    function restoreUserExpansion() {
        userExpanded = view.activityState(userExpansionKey)?.expanded === true;
    }
    onUserExpansionKeyChanged: restoreUserExpansion()
    readonly property real contentColumnWidth: Math.min(width, shell.conversationColumnWidth)
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
    readonly property bool chromeActive: (rowHover.hovered || chromeFocusHold || (view.activeFocus && view.keyboardIndex === index))
    Loader {
        active: (kind === "user" ? messageDelegate.copyableMessage : messageDelegate.turnAnswered) && messageDelegate.chromeActive
        z: 2
        width: Metrics.px(28, shell.t)
        height: Metrics.px(28, shell.t)
        x: kind === "user" ? messageSurface.x - width - 4 : messageSurface.x
        y: kind === "user" ? messageSurface.y + 2 : messageSurface.y + messageSurface.height + Metrics.px(8, shell.t)
        sourceComponent: TranscriptCopyButton {
            objectName: "copyMessageButton"
            tokens: shell.t
            label: kind === "user" ? "Copy message" : "Copy response"
            padding: Metrics.px(7, shell.t)
            idleColor: shell.t.pageBackground
            cornerRadius: Metrics.px(5, shell.t)
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
            x: messageSurface.x - Metrics.px(6, shell.t)
            y: messageSurface.y - 3
            width: messageSurface.width + Metrics.px(12, shell.t)
            height: messageSurface.height + Metrics.px(6, shell.t)
            radius: Metrics.px(6, shell.t)
            color: "transparent"
            border.color: shell.t.focus
            border.width: 2
        }
    }
    readonly property bool findCurrentRow: view.findOpen && view.findCurrentKey.length > 0 && view.findCurrentKey === messageKey
    Loader {
        active: messageDelegate.findCurrentRow && messageDelegate.activityRow
        sourceComponent: Rectangle {
            objectName: "findActivityHighlight"
            x: messageSurface.x - Metrics.px(6, shell.t)
            y: messageSurface.y - 2
            width: messageSurface.width + Metrics.px(12, shell.t)
            height: messageSurface.height + Metrics.px(4, shell.t)
            radius: Metrics.px(6, shell.t)
            color: "transparent"
            border.color: shell.t.accent
            border.width: 1
        }
    }
    width: view.width
    height: messageSurface.y + messageSurface.height + surfaceInset + (activityRow ? Metrics.px(3, shell.t) : kind === "user" ? Metrics.px(25, shell.t) : copyableMessage ? Metrics.px(44, shell.t) : view.rowSpacing)
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
        x: messageSurface.x + Metrics.px(7, shell.t)
        y: messageDelegate.groupLeader ? messageSurface.y + messageColumn.y + transcriptActivityLoader.y : 0
        width: Math.max(1, Metrics.px(2, shell.t))
        height: Math.max(0, parent.height - y)
        color: shell.t.border
    }
    Rectangle {
        id: messageSurface
        objectName: "messageSurface"
        readonly property int horizontalPadding: messageDelegate.activityRow || kind === "assistant" ? 0 : Metrics.px(kind === "user" ? 12 : 18, shell.t)
        readonly property bool userMessage: kind === "user"
        // ADR 0027: another conversation's agent wrote this turn's opening message.
        readonly property bool agentMessage: kind === "user" && subtype === "agentMessage"
        readonly property real userMaximumWidth: Math.max(Metrics.px(160, shell.t), Math.min(messageDelegate.contentColumnWidth * 0.8, messageDelegate.contentColumnWidth - Metrics.px(63, shell.t)))
        x: kind === "user" ? messageDelegate.contentColumnX + messageDelegate.contentColumnWidth - width - Metrics.px(20, shell.t) : messageDelegate.contentColumnX + Metrics.px(20, shell.t)
        y: messageDelegate.surfaceInset
        width: kind === "user"
                   ? Math.min(userMaximumWidth, Math.max(Math.ceil(bodyMetrics.advanceWidth) + Metrics.px(10, shell.t),
                              agentMessage ? Math.ceil(agentOriginMetrics.advanceWidth) + Metrics.px(4, shell.t) : 0,
                              imageGallery.bubblePreferredWidth) + horizontalPadding * 2)
                   : messageDelegate.contentColumnWidth - Metrics.px(40, shell.t)
        height: messageColumn.implicitHeight + (messageDelegate.activityRow ? 0 : Metrics.px(16, shell.t))
        radius: kind === "user" ? Metrics.px(12, shell.t) : kind === "file" ? Metrics.px(7, shell.t) : 0
        color: agentMessage ? shell.t.surfaceRaised : kind === "user" ? Qt.darker(shell.t.accent, 1.35) : kind === "file" ? shell.t.surfaceRaised : "transparent"
        border.color: agentMessage ? shell.t.accent : kind === "file" ? shell.t.border : "transparent"

        Column {
            id: messageColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: messageSurface.horizontalPadding
            anchors.rightMargin: messageSurface.horizontalPadding
            anchors.verticalCenter: parent.verticalCenter
            spacing: kind === "file" ? Metrics.px(4, shell.t) : Metrics.px(7, shell.t)

            Text {
                id: agentOrigin
                objectName: "agentMessageOrigin"
                visible: messageSurface.agentMessage
                width: parent.width
                text: visible ? meta : ""
                color: shell.t.accent
                font.family: shell.t.uiFont
                font.pixelSize: shell.t.caption
                font.weight: Font.Medium
                elide: Text.ElideRight
                Accessible.name: text
            }
            TextMetrics {
                id: agentOriginMetrics
                text: agentOrigin.text
                font: agentOrigin.font
            }

            Loader {
                active: messageDelegate.groupLeader
                visible: active
                width: parent.width
                height: active ? Metrics.px(32, shell.t) : 0
                // One muted summary line followed by its chevron, as the mobile transcript
                // reads; the rows open under a rail below it.
                sourceComponent: Item {
                    id: groupHeader
                    objectName: "transcriptGroupHeader"
                    Text {
                        id: groupSummaryText
                        objectName: "transcriptGroupSummary"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - groupChevron.width - Metrics.px(36, shell.t))
                        text: messageDelegate.groupSummary + (messageDelegate.groupRunning ? " · Running" : "")
                        color: messageDelegate.groupFailed > 0 ? shell.t.danger : shell.t.textMuted
                        font.family: shell.t.uiFont
                        font.pixelSize: shell.t.body
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    TintedIcon {
                        id: groupChevron
                        objectName: "transcriptGroupChevron"
                        anchors.left: groupSummaryText.right
                        anchors.leftMargin: Metrics.px(4, shell.t)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Metrics.px(14, shell.t)
                        height: width
                        source: "qrc:/agent/icons/chevron-right.svg"
                        color: shell.t.textMuted
                        rotation: messageDelegate.groupExpanded ? 90 : 0
                    }
                    TapHandler {
                        onTapped: view.toggleGroup(messageDelegate.groupKey)
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
                        tokens: shell.t
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
                        tokens: shell.t
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
                            preview.imageSource = source;
                            preview.open();
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
                height: Metrics.px(16, shell.t)
                spacing: Metrics.px(7, shell.t)
                visible: !messageDelegate.activityRow && !messageDelegate.chunkContinuation && kind !== "user" && kind !== "assistant" && subtype !== "question" && subtype !== "approval"
                TintedIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Metrics.px(15, shell.t)
                    height: Metrics.px(15, shell.t)
                    source: subtype === "compaction" ? "qrc:/agent/icons/bot.svg" : subtype === "webSearch" ? "qrc:/agent/icons/search.svg" : kind === "tool" ? "qrc:/agent/icons/square-terminal.svg" : kind === "file" ? "qrc:/agent/icons/file-code.svg" : "qrc:/agent/icons/sparkles.svg"
                    color: subtype === "compaction" ? shell.t.textMuted : subtype === "webSearch" ? shell.t.textMuted : kind === "tool" ? shell.t.warning : kind === "file" ? shell.t.tabActiveText : shell.t.accent
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: meta
                    color: shell.t.textMuted
                    font.family: shell.t.uiFont
                    font.pixelSize: shell.t.caption
                    font.weight: Font.DemiBold
                }
            }

            Loader {
                active: subtype === "question" || subtype === "approval"
                visible: active
                width: parent.width
                height: item ? item.implicitHeight : 0
                sourceComponent: subtype === "approval" ? approvalCardComponent : questionCardComponent
            }

            Component {
                id: questionCardComponent
                AgentQuestionCard {
                objectName: "messageQuestionCard"
                questions: messageDelegate.messageQuestions
                answerData: messageDelegate.messageQuestionAnswers
                pending: messageDelegate.running
                messageKey: messageDelegate.messageKey
                tokens: shell.t
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
                    pending: messageDelegate.running
                    answerData: messageDelegate.messageQuestionAnswers
                    questions: messageDelegate.messageQuestions
                    tokens: shell.t
                    onDecisionSubmitted: function (answers) {
                        host.answerQuestion(messageKey, answers)
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
                height: userOverlong && !messageDelegate.userExpanded
                    ? Math.min(implicitHeight, userCollapsedHeight)
                    : sourceInitialized ? Math.max(implicitHeight, font.pixelSize * 1.4) : estimatedUnrenderedHeight
                clip: userOverlong && !messageDelegate.userExpanded
                topPadding: codeBlocks.length > 0 && codeBlocks[0].startsDocument ? 8 : 0
                bottomPadding: codeBlocks.length > 0 && codeBlocks[codeBlocks.length - 1].endsDocument ? 8 : 0
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
                    if (readingAnchorOwnsBody && host.sessionRunning
                            && sourceInitialized && renderedSource.length > 0 && !readerSourcePinned) {
                        readerSourcePinned = true;
                        shell.traceTranscript("reader-pin", {key: messageKey, pinned: true});
                    }
                }
                onReadingAnchorOwnsBodyChanged: {
                    if (readingAnchorOwnsBody) {
                        pinReaderIfStreaming();
                    } else {
                        if (readerSourcePinned)
                            shell.traceTranscript("reader-pin", {key: messageKey, pinned: false});
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
                    if (sourceInitialized && sameMessage && readerSourcePinned && selectionStart === selectionEnd
                            && !prefixChanged && !force && renderedMermaidRevision !== host.mermaidRevision
                            && renderedSource.toLowerCase().indexOf("mermaid") >= 0) {
                        rerenderHeldSource();
                        return;
                    }
                    // A closing fence/reference may reinterpret text *above* the
                    // reader. Retain that reader's displayed document through
                    // completion, until Latest or leaving this message releases
                    // the pin. Explicit older-prefix loads are exempt.
                    // An empty pinned document must still accept the first real
                    // body; otherwise "Codex · streaming" shows with no text.
                    if (sourceInitialized && sameMessage && readerSourcePinned
                            && renderedSource.length > 0 && !prefixChanged && !force)
                        return;
                    // Replacing the document drops the reader's selection, so a
                    // streaming answer could not be selected and copied. Hold what
                    // is rendered while text is selected; clearing the selection
                    // applies the newest source.
                    if (sourceInitialized && sameMessage && selectionStart !== selectionEnd
                            && !prefixChanged && !force) {
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
                visible: !messageDelegate.activityRow && subtype !== "question" && subtype !== "approval" && kind !== "file"
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                selectionColor: shell.t.accent
                selectedTextColor: "#ffffff"
                color: messageSurface.agentMessage ? shell.t.textMain : kind === "user" ? "#ffffff" : kind === "file" ? shell.t.tabActiveText : shell.t.textMain
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
                property var inlineCode: []
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
                        inlineCode = host.messageInlineCode(textDocument);
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
                    function onTokensChanged() {
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
                        radius: Metrics.px(6, shell.t)
                        color: shell.t.codeBlockBackground
                        border.color: shell.t.codeBlockBorder
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
                        radius: Metrics.px(6, shell.t)
                        color: shell.t.codeBlockBackground
                        border.color: shell.t.codeBlockBorder
                        border.width: 1
                    }
                }
                Repeater {
                    model: bodyText.mermaidBlocks
                    delegate: TranscriptMermaidSwitch {
                        id: mermaidSwitch
                        objectName: "mermaidSwitch"
                        required property var modelData
                        tokens: shell.t
                        rendered: modelData.rendered
                        // Beside the copy button of a source block, in the corner of a diagram.
                        x: modelData.x + modelData.width - width - Metrics.px(4, shell.t) - (rendered ? 0 : Metrics.px(30, shell.t))
                        y: modelData.y + bodyText.topPadding + Metrics.px(5, shell.t)
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
                Repeater {
                    model: bodyText.inlineCode
                    delegate: Rectangle {
                        objectName: "messageInlineCodeChip"
                        required property var modelData
                        x: modelData.x
                        y: modelData.y + bodyText.topPadding
                        width: modelData.width
                        height: modelData.height
                        z: -1
                        radius: Metrics.px(4, shell.t)
                        color: shell.t.inlineCodeBackground
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
                        radius: Metrics.px(2, shell.t)
                        color: shell.t.accentMuted
                    }
                }
                Repeater {
                    // Fenced code of this block's Markdown source is extracted only once the row
                    // shows its chrome; buttons appear when it lines up with the rendered code
                    // frames (indented code blocks do not get one).
                    model: messageDelegate.chromeActive && kind === "assistant" && bodyText.codeBlocks.length > 0 && (bodyText.codeFencesIdentity ? host.codeBlockSources(body).length === bodyText.codeBlocks.length : bodyText.codeFences.length === bodyText.codeBlocks.length) ? bodyText.codeBlocks : []
                    delegate: TranscriptCopyButton {
                        objectName: "copyCodeBlockButton"
                        required property var modelData
                        required property int index
                        x: modelData.x + modelData.width - width - Metrics.px(4, shell.t)
                        y: modelData.y + bodyText.topPadding + Metrics.px(4, shell.t)
                        z: 2
                        width: Metrics.px(26, shell.t)
                        height: Metrics.px(26, shell.t)
                        tokens: shell.t
                        label: "Copy code"
                        padding: Metrics.px(6, shell.t)
                        idleColor: shell.t.codeBlockBackground
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
                            if (host.openMermaidImage(source))
                                return;
                            preview.imageSource = source;
                            preview.open();
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
                        color: shell.t.panelSubtle
                        border.color: shell.t.border
                        radius: Metrics.px(6, shell.t)
                        Text {
                            objectName: "mermaidPendingLabel"
                            anchors.centerIn: parent
                            visible: parent.pending
                            text: "Loading..."
                            color: shell.t.textSecondary
                            font.family: shell.t.uiFont
                            font.pixelSize: shell.t.body
                        }
                        Column {
                            anchors.centerIn: parent
                            width: parent.width - Metrics.px(24, shell.t)
                            spacing: Metrics.px(8, shell.t)
                            visible: !parent.pending
                            Text {
                                text: host.mermaidInstalling ? "Installing Mermaid..." : "Mermaid"
                                color: shell.t.textSecondary
                                font.family: shell.t.uiFont
                                font.pixelSize: shell.t.body
                            }
                            Row {
                                width: parent.width
                                spacing: Metrics.px(8, shell.t)
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
                                    width: parent.width - Metrics.px(40, shell.t)
                                    height: Math.max(Metrics.px(32, shell.t), contentHeight)
                                    verticalAlignment: TextEdit.AlignVCenter
                                    text: "npm install -g @mermaid-js/mermaid-cli"
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    color: shell.t.textMain
                                    font.family: shell.t.monoFont
                                    font.pixelSize: shell.t.body
                                }
                                Basic.ToolButton {
                                    id: mermaidInstallButton
                                    objectName: "mermaidInstallButton"
                                    width: Metrics.px(32, shell.t)
                                    height: Metrics.px(32, shell.t)
                                    padding: Metrics.px(7, shell.t)
                                    enabled: !host.mermaidInstalling
                                    contentItem: TintedIcon {
                                        source: "qrc:/agent/icons/play.svg"
                                        color: parent.enabled ? shell.t.textMain : shell.t.textDisabled
                                    }
                                    background: Rectangle {
                                        color: parent.hovered ? shell.t.surfaceHover : "transparent"
                                        radius: Metrics.px(4, shell.t)
                                    }
                                    onClicked: host.installMermaid()
                                    Accessible.name: "Install Mermaid"
                                    KetToolTip {
                                        tokens: shell.t
                                        visible: mermaidInstallButton.hovered
                                        text: "Install Mermaid"
                                    }
                                }
                            }
                            Text {
                                width: parent.width
                                visible: host.mermaidInstallError.length > 0 && !host.mermaidInstalling
                                text: host.mermaidInstallError
                                color: shell.t.warning
                                font.family: shell.t.uiFont
                                font.pixelSize: shell.t.caption
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                KetToolTip {
                                    tokens: shell.t
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
                    readonly property string link: containsMouse && bodyText.sourceInitialized
                        ? host.messageLinkAt(bodyText.textDocument,
                                             bodyText.positionAt(mouseX, mouseY))
                        : ""
                    cursorShape: link.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
                }
                font.family: kind === "tool" && subtype !== "compaction" ? shell.t.monoFont : shell.t.uiFont
                font.pixelSize: kind === "tool" ? shell.t.body : shell.t.reading
            }

            Basic.Button {
                id: userMessageMore
                objectName: "userMessageShowMore"
                visible: bodyText.userOverlong
                text: messageDelegate.userExpanded ? "Show less" : "Show more"
                Accessible.name: text + " user message"
                implicitHeight: visible ? Metrics.px(26, shell.t) : 0
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                background: Item {}
                contentItem: Text {
                    text: userMessageMore.text
                    color: "#ffffff"
                    font.family: shell.t.uiFont
                    font.pixelSize: shell.t.body
                    font.underline: userMessageMore.hovered || userMessageMore.activeFocus
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    messageDelegate.userExpanded = !messageDelegate.userExpanded;
                    view.rememberActivityState(messageDelegate.userExpansionKey, {expanded: messageDelegate.userExpanded});
                }
            }

            MessageImageGallery {
                id: imageGallery
                objectName: "messageImageGallery"
                readonly property string expansionKey: JSON.stringify(["images", view.cacheScope, messageDelegate.messageKey])
                function restoreImages() {
                    const saved = view.activityState(expansionKey);
                    maximumVisible = saved === undefined ? 3 : saved.imageLimit;
                }
                onExpansionKeyChanged: restoreImages()
                Component.onCompleted: restoreImages()
                onExpanded: view.rememberActivityState(expansionKey, {imageLimit: maximumVisible})
                width: Math.min(parent.width, preferredWidth)
                sources: messageDelegate.messageImages
                tokens: shell.t
                host: messageDelegate.host
                visible: !messageDelegate.activityRow && imageCount > 0
                // Thumbnail rows have fixed geometry; adding/removing attachments
                // still preserves the reading anchor.
                onHeightChanged: {
                    if (view.followTail || shell.transcript !== view)
                        return;
                    host.scheduleTranscriptRestore();
                }
                onImageClicked: function (source) {
                    preview.imageSource = source;
                    preview.open();
                }
            }
        }
    }
}
