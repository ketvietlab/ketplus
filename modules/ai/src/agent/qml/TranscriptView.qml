import Ket
import Ket as Kit
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "TranscriptPosition.js" as TranscriptPosition

// The transcript positions its own rows. ListView placed rows it had never laid
// out by extrapolating an average height, so the extent was rewritten under the
// reader on every realization and scrolling up jerked backwards. Here every row
// sits at TranscriptRowGeometry.top(row), a sum of measured heights, and the
// reading position is held as a row and an offset into it (Telegram's
// ScrollTopState), so any height change moves contentY by exactly that change.
Flickable {
    id: transcript
    required property var shell
    required property var host
    required property var preview
    required property var settleTimer
    readonly property bool windowVisible: visible && Window.window !== null
        && Window.window.visible && Window.window.visibility !== Window.Minimized
    property bool measurementPending: true
    property var activityStates: ({})
    property var activityStateOrder: []
    function activityState(key) {
        return activityStates[key];
    }
    function rememberActivityState(key, value) {
        const states = Object.assign({}, activityStates);
        const order = activityStateOrder.filter(entry => entry !== key);
        order.push(key);
        states[key] = value;
        while (order.length > 256)
            delete states[order.shift()];
        activityStates = states;
        activityStateOrder = order;
    }

    // Activity group expansion lives in AgentMessageModel, because it decides which rows
    // exist: a collapsed group is one row, not a leader followed by zero-height members.
    // Bumped on every expansion change so bindings that call groupExpanded() re-evaluate;
    // the leader row itself receives no dataChanged when its members appear.
    property int groupExpansionRevision: 0
    function groupExpanded(key) {
        groupExpansionRevision;
        return !!messageModel && messageModel.groupExpanded(key);
    }
    // Runs a presentation change that alters row heights under the mutation anchor, so a reader
    // above, inside or below it keeps their position. Inside an outer transcript update the
    // outer update already owns the anchor.
    function runAnchoredMutation(mutation) {
        const anchored = shell.transcript === transcript && !shell.updatingTranscript;
        if (anchored)
            shell.beginTranscriptUpdate();
        try {
            mutation();
        } finally {
            if (anchored)
                shell.endTranscriptUpdate();
        }
    }
    function setGroupExpanded(key, expanded) {
        if (!key || !messageModel)
            return;
        runAnchoredMutation(() => messageModel.setGroupExpanded(key, expanded));
    }
    function toggleGroup(key) {
        setGroupExpanded(key, !groupExpanded(key));
    }
    // --- Keyboard navigation (U4) -------------------------------------------------------
    // Up/Down move between rows (a collapsed group is one row), Enter toggles a group or
    // activity details, Cmd+C copies the focused message.
    // The focus ring shows only after keyboard navigation, never after a click.
    property int keyboardIndex: -1
    property bool keyboardFocusVisible: false
    activeFocusOnTab: true
    Accessible.role: Accessible.List
    Accessible.name: "Conversation"
    onActiveFocusChanged: {
        if (!activeFocus)
            keyboardFocusVisible = false;
    }
    function moveKeyboardFocus(step) {
        if (!messageModel || messageModel.count === 0)
            return;
        let index = keyboardIndex;
        if (index < 0 || index >= messageModel.count)
            index = step > 0 ? Math.max(-1, indexAt(1, contentY + 1) - 1) : Math.max(0, indexAt(1, contentY + height - 1)) + 1;
        const count = messageModel.count;
        index += step;
        // A collapsed group is already one row, so only chunk continuations are skipped.
        while (index >= 0 && index < count) {
            if (!messageModel.get(index).chunkContinuation)
                break;
            index += step;
        }
        if (index < 0 || index >= messageModel.count)
            return;
        keyboardIndex = index;
        keyboardFocusVisible = true;
        shell.beginTranscriptInput();
        cancelFlick();
        followTail = false;
        streamingTailRequested = false;
        shell.restoringTranscript = true;
        forceLayout();
        positionViewAtIndex(index, ListView.Contain);
        shell.restoringTranscript = false;
        previousScrollY = contentY;
        shell.captureTranscriptWheelPosition();
        noteTranscriptInput();
    }
    // Accessible semantics of the delegate at `index` (-1: the list itself); used by tests.
    function accessibleInfo(index) {
        const item = index < 0 ? transcript : itemAtIndex(index);
        return item ? {
            role: item.Accessible.role,
            name: item.Accessible.name,
            description: item.Accessible.description
        } : ({});
    }
    function activateKeyboardRow() {
        if (keyboardIndex < 0 || !messageModel)
            return;
        const row = messageModel.get(keyboardIndex);
        if (row.groupRole === "leader") {
            toggleGroup(row.groupKey);
            return;
        }
        const item = itemAtIndex(keyboardIndex);
        if (item && item.toggleActivityDetails)
            item.toggleActivityDetails();
    }
    // Cmd+C on a keyboard-focused row is the transcript's, not the Edit menu's.
    Keys.onShortcutOverride: event => event.accepted = event.matches(StandardKey.Copy) && keyboardIndex >= 0
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
            moveKeyboardFocus(event.key === Qt.Key_Down ? 1 : -1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            activateKeyboardRow();
            event.accepted = true;
        } else if (event.matches(StandardKey.Copy) && keyboardIndex >= 0) {
            copyMessage(keyboardIndex);
            event.accepted = true;
        }
    }
    TapHandler {
        objectName: "transcriptContextTap"
        acceptedButtons: Qt.RightButton
        // Rows with their own right-click menu (changed files) take the click first.
        onTapped: function(eventPoint) {
            const focused = transcript.Window.activeFocusItem;
            transcriptContextMenu.selectionItem = focused && typeof focused.selectedText === "string" && focused.selectedText.length > 0 ? focused : null;
            transcriptContextMenu.fileTarget = "";
            const pos = eventPoint.position;
            const row = transcript.itemAtIndex(transcript.indexAt(pos.x, pos.y + transcript.contentY));
            const body = row ? row.readingTextItem : null;
            if (body && body.visible) {
                const local = body.mapFromItem(transcript, pos.x, pos.y);
                if (local.x >= 0 && local.y >= 0 && local.x < body.width && local.y < body.height) {
                    const href = transcript.host.messageLinkAt(body.textDocument, body.positionAt(local.x, local.y));
                    transcriptContextMenu.fileTarget = transcript.host.revealableFilePath(href);
                }
            }
            transcriptContextMenu.popup();
        }
    }
    TranscriptContextMenu {
        id: transcriptContextMenu
        parent: transcript
        host: transcript.host

        // While a turn runs only the loaded rows are copied; otherwise the whole
        // conversation is read from history, which can be cancelled.
        onCopyConversationRequested: transcript.host.historyState.copying ? transcript.host.cancelConversationCopy() : transcript.host.sessionRunning ? transcript.host.copyText(transcript.shell.conversationCopyText()) : transcript.host.copyConversation(transcript.shell.conversationCopyText())
    }
    TapHandler {
        // Pointer interaction takes focus without showing the keyboard focus ring.
        gesturePolicy: TapHandler.DragThreshold
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        onTapped: {
            transcript.keyboardFocusVisible = false;
            const index = transcript.indexAt(point.position.x, point.position.y + transcript.contentY);
            if (index >= 0)
                transcript.keyboardIndex = index;
        }
    }

    // --- New messages while reading history (U5) ------------------------------------------
    // Counts user/assistant messages and activity groups (once per stable group) appended at the
    // live end after the reader left the tail. Streamed deltas, group members and rows delivered
    // by a newer-history page do not count.
    property int newMessageCount: 0
    // messageKey of each counted row -> counting unit (its own key, or its stable group key).
    property var newMessageUnits: ({})
    // Counted activity rows, whose unit changes when regrouping merges or splits them.
    property var newActivityKeys: ({})
    // Set while a completed newer-history page may still be inserting its rows.
    property bool newerPageLanding: false
    onFollowTailChanged: {
        if (followTail)
            resetNewMessageCount();
        shell.traceTranscript("follow-tail", {});
        reportReadingKey();
    }
    function resetNewMessageCount() {
        newMessageCount = 0;
        newMessageUnits = ({});
        newActivityKeys = ({});
    }
    function clearNewerPageLanding() {
        newerPageLanding = false;
    }
    // Qt.callLater coalesces calls to one function (keeping only the last arguments), so
    // appended ranges accumulate here and are counted once grouping roles have settled.
    property int pendingNewFirst: -1
    property int pendingNewLast: -1
    function queueNewRows(first, last) {
        pendingNewFirst = pendingNewFirst < 0 ? first : Math.min(pendingNewFirst, first);
        pendingNewLast = Math.max(pendingNewLast, last);
        Qt.callLater(transcript.flushNewRows);
    }
    function flushNewRows() {
        const first = pendingNewFirst;
        const last = pendingNewLast;
        pendingNewFirst = -1;
        pendingNewLast = -1;
        if (first >= 0)
            countNewRows(first, last);
    }
    function newMessageUnit(row) {
        return row.category === "activity" && row.groupKey ? row.groupKey : row.messageKey;
    }
    function countNewRows(first, last) {
        if (followTail || !messageModel)
            return;
        for (let index = first; index <= last && index < messageModel.count; ++index) {
            const row = messageModel.get(index);
            if (!row.messageKey || row.chunkContinuation || newMessageUnits[row.messageKey] !== undefined)
                continue;
            const activity = row.category === "activity";
            if (!(activity || row.category === "user" || row.category === "final" || row.category === "commentary" || row.kind === "assistant"))
                continue;
            newMessageUnits[row.messageKey] = newMessageUnit(row);
            if (activity)
                newActivityKeys[row.messageKey] = true;
        }
        recountNewMessages();
    }
    // Regrouping can merge counted activity rows into one group (or split one apart).
    function refreshNewMessageUnits() {
        if (followTail || !messageModel || newMessageCount === 0)
            return;
        let changed = false;
        for (const key in newActivityKeys) {
            // By rendered row, not by model row: merging rows into a group is
            // exactly what withholds them, so the rows whose unit just changed
            // are the ones a model-row lookup can no longer find.
            const row = messageModel.rowForKey(key);
            if (!row.messageKey)
                continue;
            const unit = newMessageUnit(row);
            if (unit !== newMessageUnits[key]) {
                newMessageUnits[key] = unit;
                changed = true;
            }
        }
        if (changed)
            recountNewMessages();
    }
    function recountNewMessages() {
        const seen = {};
        let total = 0;
        for (const key in newMessageUnits) {
            const unit = newMessageUnits[key];
            if (seen[unit])
                continue;
            seen[unit] = true;
            ++total;
        }
        newMessageCount = total;
    }
    Connections {
        target: transcript.messageModel
        ignoreUnknownSignals: true
        function onRowsInserted(parent, first, last) {
            // Only rows appended at the live end are new: prepended history pages and rows of a
            // newer-history page are not, even when hasNewer already flipped false.
            const newerPage = transcript.newerPageLanding || (transcript.historyRequestPending && transcript.historyRequestDirection === "newer");
            if (last === transcript.messageModel.count - 1 && first > 0 && !newerPage && !transcript.host.historyState.hasNewer)
                transcript.queueNewRows(first, last);
        }
        function onDataChanged() {
            Qt.callLater(transcript.refreshNewMessageUnits);
        }
        function onModelReset() {
            transcript.resetNewMessageCount();
            transcript.keyboardIndex = -1;
        }
        function onGroupExpansionChanged() {
            ++transcript.groupExpansionRevision;
        }
    }

    // --- Find in transcript -------------------------------------------------------------
    // Searches the source text of loaded rows (collapsed group members included). Searching
    // backwards past the first loaded match pages older history in bursts of at most
    // findOlderPageLimit pages; pressing Previous again continues with another burst.
    property bool findOpen: false
    property string findQuery: ""
    property var findResults: []
    property int findCurrent: -1
    property string findCurrentKey: ""
    property int findCurrentStart: -1
    property string findStatus: ""
    property bool findSearchingOlder: false
    readonly property int findOlderPageLimit: 10
    property int findOlderPagesLoaded: 0
    property int findOlderPagesSearched: 0
    property var findOlderAnchor: null
    property string findOlderEdgeKey: ""
    property int findRequestToken: -1
    // Set when the state update completing findRequestToken arrived.
    property bool findRequestCompleted: false
    property int findRetryAttempts: 0
    readonly property string findCountText: findQuery.length === 0 ? "" : findResults.length === 0 ? "No results" : (findCurrent + 1) + " / " + findResults.length
    signal findOpened
    Timer {
        id: findRetryTimer
        interval: 100
        onTriggered: transcript.requestOlderForFind()
    }
    // Live refresh of loaded results: coalesced to one refresh per event-loop turn, and to at
    // most one every 150 ms while a turn streams.
    Timer {
        id: findRefreshTimer
        interval: transcript.host.sessionRunning ? 150 : 0
        onTriggered: transcript.refreshFindResultsLive()
    }
    Connections {
        target: transcript.messageModel
        ignoreUnknownSignals: true
        function onCountChanged() {
            transcript.scheduleFindRefresh();
        }
        function onDataChanged() {
            transcript.scheduleFindRefresh();
        }
        function onModelReset() {
            transcript.scheduleFindRefresh();
        }
    }
    function scheduleFindRefresh() {
        if (findOpen && findQuery.length > 0 && !findSearchingOlder && !findRefreshTimer.running)
            findRefreshTimer.start();
    }
    function refreshFindResultsLive() {
        if (!findOpen || findQuery.length === 0 || findSearchingOlder)
            return;
        refreshFindResults();
        if (findCurrent < 0) {
            findCurrentKey = "";
            findCurrentStart = -1;
        }
    }
    function openFind() {
        findOpen = true;
        findOpened();
    }
    function closeFind() {
        findOpen = false;
        findQuery = "";
        findResults = [];
        findCurrent = -1;
        findCurrentKey = "";
        findCurrentStart = -1;
        findSearchingOlder = false;
        findRequestToken = -1;
        findRequestCompleted = false;
        findStatus = "";
        findRetryTimer.stop();
        forceActiveFocus();
    }
    function refreshFindResults() {
        const previous = findCurrent >= 0 && findCurrent < findResults.length ? findResults[findCurrent] : null;
        findResults = messageModel && findQuery.length > 0 ? messageModel.findMatches(findQuery) : [];
        findCurrent = previous ? findResults.findIndex(match => match.key === previous.key && match.start === previous.start) : -1;
    }
    function setFindQuery(query) {
        findQuery = query;
        findStatus = "";
        findSearchingOlder = false;
        findRetryTimer.stop();
        findCurrent = -1;
        refreshFindResults();
        if (findResults.length === 0) {
            findCurrentKey = "";
            return;
        }
        // Start at the first match at or below the reading position.
        const top = indexAt(1, contentY + 1);
        let first = findResults.findIndex(match => match.row >= top);
        gotoFindResult(first >= 0 ? first : 0);
    }
    function findNext() {
        if (findResults.length === 0)
            return;
        gotoFindResult((findCurrent + 1) % findResults.length);
    }
    function findPrevious() {
        if (findSearchingOlder || findQuery.length === 0)
            return;
        if (findCurrent > 0) {
            gotoFindResult(findCurrent - 1);
            return;
        }
        if (host.historyState.hasOlder && !host.historyState.error) {
            findOlderAnchor = findResults.length > 0 ? findResults[0] : null;
            findOlderEdgeKey = historyEdgeKey("older");
            findOlderPagesLoaded = 0;
            findRetryAttempts = 0;
            findRequestToken = -1;
            findRequestCompleted = false;
            findSearchingOlder = true;
            findStatus = "Searching older history…";
            requestOlderForFind();
            return;
        }
        if (findResults.length > 0)
            gotoFindResult(findResults.length - 1);
    }
    function requestOlderForFind() {
        if (!findSearchingOlder)
            return;
        if (historyRequestPending)
            return; // Its completion continues the search.
        if (!historyRequestAllowed()) {
            if (++findRetryAttempts > 100) {
                findSearchingOlder = false;
                findStatus = "Could not load older history";
            } else {
                findRetryTimer.restart();
            }
            return;
        }
        requestHistoryPage("older");
        findRequestToken = historyRequestToken;
    }
    // Called (deferred) for every settled history state update while searching older history.
    // Only the completion of the find's own request, or a page that moved the older edge (a
    // prefetch that raced it), counts as a searched page; unrelated updates count nothing.
    function resumeOlderFindSearch() {
        if (!findSearchingOlder || historyRequestPending)
            return; // The in-flight request's completion resumes the search.
        const completed = findRequestCompleted;
        findRequestCompleted = false;
        if (completed || historyEdgeKey("older") !== findOlderEdgeKey) {
            continueOlderFindSearch();
            return;
        }
        // No request of ours is in flight (never issued, declined or dropped): issue it now
        // unless the retry timer already will.
        findRequestToken = -1;
        if (!findRetryTimer.running)
            requestOlderForFind();
    }
    function continueOlderFindSearch() {
        // Any completed older page (ours or a prefetch that raced it) is searched.
        if (!findSearchingOlder || historyRequestPending)
            return;
        findRequestToken = -1;
        ++findOlderPagesLoaded;
        ++findOlderPagesSearched;
        findCurrent = -1;
        refreshFindResults();
        const anchor = findOlderAnchor;
        const firstOld = anchor ? findResults.findIndex(match => match.key === anchor.key && match.start === anchor.start) : findResults.length;
        if (anchor && firstOld < 0 && findResults.length > 0) {
            // The anchor match was trimmed from the newer end meanwhile: every loaded match is
            // older than it, so the nearest earlier match is the last one.
            findSearchingOlder = false;
            findStatus = "";
            gotoFindResult(findResults.length - 1);
            return;
        }
        if (firstOld > 0) {
            findSearchingOlder = false;
            findStatus = "";
            gotoFindResult(firstOld - 1);
            return;
        }
        const edge = historyEdgeKey("older");
        const edgeMoved = edge !== findOlderEdgeKey;
        findOlderEdgeKey = edge;
        if (host.historyState.error || !host.historyState.hasOlder || !edgeMoved) {
            findSearchingOlder = false;
            findStatus = findResults.length > 0 ? "No earlier matches" : "No matches in this conversation";
            if (findResults.length > 0)
                gotoFindResult(findResults.length - 1);
            return;
        }
        if (findOlderPagesLoaded >= findOlderPageLimit) {
            findSearchingOlder = false;
            findStatus = "No match in " + findOlderPageLimit + " older pages · Previous searches further";
            if (anchor) {
                const current = findResults.findIndex(match => match.key === anchor.key && match.start === anchor.start);
                findCurrent = current;
            }
            return;
        }
        requestOlderForFind();
    }
    function gotoFindResult(resultIndex) {
        const match = findResults[resultIndex];
        if (!match || !messageModel)
            return;
        findCurrent = resultIndex;
        findCurrentKey = match.key;
        findCurrentStart = match.start;
        if (match.groupKey)
            setGroupExpanded(match.groupKey, true);
        let row = messageModel.indexOfKey(match.key);
        if (row < 0)
            row = match.row;
        if (row < 0)
            return;
        // Programmatic navigation is explicit reading intent: stop tail following and re-anchor.
        shell.beginTranscriptInput();
        cancelFlick();
        followTail = false;
        streamingTailRequested = false;
        shell.restoringTranscript = true;
        for (let pass = 0; pass < 2; ++pass) {
            forceLayout();
            positionViewAtIndex(row, ListView.Center);
        }
        shell.restoringTranscript = false;
        previousScrollY = contentY;
        shell.captureTranscriptWheelPosition();
        noteTranscriptInput();
    }
    // Rendered-text span of the current match inside `textItem` (the n-th occurrence in the
    // source maps to the n-th occurrence in the rendered text; falls back to the first).
    function findHighlightRange(textItem, source) {
        if (!findOpen || findQuery.length === 0 || !textItem || textItem.length === 0)
            return null;
        const needle = findQuery.toLowerCase();
        const lowerSource = String(source).toLowerCase();
        let ordinal = 0;
        for (let at = lowerSource.indexOf(needle); at >= 0 && at < findCurrentStart; at = lowerSource.indexOf(needle, at + needle.length))
            ++ordinal;
        const rendered = textItem.getText(0, textItem.length).toLowerCase();
        let position = rendered.indexOf(needle);
        let first = position;
        for (let skipped = 0; position >= 0 && skipped < ordinal; ++skipped)
            position = rendered.indexOf(needle, position + needle.length);
        if (position < 0)
            position = first;
        if (position < 0)
            return null;
        const start = textItem.positionToRectangle(position);
        const end = textItem.positionToRectangle(position + needle.length);
        const sameLine = Math.abs(end.y - start.y) < 1;
        return {
            x: start.x,
            y: start.y,
            width: Math.max(KetTheme.px(4, KetTheme.zoom), sameLine ? end.x - start.x : textItem.width - start.x),
            height: start.height
        };
    }

    // Source text for copying the message at `index` (chunked messages copy their whole source).
    function messageCopyText(index) {
        return messageModel ? host.entryCopyText(messageModel.sourceForRow(index)) : "";
    }
    // Every row of the activity group led by `index`.
    function groupCopyText(index) {
        if (!messageModel)
            return "";
        const leader = messageModel.get(index);
        const parts = [];
        for (let row = index; row < messageModel.count && row < index + Math.max(1, leader.groupSize); ++row)
            parts.push(messageCopyText(row));
        return parts.filter(part => part.length > 0).join("\n\n");
    }
    function answerRow(entry) {
        return entry && entry.kind === "assistant" && entry.category !== "activity" && entry.subtype !== "question" && entry.subtype !== "approval";
    }
    // Where a turn's answer ends: 0 inside a turn, 1 the last agent text before the next
    // request or its turn's end line, 2 the last agent text of the transcript (its turn may
    // still be running).
    // `count` only makes bindings re-evaluate as rows arrive.
    function answerEndState(index, count) {
        if (!messageModel || !answerRow(messageModel.get(index)))
            return 0;
        for (let row = index + 1; row < messageModel.count; ++row) {
            const entry = messageModel.get(row);
            // A steer joins the running turn; the answer goes on under it.
            if (entry.kind === "user" && entry.eventType === "turn.steered")
                continue;
            if (entry.kind === "user" || entry.subtype === "turnEnd")
                return 1;
            if (answerRow(entry))
                return 0;
        }
        return 2;
    }
    // Whether the turn of the answer at `index` was stopped before it finished: its end row
    // is neither completed nor failed. `count` only makes bindings re-evaluate.
    function answerStopped(index, count) {
        if (!messageModel)
            return false;
        for (let row = index + 1; row < messageModel.count; ++row) {
            const entry = messageModel.get(row);
            if (entry.subtype === "turnEnd")
                return entry.status !== "completed" && entry.status !== "failed";
            if (entry.kind === "user" && entry.eventType !== "turn.steered")
                return false;
        }
        return false;
    }
    // Everything the agent wrote in the turn ending at `index`, without its commands,
    // files or tool steps.
    function answerCopyText(index) {
        if (!messageModel)
            return "";
        const parts = [];
        for (let row = index; row >= 0; --row) {
            const entry = messageModel.get(row);
            // A steer joined the turn: what the agent wrote before it is the same answer.
            if (entry.kind === "user" && entry.eventType === "turn.steered")
                continue;
            if (entry.kind === "user")
                break;
            if (answerRow(entry) && !entry.chunkContinuation)
                parts.unshift(messageCopyText(row));
        }
        return parts.filter(part => part.length > 0).join("\n\n");
    }
    function copyMessage(index) {
        const row = messageModel ? messageModel.get(index) : null;
        if (!row)
            return;
        host.copyText(row.groupRole === "leader" ? groupCopyText(index) : messageCopyText(index));
    }
    // Every model row is on screen now: a collapsed group contributes its leader only.
    function visibleRowIndex(index) {
        return index;
    }

    objectName: "cachedAgentTranscript"
    anchors.fill: parent
    visible: false
    enabled: false
    opacity: shell.settlingTranscript && shell.transcript === transcript ? 0 : 1
    // Use only journal timestamps. Older records without dates never invent a day.
    function dayLabel(index, timestamp, count) {
        const date = new Date(timestamp);
        if (!timestamp || isNaN(date.getTime())) return "";
        for (let previous = index - 1; previous >= 0; --previous) {
            const entry = messageModel.get(previous);
            if (!entry.timestamp) continue;
            const before = new Date(entry.timestamp);
            if (isNaN(before.getTime())) continue;
            if (before.toDateString() === date.toDateString()) return "";
            break;
        }
        const today = new Date();
        const yesterday = new Date(today.getFullYear(), today.getMonth(), today.getDate() - 1);
        if (date.toDateString() === today.toDateString()) return qsTr("Today");
        if (date.toDateString() === yesterday.toDateString()) return qsTr("Yesterday");
        // The mock's divider reads "Thursday, 24 September"; other years add theirs.
        const format = date.getFullYear() === today.getFullYear() ? "dddd, d MMMM" : "dddd, d MMMM yyyy";
        return date.toLocaleDateString(Qt.locale("en_GB"), format);
    }
    property string cacheScope: ""
    property var messageModel: null
    property alias wheelController: transcriptWheel
    property alias verticalScrollBar: transcriptScrollBar
    property bool followTail: true
    readonly property string scrollState: followTail ? "FollowingLatest" : "ReadingHistory"
    property bool streamingViewportPrepared: false
    property bool streamingTailRequested: false
    property real previousScrollY: 0
    // Explicit viewport epoch for virtualized delegates. ListView can move an
    // already-instantiated delegate without re-evaluating every nested binding
    // in the same polish pass; the epoch gives visible rich-text rows a bounded
    // post-movement opportunity to materialize their newest model source.
    property int viewportRevision: 0

    // Distance-based history prefetch. A page is requested whenever the
    // rendered buffer beyond the viewport edge is shorter than the threshold,
    // regardless of input direction (wheel, drag, momentum, scrollbar, or a
    // prepend that left the buffer short). One request is in flight at a time;
    // every completion re-evaluates once layout settles.
    // Six viewports of loaded content are kept beyond each edge of the view.
    readonly property int bufferViewports: 6
    readonly property real prefetchThreshold: Math.max(bufferViewports * height, 3000)
    property bool historyRequestPending: false
    property string historyRequestDirection: ""
    property string historyRequestEdgeKey: ""
    // Opposite edge at request time: a completion that changed it trimmed that edge.
    property string historyRequestOppositeKey: ""
    // Token echoed by the host as historyState.requestToken; only that update completes
    // the pending request, so an unrelated state update can never back a direction off.
    property int historyRequestToken: 0
    // A completion that did not move the requested edge (zero new rows) backs
    // that direction off until fresh physical input, preventing request storms.
    property bool olderPrefetchBackoff: false
    property bool newerPrefetchBackoff: false
    // A page that trimmed an edge blocks automatic requests toward that edge until fresh
    // input (wheel, drag, scrollbar, keyboard/programmatic navigation, Jump to latest);
    // otherwise compact rows in a tall viewport ping-pong older/newer pages forever.
    property bool olderTrimBlocked: false
    property bool newerTrimBlocked: false
    property bool idlePrefetchDone: false
    // Measurement/test hooks.
    property int olderPageRequests: 0
    property int newerPageRequests: 0
    property int topHardStops: 0

    Timer {
        id: historyRequestWatchdog
        // The host may decline a request without publishing state (for
        // example while attaching); never leave pagination wedged.
        interval: 10000
        onTriggered: transcript.historyRequestPending = false
    }
    Timer {
        id: historyPrefetchSettle
        interval: 32
        onTriggered: transcript.evaluateHistoryPrefetch()
    }
    Timer {
        id: idlePrefetchTimer
        interval: 500
        onTriggered: transcript.runIdlePrefetch()
    }
    Connections {
        target: transcript.messageModel
        ignoreUnknownSignals: true
        function onCountChanged() {
            historyPrefetchSettle.restart();
            Qt.callLater(transcript.evaluateHistoryPrefetch);
            if (!transcript.idlePrefetchDone)
                idlePrefetchTimer.restart();
        }
    }
    Connections {
        target: transcript.host
        function onHistoryStateChanged() {
            transcript.historyStateUpdated();
        }
        function onSessionChanged() {
            // Idle prefetch waits for the running turn; re-arm once it completes.
            if (!transcript.host.sessionRunning && !transcript.idlePrefetchDone)
                idlePrefetchTimer.restart();
        }
    }
    onMessageModelChanged: {
        measurementPending = true;
        // Another conversation: its session has not heard this view's key.
        lastReportedReadingKey = "\u0001";
        idlePrefetchDone = false;
        olderPrefetchBackoff = false;
        newerPrefetchBackoff = false;
        olderTrimBlocked = false;
        newerTrimBlocked = false;
        historyRequestPending = false;
        idlePrefetchTimer.restart();
        resetNewMessageCount();
    }

    readonly property real tailFollowBuffer: 32
    function nearTail() {
        const end = originY + Math.max(0, contentHeight - height);
        return contentY >= end - tailFollowBuffer;
    }
    // The bottom of a trimmed historical window is not the live tail when
    // newer pages can be appended in place.
    // A trimmed window is never the live tail, even when newer rows cannot be paged in
    // place (Jump to latest reloads the newest window).
    function liveTail() {
        return nearTail() && !host.historyState.hasNewer;
    }
    function contentAbove() {
        return contentY - originY;
    }
    function contentBelow() {
        return originY + contentHeight - contentY - height;
    }
    function historyEdgeKey(direction) {
        if (!messageModel || messageModel.count === 0)
            return "";
        const row = messageModel.get(direction === "older" ? 0 : messageModel.count - 1);
        return row && row.messageKey ? row.messageKey : "";
    }
    function averageRenderedRowHeight() {
        let total = 0;
        let rows = 0;
        for (const child of contentItem.children) {
            if (child.index === undefined || child.messageKey === undefined || !child.visible || child.height <= 0)
                continue;
            total += child.height;
            ++rows;
        }
        if (rows > 0)
            return total / rows;
        return messageModel && messageModel.count > 0 ? contentHeight / messageModel.count : 0;
    }
    // A realized row telling the geometry model its real height, and the inputs
    // every measurement depended on. When any of those inputs changes, a stored
    // height is not an approximation of the new one, so the model discards the
    // lot rather than applying heights taken at a geometry that no longer holds.
    //
    // Reports are queued and applied together once the current layout step
    // finishes. A row reports its height while it is being created, and applying
    // that immediately would move the viewport and change which rows exist from
    // inside the creation of one of them.
    property var pendingRowHeights: ({})
    property bool rowHeightsQueued: false
    function publishRowHeight(key, rowHeight, bodyHeight, owner) {
        if (!messageModel || !key || !(rowHeight >= 0))
            return;
        pendingRowHeights[key] = [rowHeight, bodyHeight === undefined ? -1 : bodyHeight, owner, messageModel];
        if (!rowHeightsQueued) {
            rowHeightsQueued = true;
            Qt.callLater(transcript.flushRowHeights);
            // Also before the next frame is drawn: a delegate that has already
            // taken its new height must not be presented at positions computed
            // from its old one. The polish pass runs before rendering and repeats
            // while items ask for it, so reports raised during it still land.
            if (shell.transcript === transcript)
                host.scheduleTranscriptRestore();
        }
    }
    function flushRowHeights() {
        if (!rowHeightsQueued)
            return;
        rowHeightsQueued = false;
        const pending = pendingRowHeights;
        pendingRowHeights = ({});
        if (!messageModel)
            return;
        for (const key in pending) {
            const report = pending[key];
            // Destroying an image gallery changes its parent's height. Such a
            // report belongs to a retired delegate, not to the cached row.
            if (report[3] !== messageModel
                    || (report[2] !== undefined
                        && (!report[2] || itemAtIndex(messageModel.indexOfKey(key)) !== report[2])))
                continue;
            messageModel.geometry.setRowHeight(key, report[0], report[1]);
        }
    }
    // Rows no delegate has reported are measured off the GUI thread, a few at a
    // time while the reader is not moving. Measuring costs about 1.7 ms a body
    // on a worker and nothing at all on this thread, but the results arrive here,
    // so they are still paced rather than delivered in one burst.
    Timer {
        id: rowMeasurementTimer
        objectName: "transcriptMeasurementTimer"
        interval: 120
        repeat: true
        running: !!transcript.messageModel && transcript.measurementPending
            && transcript.windowVisible && !transcript.scrolling && !transcript.dragging
        onTriggered: {
            if (!transcript.messageModel || transcript.width <= 0)
                return;
            // Measure as the delegate lays out: its body's real width once a
            // row has reported one, and the reading font rather than the
            // application's. Anything else is a height every row corrects
            // when it is realized, under the reader.
            const width = transcript.bodyWidth > 0 ? transcript.bodyWidth : transcript.width - 64;
            transcript.measurementPending = transcript.messageModel.measureUnmeasuredRows(32, width, transcript.readingFont);
        }
    }
    // Widest body a delegate has reported at this layout: an assistant answer,
    // which spans the conversation column.
    property real bodyWidth: 0
    readonly property font readingFont: Qt.font({family: KetTheme.fontFamily, pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels})
    readonly property string layoutFingerprint: Math.round(width) + "|" + KetTheme.fontFamily + "|"
        + agentHost.monoFontFamily + "|" + KetTheme.zoom + "|" + Qt.application.font.pixelSize + "|" + Screen.devicePixelRatio + "|" + shell.conversationColumnWidth
    onLayoutFingerprintChanged: {
        measurementPending = true;
        if (!messageModel)
            return;
        clearMeasuredHeights();
        bodyWidth = 0;
        messageModel.geometry.setLayoutFingerprint(layoutFingerprint);
        // That dropped every measurement, but a delegate reports its height only
        // when the height changes, and a new width leaves many rows' heights as
        // they were. Report every realized row again, or those rows fall back to
        // the provisional height and the rows below are drawn over them.
        for (let index = 0; index < rowRepeater.count; ++index) {
            const row = rowRepeater.itemAt(index);
            if (row)
                publishRowHeight(row.messageKey, row.height, row.reportedBodyHeight, row);
        }
    }

    // Layout trace only: one pass over the realized window per frame. Realized
    // delegates are the whole truth for what is on screen — a row ListView has not
    // created cannot be visible — so this is exact, not a sample.
    //
    // The collapsed counts stay in the trace even though a collapsed group is now
    // one row: they are what the Stage 0 baseline was measured with, so an
    // after-the-fix capture has to report the same fields to be comparable. On
    // this build they should read zero.
    function tracedRowCensus() {
        const top = contentY;
        const bottom = contentY + height;
        const keys = [];
        const collapsed = [];
        let geometryMismatches = 0;
        let firstIndex = -1;
        let lastIndex = -1;
        let collapsedInViewport = 0;
        for (const child of contentItem.children) {
            if (child.messageKey === undefined || !child.messageKey || child.index === undefined)
                continue;
            if (child.height <= 0) {
                collapsed.push(child.index);
                // A zero-height row still has a position, even with no extent.
                if (child.y >= top && child.y <= bottom)
                    ++collapsedInViewport;
                continue;
            }
            if (!child.visible || child.y >= bottom || child.y + child.height <= top)
                continue;
            keys.push(child.messageKey);
            // What the geometry model believes this row is, against what it
            // turned out to be. A non-zero count here means a delegate changed
            // height without telling us, and every number the model produces is
            // then a guess wearing a measurement's clothes.
            if (Math.abs(messageModel.geometry.height(child.index) - child.height) > 1)
                ++geometryMismatches;
            if (firstIndex < 0 || child.index < firstIndex)
                firstIndex = child.index;
            if (child.index > lastIndex)
                lastIndex = child.index;
        }
        // A run of collapsed members spans model indexes but no pixels, so the index
        // span of what is on screen is the honest denominator for their density.
        let collapsedInSpan = 0;
        for (const index of collapsed)
            if (index >= firstIndex && index <= lastIndex)
                ++collapsedInSpan;
        return {
            keys: keys,
            collapsedRealized: collapsed.length,
            collapsedInViewport: collapsedInViewport,
            collapsedInSpan: collapsedInSpan,
            indexSpan: firstIndex < 0 ? 0 : lastIndex - firstIndex + 1,
            // ListView's extrapolated extent beside the sum of what rows
            // actually measured, so the two can be compared over a whole scroll.
            listContentHeight: Math.round(contentHeight),
            geometryTotal: Math.round(messageModel.geometry.totalHeight),
            geometryRows: messageModel.geometry.rowCount,
            geometryMeasured: messageModel.geometry.measuredRowCount,
            geometryMismatches: geometryMismatches
        };
    }
    // Layout trace only (M1): a fling that reverses is as visible to the reader as a
    // gesture that does, so the two are reported separately rather than merged.
    function tracedGesturePhase() {
        if (wheelEngaged || dragging || transcriptScrollBar.pressed)
            return "gesture";
        return moving || wheelGlide.running ? "momentum" : "idle";
    }
    function historyRequestAllowed() {
        const state = host.historyState;
        return shell.transcript === transcript && !shell.restoringTranscript && !shell.updatingTranscript && !shell.settlingTranscript && !historyRequestPending && !state.loading && !state.error && !!messageModel && messageModel.count > 0;
    }
    function requestHistoryPage(direction) {
        historyRequestPending = true;
        historyRequestDirection = direction;
        historyRequestEdgeKey = historyEdgeKey(direction);
        historyRequestOppositeKey = historyEdgeKey(direction === "older" ? "newer" : "older");
        // Unique across cached transcripts sharing one host.
        historyRequestToken = ++shell.transcriptHistoryRequestSerial;
        historyRequestWatchdog.restart();
        shell.traceTranscript("history-request", {direction: direction, contentAbove: contentAbove(), contentBelow: contentBelow()});
        const rowHeight = averageRenderedRowHeight();
        // Lets the window be sized by how much it shows rather than by a count.
        host.reportTranscriptExtent(rowsHeight, height);
        if (direction === "older") {
            ++olderPageRequests;
            host.loadOlderMessages(height, rowHeight, historyRequestToken);
        } else {
            ++newerPageRequests;
            host.loadNewerMessages(height, rowHeight, historyRequestToken);
        }
    }
    // Direction of the current physical/kinetic motion: "older", "newer" or "".
    function readingDirection() {
        if (!moving && !wheelEngaged)
            return "";
        if (verticalVelocity < -1 || (wheelEngaged && transcriptWheel.direction > 0))
            return "older";
        if (verticalVelocity > 1 || (wheelEngaged && transcriptWheel.direction < 0))
            return "newer";
        return "";
    }
    function evaluateHistoryPrefetch() {
        if (followTail || !historyRequestAllowed())
            return false;
        const state = host.historyState;
        const direction = readingDirection();
        if (state.hasOlder && !olderPrefetchBackoff && !olderTrimBlocked && direction !== "newer" && contentAbove() < prefetchThreshold) {
            requestHistoryPage("older");
            return true;
        }
        if (state.hasNewer && state.canLoadNewer && !newerPrefetchBackoff && !newerTrimBlocked && direction !== "older" && contentBelow() < prefetchThreshold) {
            requestHistoryPage("newer");
            return true;
        }
        return false;
    }
    // Compatibility entry point for callers that only mean "re-check now".
    function requestOlderAtTop() {
        return evaluateHistoryPrefetch();
    }
    function historyStateUpdated() {
        const state = host.historyState;
        shell.traceTranscript("history-state", {
            loading: !!state.loading,
            hasOlder: !!state.hasOlder,
            hasNewer: !!state.hasNewer,
            error: state.error || "",
            pending: historyRequestPending,
            olderBackoff: olderPrefetchBackoff,
            newerBackoff: newerPrefetchBackoff,
            olderTrimBlocked: olderTrimBlocked,
            newerTrimBlocked: newerTrimBlocked,
            lastBatch: state.lastBatch || ({})
        });
        if (findSearchingOlder && !state.loading) {
            if (findRequestToken > 0 && Number(state.requestToken || 0) === findRequestToken)
                findRequestCompleted = true;
            Qt.callLater(transcript.resumeOlderFindSearch);
        }
        if (historyRequestPending && !state.loading && Number(state.requestToken || 0) === historyRequestToken) {
            historyRequestPending = false;
            historyRequestWatchdog.stop();
            const older = historyRequestDirection === "older";
            if (!older) {
                // Its rows may be inserted after this update; they are history, not new messages.
                newerPageLanding = true;
                Qt.callLater(transcript.clearNewerPageLanding);
            }
            if (!state.error && historyEdgeKey(historyRequestDirection) === historyRequestEdgeKey) {
                if (older)
                    olderPrefetchBackoff = true;
                else
                    newerPrefetchBackoff = true;
            }
            if (!state.error && historyEdgeKey(older ? "newer" : "older") !== historyRequestOppositeKey) {
                if (older)
                    newerTrimBlocked = true;
                else
                    olderTrimBlocked = true;
            }
        }
        historyPrefetchSettle.restart();
        Qt.callLater(transcript.evaluateHistoryPrefetch);
        if (!idlePrefetchDone && !state.loading)
            idlePrefetchTimer.restart();
    }
    // Background prefetch of one older page once the conversation is idle.
    // The prepend is anchored like any other and does not chain by itself.
    function runIdlePrefetch() {
        if (idlePrefetchDone || shell.transcript !== transcript || !host.historyState.hasOlder || host.historyState.error)
            return;
        // Never prepend under a streaming turn; onSessionChanged re-arms after it completes.
        if (host.sessionRunning)
            return;
        // Rows that do not fill the viewport cannot be scrolled, so no reader input would
        // ever ask for the page above them (a newest page that is mostly a compaction and
        // collapsed activity shows as a near-empty transcript). Those pages are loaded even
        // at the tail -- they land above a pinned bottom -- one per idle pass until the
        // viewport is full.
        const underfilled = rowsHeight < height;
        if (underfilled && olderPrefetchBackoff)
            return;
        // Not while the reader is at the newest message: a large older page can push the window
        // past its budget, and the trim that follows drops the newest rows from under the
        // reader. Scrolling up re-arms loading through the ordinary prefetch.
        if (followTail && !underfilled)
            return;
        if (shell.transcriptInputActive() || !historyRequestAllowed()) {
            idlePrefetchTimer.restart();
            return;
        }
        idlePrefetchDone = !underfilled;
        if (underfilled)
            shell.traceTranscript("underfilled-prefetch", {rowsHeight: rowsHeight});
        requestHistoryPage("older");
    }
    function clearHistoryTrimBlocks() {
        olderTrimBlocked = false;
        newerTrimBlocked = false;
    }
    function noteTranscriptInput() {
        olderPrefetchBackoff = false;
        newerPrefetchBackoff = false;
        clearHistoryTrimBlocks();
        if (!idlePrefetchDone)
            idlePrefetchTimer.restart();
        Qt.callLater(transcript.evaluateHistoryPrefetch);
    }
    function retryHistory() {
        olderPrefetchBackoff = false;
        if (host.historyState.resyncRequired)
            followTail = true;
        // One notice covers both directions, so retrying the older page after a
        // newer one failed would ask the daemon for a page that never broke and
        // leave the notice standing.
        requestHistoryPage(host.historyState.errorNewer ? "newer" : "older");
    }

    // --- Owned layout ----------------------------------------------------------------
    readonly property var geometry: messageModel ? messageModel.geometry : null
    readonly property var windowModel: messageModel ? messageModel.window : null
    readonly property int count: messageModel ? messageModel.count : 0
    // Bumped on every geometry change. top(row) is a function, not a property, so
    // bindings that place rows read this to know when to ask again.
    property int geometryRevision: 0
    readonly property real headerHeight: historyHeader.height
    // Assigned on each geometry change rather than bound to it: a row reporting
    // its height from inside a layout pass would otherwise re-enter this binding.
    property real rowsHeight: 0
    contentWidth: width
    // Assigned, not bound. Setting it can clamp contentY, which realizes rows,
    // which report heights, which change it again -- inside the assignment.
    function updateContentHeight() {
        const next = headerHeight + rowsHeight + transcriptFooter.height;
        if (Math.abs(next - contentHeight) < 0.01)
            return;
        // Flickable pulls contentY back inside a shorter extent as part of the
        // assignment. That is not the reader moving; the reading position is
        // re-applied right after.
        const placing = placingRows;
        placingRows = true;
        try {
            contentHeight = next;
        } finally {
            placingRows = placing;
        }
    }
    // Pixels realized on either side of the viewport, with ListView's name and
    // meaning: rows this close to the reader keep their delegates, so a row
    // growing while streaming is not rebuilt as it crosses the viewport edge.
    // Zero means twice the viewport height. A plain value rather than a binding,
    // so a value assigned from outside is not silently re-bound on resize.
    property real cacheBuffer: 0
    readonly property real realizedMargin: cacheBuffer > 0 ? cacheBuffer : height * 2
    onRealizedMarginChanged: updateRowWindow()
    // The reading position, in TranscriptPosition.capture's shape: the reading
    // row's key, how far the viewport starts from that row's top, and -- when the
    // row is a long message spanning the top of the viewport -- the text the
    // reader is on, so content changing above that line inside the same message
    // is held too. A row's top alone cannot express that.
    property var readingAnchor: null
    // Set while this view moves contentY itself, so that move is not mistaken for
    // the reader's.
    property bool placingRows: false

    function rowTop(row) {
        return headerHeight + (geometry ? geometry.top(row) : 0);
    }
    function maximumContentY() {
        return Math.max(0, contentHeight - height);
    }
    // The reading row, chosen as TranscriptPosition.capture chooses it: the row
    // whose top is nearest a point a third of the way down the viewport, among
    // rows that begin inside it. A row that merely contains that point may have
    // begun far above and be growing at its end; holding its top would push the
    // row the reader is actually on down the screen.
    readonly property real readingProbe: Math.max(24, Math.min(height - 24, height / 3))
    function readingRow() {
        const probeRow = geometry.indexAtY(Math.max(0, contentY + readingProbe - headerHeight));
        if (probeRow < 0)
            return -1;
        let best = -1;
        let bestDistance = Number.POSITIVE_INFINITY;
        for (const row of [probeRow, probeRow + 1]) {
            // The geometry's own row count, not the view's: a page reports its
            // removal to the geometry before the view's count follows, and one
            // row past the geometry's end is the bottom of the content -- a row
            // with no key, which would leave nothing for the insertion to hold.
            if (row >= geometry.rowCount)
                continue;
            const screenY = rowTop(row) - contentY;
            if (screenY < 24 || screenY >= height - 24)
                continue;
            const distance = Math.abs(screenY - readingProbe);
            if (distance < bestDistance) {
                best = row;
                bestDistance = distance;
            }
        }
        // One row taller than the viewport: it is the only thing on screen.
        return best >= 0 ? best : probeRow;
    }
    function rememberReadingPosition() {
        if (placingRows || !geometry)
            return;
        const row = readingRow();
        if (row < 0) {
            readingAnchor = null;
            return;
        }
        const anchor = {
            followTail: false,
            index: row,
            key: geometry.keyAt(row),
            offset: contentY - rowTop(row),
            position: contentY
        };
        if (!followTail && itemAtIndex(row)) {
            const captured = TranscriptPosition.capture(transcript);
            if (captured.key === anchor.key && captured.text)
                anchor.text = captured.text;
        }
        readingAnchor = anchor;
        // The workspace's anchor is the same position under an older name, and
        // other code reads it -- a streaming row pins its text while the reader
        // is anchored on it. Moving the viewport without updating it would pin
        // the wrong row.
        if (shell.transcript === transcript)
            shell.transcriptAnchor = anchor;
    }
    // Puts the viewport back where the reader was: at the bottom while following
    // the tail, otherwise on the same line of the same row. Called after every
    // geometry change, so a row above the reader growing moves contentY by exactly
    // the growth and nothing on screen moves.
    // final: the retry after the model settled. A regroup reports its rows as
    // a removal and then an insertion, and the geometry is published between
    // the two: the row a reader's anchor moves to may not be laid out yet.
    function holdReadingPosition(final) {
        if (placingRows || !geometry)
            return;
        let target;
        let reachable = true;
        if (followTail) {
            target = maximumContentY();
        } else {
            const anchor = resolveAnchor(readingAnchor);
            const row = anchor ? geometry.rowForKey(anchor.key) : -1;
            if (row < 0 && readingAnchor && final !== true) {
                Qt.callLater(holdSettledReadingPosition);
                return;
            }
            if (row < 0) {
                // Nothing left to hold: the reader's message is no longer
                // loaded. Adopting contentY here keeps whatever now sits under
                // it, which is the visible jump -- record it.
                shell.traceTranscript("anchor-lost", {key: readingAnchor ? readingAnchor.key : "",
                                                      anchorOffset: readingAnchor ? readingAnchor.offset : 0});
                rememberReadingPosition();
                return;
            }
            const item = anchor.text ? itemAtIndex(row) : null;
            const wanted = item ? TranscriptPosition.contentY(item, anchor) : rowTop(row) + anchor.offset;
            target = Math.min(maximumContentY(), Math.max(0, wanted));
            reachable = Math.abs(target - wanted) < 0.5;
        }
        // Fractional line heights move rows by fractions of a pixel; skipping
        // those and then adopting the result would let the reader drift.
        if (Math.abs(target - contentY) >= 0.01)
            placeContentY(target);
        // With the viewport back in place, the row nearest the reading point may
        // now be a different one, and the line under it may have moved within its
        // row. Holding what is there now keeps the row the reader is looking at
        // still. Nothing moves here.
        //
        // Not when the position could not be reached: rows whose heights were
        // just discarded (a width change) count as placeholders until they
        // report again, and the extent is briefly too short. Adopting the clamped
        // position would forget where the reader really was.
        if (reachable)
            rememberReadingPosition();
    }
    function holdSettledReadingPosition() {
        holdReadingPosition(true);
    }
    // The anchor as a row the view lays out. A message keeps its identity while
    // the row showing it changes: older tool rows prepended into its activity
    // group make it a withheld member, and re-chunking a long answer renames
    // its blocks. The anchor moves to that row rather than being dropped,
    // which would adopt whatever sits under the viewport now.
    function resolveAnchor(anchor) {
        if (!anchor || !anchor.key || geometry.rowForKey(anchor.key) >= 0 || !messageModel)
            return anchor;
        const key = messageModel.visibleKeyFor(anchor.key);
        if (!key || geometry.rowForKey(key) < 0)
            return anchor;
        shell.traceTranscript("anchor-resolved", {from: anchor.key, to: key, anchorOffset: anchor.offset});
        // The text sample belonged to the old row; the offset is the best
        // remaining estimate of the line, bounded by the new row.
        const resolved = Object.assign({}, anchor, {key: key, text: undefined,
                                                    offset: Math.max(0, Math.min(anchor.offset, geometry.height(geometry.rowForKey(key))))});
        if (anchor === readingAnchor)
            readingAnchor = resolved;
        return resolved;
    }
    // Adopts a reading position captured elsewhere (TranscriptPosition.capture)
    // and holds it.
    function holdAnchor(captured) {
        if (!geometry || !captured)
            return;
        const anchor = resolveAnchor(captured);
        const row = anchor.key ? geometry.rowForKey(anchor.key) : Math.min(anchor.index, count - 1);
        if (row < 0) {
            // The row went away with a trimmed edge. When the view still holds a
            // row of its own, that row is where the reader is; the anchor's old
            // content position would now point at whatever a page put there.
            const own = readingAnchor ? geometry.rowForKey(readingAnchor.key) : -1;
            if (own >= 0 || geometry.rowCount > 0) {
                if (own >= 0)
                    holdReadingPosition();
                updateRowWindow();
                return;
            }
            if (anchor.position !== undefined) {
                placeContentY(Math.min(maximumContentY(), Math.max(0, anchor.position)));
                readingAnchor = null;
                rememberReadingPosition();
            }
            return;
        }
        const before = contentY;
        const ownKey = readingAnchor ? readingAnchor.key : "";
        readingAnchor = Object.assign({}, anchor, {
            key: geometry.keyAt(row),
            index: row
        });
        holdReadingPosition();
        // The workspace restores an anchor it captured before a mutation. When
        // that moves the viewport, the view's own anchor disagreed with it: one
        // of the two is stale, and the reader saw the difference.
        if (Math.abs(contentY - before) >= 0.5)
            shell.traceTranscript("restore-moved", {delta: contentY - before, key: anchor.key,
                                                    ownKey: ownKey, followTail: followTail});
        // An explicit restore runs once its mutation has finished, and its caller
        // reads the restored rows straight away.
        updateRowWindow();
    }
    // Where the fling in progress will come to rest. Flickable does not report a
    // reliable velocity late in a fling, but under constant deceleration the
    // speed follows from the distance still to travel, which survives a shift.
    property real flingEnd: 0
    onVerticalVelocityChanged: {
        const velocity = verticalVelocity;
        if (flicking && !placingRows && Math.abs(velocity) > 1)
            flingEnd = contentY + Math.sign(velocity) * velocity * velocity / (2 * flickDeceleration);
    }
    function placeContentY(target) {
        // A fling in progress keeps going: assigning contentY ends it, so it is
        // restarted from the new position with the distance it had left.
        const flinging = flicking && !dragging;
        const remaining = flingEnd + (target - contentY) - target;
        const restoring = shell.restoringTranscript;
        placingRows = true;
        shell.restoringTranscript = true;
        try {
            contentY = target;
        } finally {
            shell.restoringTranscript = restoring;
            placingRows = false;
        }
        if (flinging && !flicking && Math.abs(remaining) > 1) {
            shell.traceTranscript("fling-restart", {remaining: remaining});
            flick(0, -Math.sign(remaining) * Math.sqrt(2 * flickDeceleration * Math.abs(remaining)));
        }
    }
    function geometryUpdated() {
        rowsHeight = geometry ? geometry.totalHeight : 0;
        updateContentHeight();
        ++geometryRevision;
        holdReadingPosition();
        Qt.callLater(transcript.updateRowWindow);
    }
    // Which rows get a delegate: everything within the over-range of the viewport,
    // found by binary search over the same prefix sum the rows are placed from.
    property bool updatingRowWindow: false
    function updateRowWindow() {
        if (!windowVisible) {
            rowWindowGrowth.running = false;
            return;
        }
        if (!windowModel || !geometry)
            return;
        // Mid-mutation the rows have moved but the viewport has not been put back
        // over the reader's row yet; rows picked now would be the page's new rows
        // at the old position, all created at once and dropped again a moment
        // later. The mutation's restore asks again once it has run.
        if (shell.transcript === transcript && shell.updatingTranscript) {
            Qt.callLater(transcript.updateRowWindow);
            return;
        }
        // Creating a row can move the viewport; that move asks again once this
        // pass has finished instead of re-entering it.
        if (updatingRowWindow) {
            Qt.callLater(transcript.updateRowWindow);
            return;
        }
        updatingRowWindow = true;
        try {
            updateRowWindowNow();
        } finally {
            updatingRowWindow = false;
        }
    }
    function updateRowWindowNow() {
        // The geometry's own row count, not the view's: a mutation reports its
        // rows to the geometry before the view's count catches up.
        const rows = geometry.rowCount;
        if (rows === 0) {
            windowModel.setRange(0, -1);
            return;
        }
        const rowAt = (y, fallback) => {
            if (y <= 0)
                return 0;
            const row = geometry.indexAtY(y);
            return row < 0 ? fallback : row;
        };
        const viewTop = contentY - headerHeight;
        const visibleFirst = rowAt(viewTop, rows - 1);
        const visibleLast = rowAt(viewTop + height, rows - 1);
        let first = rowAt(viewTop - realizedMargin, rows - 1);
        let last = rowAt(viewTop + height + realizedMargin, rows - 1);
        // Rows on screen are created at once; rows in the margin a few a frame.
        // A page inserted above the reader puts dozens of rows in the margin in
        // one step, and building them all in that frame is a visible stall.
        const current = windowModel.first;
        const currentLast = windowModel.last;
        if (current >= 0 && currentLast >= current) {
            if (first < current)
                first = Math.max(first, Math.min(visibleFirst, current - realizeRowsPerFrame));
            if (last > currentLast)
                last = Math.min(last, Math.max(visibleLast, currentLast + realizeRowsPerFrame));
        }
        const wantedFirst = rowAt(viewTop - realizedMargin, rows - 1);
        const wantedLast = rowAt(viewTop + height + realizedMargin, rows - 1);
        // A viewport always shows at least one row. An empty span would drop
        // every delegate only to rebuild them on the next pass.
        windowModel.setRange(Math.min(first, last), last);
        rowWindowGrowth.running = first !== wantedFirst || last !== wantedLast;
    }
    readonly property int realizeRowsPerFrame: 3
    FrameAnimation {
        id: rowWindowGrowth
        objectName: "transcriptRowWindowGrowth"
        onTriggered: transcript.updateRowWindow()
    }
    Connections {
        target: transcript.geometry
        function onGeometryChanged() {
            transcript.measurementPending = true;
            transcript.geometryUpdated();
        }
    }
    // A switch to a warm conversation re-binds geometry and windowModel in no
    // set order, and the model publishes no geometry change of its own. Laid
    // out only from onWindowModelChanged, the view could read the previous
    // conversation's geometry and keep its extent: the tail would sit far below
    // the rows and the transcript would show blank.
    onGeometryChanged: geometryUpdated()
    onHeightChanged: {
        holdReadingPosition();
        updateRowWindow();
    }
    onHeaderHeightChanged: {
        updateContentHeight();
        holdReadingPosition();
    }
    onCountChanged: Qt.callLater(transcript.updateRowWindow)
    onWindowModelChanged: {
        readingAnchor = null;
        geometryUpdated();
        Qt.callLater(transcript.updateRowWindow);
    }

    // ListView's positioning API, answered exactly from the geometry. The anchor
    // machinery in AgentWorkspace.qml still calls these.
    function indexAt(x: real, y: real): int {
        return geometry ? geometry.indexAtY(y - headerHeight) : -1;
    }
    function itemAtIndex(index: int): Item {
        if (!windowModel)
            return null;
        const row = windowModel.windowRow(index);
        return row >= 0 ? rowRepeater.itemAt(row) : null;
    }
    function forceLayout() {
        updateRowWindow();
    }
    function positionViewAtBeginning() {
        contentY = 0;
        rememberReadingPosition();
        updateRowWindow();
    }
    // Every row in the viewport has a height a delegate or a measurement gave it.
    function visibleRowsMeasured() {
        if (!geometry || geometry.rowCount === 0)
            return true;
        const top = contentY - headerHeight;
        let first = geometry.indexAtY(Math.max(0, top));
        let last = geometry.indexAtY(top + height);
        if (first < 0)
            first = 0;
        if (last < 0)
            last = geometry.rowCount - 1;
        for (let row = first; row <= last; ++row)
            if (!geometry.measured(row))
                return false;
        return true;
    }
    function positionViewAtEnd() {
        contentY = maximumContentY();
        rememberReadingPosition();
        updateRowWindow();
    }
    function positionViewAtIndex(index: int, mode: int) {
        if (!geometry || index < 0 || index >= count)
            return;
        const top = rowTop(index);
        const rowHeight = geometry.height(index);
        let target = contentY;
        if (mode === ListView.Beginning)
            target = top;
        else if (mode === ListView.Center)
            target = top + rowHeight / 2 - height / 2;
        else if (mode === ListView.End)
            target = top + rowHeight - height;
        else if (top < contentY)
            target = top;
        else if (top + rowHeight > contentY + height)
            target = top + rowHeight - height;
        contentY = Math.min(maximumContentY(), Math.max(0, target));
        rememberReadingPosition();
        // Realize the row now, so itemAtIndex() finds it without waiting a frame.
        updateRowWindow();
    }

    clip: true
    // Wheel and touchpad scrolling is applied by transcriptWheel below, not by
    // Flickable. During a touchpad gesture Flickable positions content relative
    // to where the gesture began, so a correction this view makes mid-gesture --
    // a row above the reader growing -- was overwritten on the next event and the
    // reader was thrown back to the gesture's start. With one owner of contentY
    // a correction stays made. macOS delivers touchpad momentum as wheel events,
    // so kinetic scrolling is unaffected.
    interactive: false
    readonly property real rowSpacing: 2
    // While a page can still arrive at an edge, let a flick run into that
    // bound instead of hard-stopping it; boundsMovement keeps the content
    // itself at the bound, so there is no visual overscroll.
    boundsBehavior: host.historyState.hasOlder || (host.historyState.hasNewer && host.historyState.canLoadNewer) ? Flickable.OvershootBounds : Flickable.StopAtBounds
    boundsMovement: Flickable.StopAtBounds
    // Keep two viewport extents warm on either side. Mixed-height
    // rows near the reader must not be recycled while a cached
    // tail delegate grows during streaming; this remains bounded
    // and preserves ListView virtualization for long transcripts.
    WheelHandler {
        id: transcriptWheel
        property int direction: 0
        // macOS ends a finger gesture with ScrollEnd but ends its inertia with
        // nothing at all, leaving the handler active; inertia is timed out instead.
        property int lastPhase: Qt.NoScrollPhase
        target: null
        blocking: true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            // Fresh physical input supersedes any late correction
            // left by the preceding streamed mutation.  The next
            // mutation captures the resulting reading position.
            shell.beginTranscriptInput();
            shell.traceTranscript("wheel", {wheelPhase: event.phase, pixelDeltaY: event.pixelDelta.y,
                                            angleDeltaY: event.angleDelta.y, flicking: transcript.flicking});
            const delta = event.pixelDelta.y || event.angleDelta.y;
            // Mark the synchronous Flickable motion produced by this
            // event as physical input (see onContentYChanged).
            shell.transcriptWheelEventPending = true;
            Qt.callLater(transcript.clearWheelEventPending);
            if (delta !== 0)
                direction = delta > 0 ? 1 : -1;
            if (direction > 0)
                transcript.followTail = false;
            if (direction > 0)
                transcript.streamingTailRequested = false;
            shell.captureTranscriptWheelPosition();
            // A touchpad reports pixels; a notched wheel reports eighths of a
            // degree, scrolled as the platform's lines per notch.
            // A touchpad already delivers smooth motion and is applied as it
            // comes; notches are glided so a click is not a visible jump.
            if (event.pixelDelta.y !== 0)
                transcript.contentY = Math.min(transcript.maximumContentY(), Math.max(0, transcript.contentY - event.pixelDelta.y));
            else if (event.angleDelta.y !== 0)
                transcript.glideWheel(event.angleDelta.y / 120 * Qt.styleHints.wheelScrollLines * 20);
            transcript.noteTranscriptInput();
            // With no Flickable motion there is no movementEnded: a phased
            // gesture ends at ScrollEnd, a notched wheel when its clicks stop.
            lastPhase = event.phase;
            if (event.phase === Qt.ScrollEnd) {
                wheelIdle.stop();
                Qt.callLater(transcript.movementEnded);
            } else if (event.phase === Qt.NoScrollPhase || event.phase === Qt.ScrollMomentum)
                wheelIdle.restart();
            else
                wheelIdle.stop();
        }
        onActiveChanged: {
            if (!active)
                ++transcript.viewportRevision;
            if (!active && !shell.restoringTranscript && !shell.updatingTranscript) {
                // Only a downward (or neutral) gesture re-arms tail following; the
                // kinetic tail of an upward flick may not have left the buffer yet.
                transcript.followTail = transcript.liveTail() && direction <= 0;
                transcript.streamingTailRequested = transcript.followTail;
                shell.captureTranscriptWheelPosition();
            }
        }
    }
    Timer {
        id: wheelIdle
        interval: 120
        onTriggered: {
            if (wheelGlide.running)
                restart();
            else
                transcript.movementEnded();
        }
    }
    // Distance a notched wheel still owes the reader. It is relative to wherever
    // the content is, so a position hold that shifts contentY mid-glide does not
    // change where the glide takes the reader.
    property real wheelGlideRemaining: 0
    onWindowVisibleChanged: {
        if (!windowVisible) {
            wheelGlideRemaining = 0;
            rowWindowGrowth.running = false;
        } else {
            updateRowWindow();
        }
    }
    function glideWheel(pixels) {
        // Reversing direction drops what was left of the other way.
        if (wheelGlideRemaining * pixels < 0)
            wheelGlideRemaining = 0;
        wheelGlideRemaining += pixels;
    }
    FrameAnimation {
        id: wheelGlide
        objectName: "transcriptWheelGlide"
        running: transcript.windowVisible && Math.abs(transcript.wheelGlideRemaining) > 0
        onTriggered: {
            const remaining = transcript.wheelGlideRemaining;
            // Exponential approach: most of a notch lands in the first ~100 ms.
            // A frame is taken to last between 1/240 s and 1/30 s, so a stalled
            // frame does not land the whole notch at once.
            const seconds = Math.min(1 / 30, Math.max(1 / 240, frameTime));
            const step = Math.abs(remaining) < 0.5 ? remaining : remaining * (1 - Math.exp(-seconds / 0.04));
            const target = Math.min(transcript.maximumContentY(), Math.max(0, transcript.contentY - step));
            const clamped = target !== transcript.contentY - step;
            transcript.contentY = target;
            transcript.wheelGlideRemaining = clamped ? 0 : remaining - step;
        }
    }
    // Any scrolling the reader is doing, including input this view applies itself,
    // which Flickable's moving does not see.
    // Where the reader is in the whole conversation, 0 at its first event and 1
    // at its newest, for the position indicator.
    property real conversationPosition: 0
    property var rowCursorCache: null
    property int historyStateSerial: 0
    Connections {
        target: transcript.host
        function onHistoryStateChanged() {
            ++transcript.historyStateSerial;
        }
    }
    // An event cursor for each loaded row: rows of a turn share out the turn's
    // cursor range in order; rows of a turn with no known range (a live one) are
    // placed between their known neighbours, or between the last and the head.
    function rowCursors() {
        const state = host.historyState;
        const turns = state ? state.turnCursors : null;
        const head = state ? Number(state.headCursor || 0) : 0;
        const rows = geometry ? geometry.rowCount : 0;
        if (!messageModel || !turns || !(head > 0) || rows === 0)
            return null;
        const key = rows + "|" + geometry.keyAt(0) + "|" + geometry.keyAt(rows - 1) + "|" + historyStateSerial;
        if (rowCursorCache && rowCursorCache.key === key)
            return rowCursorCache;
        const turnIds = new Array(rows);
        for (let row = 0; row < rows; ++row) {
            const entry = messageModel.get(row);
            turnIds[row] = entry ? entry.turnId || "" : "";
        }
        const cursors = new Array(rows + 1).fill(NaN);
        for (let row = 0; row < rows;) {
            let end = row + 1;
            while (end < rows && turnIds[end] === turnIds[row])
                ++end;
            const range = turns[turnIds[row]];
            if (range && range.length === 2)
                for (let index = row; index < end; ++index)
                    cursors[index] = range[0] + (range[1] - range[0]) * (index - row) / (end - row);
            row = end;
        }
        cursors[rows] = state.hasNewer ? NaN : head;
        let previous = -1;
        for (let row = 0; row <= rows; ++row) {
            if (isNaN(cursors[row]))
                continue;
            const from = previous < 0 ? cursors[row] : cursors[previous];
            for (let gap = previous + 1; gap < row; ++gap)
                cursors[gap] = from + (cursors[row] - from) * (gap - previous) / (row - previous);
            previous = row;
        }
        if (previous < 0)
            return null;
        for (let row = previous + 1; row <= rows; ++row)
            cursors[row] = cursors[previous] + (head - cursors[previous]) * (row - previous) / (rows + 1 - previous);
        rowCursorCache = {key: key, cursors: cursors, head: head};
        return rowCursorCache;
    }
    function cursorAtY(table, y) {
        const rows = geometry.rowCount;
        if (y <= 0)
            return table.cursors[0];
        let row = geometry.indexAtY(y);
        if (row < 0)
            return table.cursors[rows];
        const height = Math.max(1, geometry.height(row));
        const fraction = Math.min(1, Math.max(0, (y - geometry.top(row)) / height));
        return table.cursors[row] + (table.cursors[row + 1] - table.cursors[row]) * fraction;
    }
    function updateConversationPosition() {
        const table = rowCursors();
        let position;
        if (table) {
            const top = contentY - headerHeight;
            const start = cursorAtY(table, top) / table.head;
            const end = cursorAtY(table, top + height) / table.head;
            const span = Math.max(0, end - start);
            position = span >= 1 ? 0 : start / (1 - span);
        } else {
            // No daemon history: the loaded rows are the whole conversation.
            const range = contentHeight - height;
            position = range > 0 ? (contentY - originY) / range : 0;
        }
        if (!host.historyState.hasOlder && contentAbove() < 1)
            position = 0;
        else if (!host.historyState.hasNewer && contentBelow() < 1)
            position = 1;
        conversationPosition = Math.min(1, Math.max(0, position));
    }
    onScrollingChanged: {
        updateConversationPosition();
        // Unlike holdingHistory this stays true at the edges: pages may land
        // there, but the window is only trimmed once the reader has stopped.
        host.setTranscriptScrolling(scrolling, scrolling ? "" : reportedReadingKey());
    }
    // The row the session trims its window around: none while following the
    // tail, since the tail is then what is being read.
    function reportedReadingKey() {
        return followTail || !readingAnchor ? "" : String(readingAnchor.key || "");
    }
    property string lastReportedReadingKey: ""
    function reportReadingKey() {
        if (shell.transcript !== transcript)
            return;
        const key = reportedReadingKey();
        if (key === lastReportedReadingKey)
            return;
        lastReportedReadingKey = key;
        host.setTranscriptReadingKey(key);
    }
    onReadingAnchorChanged: reportReadingKey()

    // The wheel handler stays active after macOS inertia, which ends without a
    // ScrollEnd; inertia counts as engaged only while its events keep coming.
    readonly property bool wheelEngaged: transcriptWheel.active
        && (transcriptWheel.lastPhase !== Qt.ScrollMomentum || wheelIdle.running)
    readonly property bool scrolling: moving || wheelGlide.running || wheelIdle.running
        || (transcriptWheel.active && transcriptWheel.lastPhase !== Qt.ScrollMomentum)
    // History that arrives while the reader is scrolling waits for them to stop,
    // so inserting it never lands in the middle of a gesture -- unless the reader
    // is within bufferViewports of either end of what is loaded. A fast fling
    // covers that in well under a second; the page has to land before the
    // reader gets there, not once they are stopped at the edge.
    readonly property bool holdingHistory: scrolling
        && contentY - originY > bufferViewports * height
        && originY + contentHeight - contentY - height > bufferViewports * height
    onHoldingHistoryChanged: host.setTranscriptInputActive(holdingHistory)
    function clearWheelEventPending() {
        shell.transcriptWheelEventPending = false;
    }
    ScrollBar.vertical: Basic.ScrollBar {
        id: transcriptScrollBar
        width: KetTheme.px(7, KetTheme.zoom)
        policy: ScrollBar.AlwaysOn
        hoverEnabled: false
        // The loaded rows are a window onto the conversation, and their height
        // changes whenever a page is loaded or trimmed; a thumb sized and placed
        // from it jumps. The bar is a position indicator instead: a fixed-size
        // thumb placed by where the reader is among all the conversation's events.
        interactive: false
        opacity: transcript.scrolling || pressed ? 1 : 0
        onPressedChanged: {
            if (pressed) {
                shell.beginTranscriptInput();
                transcript.noteTranscriptInput();
                transcriptWheel.direction = 0;
                transcript.followTail = false;
                transcript.streamingTailRequested = false;
            } else {
                transcript.followTail = transcript.liveTail();
                transcript.streamingTailRequested = transcript.followTail;
                shell.captureTranscriptWheelPosition();
            }
        }
        background: null
        contentItem: Item {
            implicitWidth: KetTheme.px(4, KetTheme.zoom)
        }
        Rectangle {
            id: positionThumb
            objectName: "transcriptPositionThumb"
            width: KetTheme.px(4, KetTheme.zoom)
            height: KetTheme.px(40, KetTheme.zoom)
            radius: KetTheme.px(2, KetTheme.zoom)
            x: parent.width - width - 1.5
            y: 2 + transcript.conversationPosition * Math.max(0, parent.height - height - 4)
            color: KetTheme.t.textMuted
        }
        Behavior on opacity {
            NumberAnimation {
                duration: 160
            }
        }
    }

    onContentYChanged: {
        if (!placingRows)
            rememberReadingPosition();
        if (transcriptScrollBar.opacity > 0 || scrolling)
            updateConversationPosition();
        // A move this view makes itself answers a model or geometry change that
        // may still be in progress (a prepend reports its removal and insertion
        // separately); the span is settled once that change has finished.
        if (placingRows)
            Qt.callLater(transcript.updateRowWindow);
        else
            updateRowWindow();
        const delta = contentY - previousScrollY;
        previousScrollY = contentY;
        // A fling carrying the reader away from the tail ends tail following at
        // once. Waiting for it to end would let the next row growing at the tail
        // snap the view back down while the reader is still moving up.
        if (!placingRows && delta < 0 && flicking && followTail) {
            followTail = false;
            streamingTailRequested = false;
        }
        shell.traceTranscript("scroll");
        // Every position change (input, momentum, anchor restore) re-checks
        // the prefetch distance once the current event completes.
        Qt.callLater(transcript.evaluateHistoryPrefetch);
        if (shell.restoringTranscript || shell.updatingTranscript)
            return;
        // Rows are placed exactly, so a contentY change is always either the reader
        // or a deliberate placement; there is no layout drift left to undo.
        if (shell.transcriptInputActive())
            shell.followTranscriptInputMotion(delta);
    }
    onMovementStarted: {
        if (!shell.restoringTranscript && (dragging || wheelEngaged || transcriptScrollBar.pressed)) {
            if (!wheelEngaged)
                transcriptWheel.direction = 0;
            followTail = false;
            streamingTailRequested = false;
        }
    }
    onDraggingChanged: {
        if (dragging) {
            shell.beginTranscriptInput();
            noteTranscriptInput();
        }
    }
    onMovementEnded: {
        ++viewportRevision;
        // The wheel gesture (including its kinetic tail) is over: stop routing
        // later non-input contentY changes (keyboard focus scroll, programmatic
        // navigation) through wheel anchor stabilization, which would snap
        // them back. Streamed mutations are anchored by the mutation path.
        const upwardGesture = transcriptWheel.direction > 0;
        if (!wheelEngaged)
            transcriptWheel.direction = 0;
        if (contentAbove() < 1 && (host.historyState.hasOlder || historyRequestPending))
            ++topHardStops;
        if (!shell.restoringTranscript && !shell.updatingTranscript) {
            followTail = liveTail() && !upwardGesture;
            streamingTailRequested = followTail;
            shell.captureTranscriptWheelPosition();
        }
    }
    onContentHeightChanged: {
        shell.traceTranscript("list-height");
        // Heights keep arriving while a running turn streams, so waiting for
        // them to stop could keep the transcript hidden indefinitely. The
        // settle has a hard deadline from when it began.
        if (shell.settlingTranscript && Date.now() - shell.transcriptSettleStarted < shell.transcriptSettleDeadline)
            settleTimer.restart();
    }
    // A width change needs no anchor of its own: re-wrapped rows report new
    // heights and holdReadingPosition keeps the reading line where it was. The
    // S2 height cache is invalidated lazily by its width bucket.

    // S2: measured body heights per messageKey for the current width bucket.
    // Used as the unrendered-row estimate so rows re-created by virtualization
    // (conversation switch, history materialization) come back at their real
    // height instead of a glyph-width guess. Cleared when the width changes.
    property var measuredHeights: ({})
    // Bumped only when a stored measurement changes so unrendered estimates
    // re-evaluate (per key); the map itself is mutated in place and does not notify.
    // Only the delegate for the changed key re-evaluates its estimate.
    signal measuredHeightChanged(string key)
    property int measuredHeightCount: 0
    // Width bucket of the stored entries (plain value, not a binding).
    property int measuredHeightWidth: -1
    readonly property int measuredHeightLimit: 4096
    function clearMeasuredHeights() {
        measuredHeights = ({});
        measuredHeightCount = 0;
        measuredHeightWidth = Math.round(width);
    }
    function measuredBodyHeight(key, length, bodyWidth) {
        if (!key || measuredHeightWidth !== Math.round(width))
            return -1;
        const entry = measuredHeights[key];
        return entry && entry.length === length && entry.width === Math.round(bodyWidth) ? entry.height : -1;
    }
    function rememberBodyHeight(key, length, bodyWidth, measured) {
        if (!key || !(measured > 0) || bodyWidth <= 0)
            return;
        if (bodyWidth > transcript.bodyWidth + 0.5)
            transcript.bodyWidth = bodyWidth;
        if (measuredHeightWidth !== Math.round(width))
            clearMeasuredHeights();
        if (!measuredHeights[key]) {
            if (measuredHeightCount >= measuredHeightLimit)
                clearMeasuredHeights();
            ++measuredHeightCount;
        }
        const previous = measuredHeights[key];
        const roundedWidth = Math.round(bodyWidth);
        if (previous && previous.length === length && previous.width === roundedWidth && Math.abs(previous.height - measured) < 0.01)
            return;
        measuredHeights[key] = {
            length: length,
            width: roundedWidth,
            height: measured
        };
        measuredHeightChanged(key);
    }

    Basic.ToolButton {
        id: jumpToLatestButton
        objectName: "jumpToLatestButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: KetTheme.px(12, KetTheme.zoom)
        // A Flickable adopts its children into the scrolling content; this button
        // belongs to the viewport, or it rides along with the rows and is only on
        // screen when the bottom of the loaded content is.
        parent: transcript
        z: 3
        height: KetTheme.px(44, KetTheme.zoom)
        width: KetTheme.px(44, KetTheme.zoom)
        padding: KetTheme.px(13, KetTheme.zoom)
        visible: !transcript.followTail && transcript.messageModel.count > 0
        contentItem: Item {
            implicitWidth: KetTheme.px(16, KetTheme.zoom)
            implicitHeight: implicitWidth
            KetIcon {
                anchors.centerIn: parent
                box: Math.min(parent.width, parent.height)
                name: KetTheme.iconForLucide("arrow-up")
                color: KetTheme.t.textMain
                rotation: 180
            }
        }
        background: Rectangle {
            color: parent.hovered ? KetTheme.t.surfaceHover : KetTheme.t.surfaceRaised
            radius: KetTheme.px(5, KetTheme.zoom)
            border.color: KetTheme.t.panelBorder
        }
        onClicked: shell.jumpToLatest()
        Accessible.name: transcript.newMessageCount > 0 ? "Jump to latest, " + transcript.newMessageCount + " new" : "Jump to latest"
        Rectangle {
            id: newMessageBadge
            objectName: "newMessageBadge"
            visible: transcript.newMessageCount > 0
            anchors.right: parent.right
            anchors.bottom: parent.top
            anchors.bottomMargin: -KetTheme.px(10, KetTheme.zoom)
            anchors.rightMargin: -KetTheme.px(6, KetTheme.zoom)
            height: KetTheme.px(18, KetTheme.zoom)
            width: Math.max(KetTheme.px(18, KetTheme.zoom), newMessageLabel.implicitWidth + KetTheme.px(10, KetTheme.zoom))
            radius: KetTheme.px(9, KetTheme.zoom)
            color: KetTheme.t.accent
            Text {
                id: newMessageLabel
                objectName: "newMessageBadgeLabel"
                anchors.centerIn: parent
                text: transcript.newMessageCount + " new"
                color: KetTheme.t.actionPrimaryInk
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                font.weight: Font.DemiBold
            }
        }
        KetToolTip {
            objectName: "ketToolTip"
            visible: jumpToLatestButton.hovered
            text: jumpToLatestButton.Accessible.name
        }
    }

    Item {
        id: historyHeader
        objectName: "olderHistoryHeader"
        y: 0
        width: transcript.width
        // The mock's padding above the first row. It doubles as the slim, non-blocking
        // edge row for older history: a spinner while a page loads and a retry control
        // only on error, so neither ever shifts the reading rows.
        height: KetTheme.px(24, KetTheme.zoom)
        readonly property bool showRetry: !!host.historyState.error || !!host.historyState.resyncRequired
        Row {
            objectName: "olderHistoryIndicator"
            anchors.centerIn: parent
            spacing: KetTheme.px(6, KetTheme.zoom)
            visible: historyHeader.height > 0 && !!host.historyState.loading && !historyHeader.showRetry
            KetActivityIndicator {
                anchors.verticalCenter: parent.verticalCenter
                width: KetTheme.px(14, KetTheme.zoom)
                height: KetTheme.px(14, KetTheme.zoom)
                running: parent.visible && transcript.windowVisible && host.workingMotionEnabled
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Loading earlier messages"
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                color: KetTheme.t.textMuted
            }
        }
        Basic.Button {
            id: olderMessagesButton
            objectName: "loadOlderMessagesButton"
            anchors.centerIn: parent
            width: Math.min(parent.width - KetTheme.px(32, KetTheme.zoom), implicitWidth)
            height: KetTheme.px(22, KetTheme.zoom)
            padding: KetTheme.px(2, KetTheme.zoom)
            horizontalPadding: KetTheme.px(10, KetTheme.zoom)
            visible: historyHeader.height > 0 && historyHeader.showRetry
            enabled: !host.historyState.loading
            text: host.historyState.resyncRequired ? "History changed · Reload latest" : "Retry loading history"
            onClicked: transcript.retryHistory()
            contentItem: Text {
                text: olderMessagesButton.text
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                color: KetTheme.t.textSecondary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            background: Rectangle {
                color: olderMessagesButton.hovered ? KetTheme.t.surfaceHover : "transparent"
                radius: KetTheme.px(5, KetTheme.zoom)
            }
            KetToolTip {
                objectName: "ketToolTip"
                visible: olderMessagesButton.hovered && !!host.historyState.error
                text: host.historyState.error || ""
            }
        }
    }

    Item {
        id: transcriptFooter
        y: transcript.headerHeight + transcript.rowsHeight
        width: transcript.width
        onHeightChanged: {
            transcript.updateContentHeight();
            transcript.holdReadingPosition();
        }
        // The run state is part of the transcript flow. Keep a
        // real gutter above it so a one-line command delegate
        // can never paint beneath the footer while its implicit
        // layout is settling during a streamed model update.
        height: host.sessionRunning ? KetTheme.px(62, KetTheme.zoom) : 0

        AgentRunStatus {
            workspace: shell
            visible: host.sessionRunning
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Math.max(KetTheme.px(16, KetTheme.zoom), (parent.width - shell.conversationColumnWidth) / 2 - KetTheme.px(4, KetTheme.zoom))
            anchors.rightMargin: Math.max(KetTheme.px(16, KetTheme.zoom), (parent.width - shell.conversationColumnWidth) / 2 - KetTheme.px(4, KetTheme.zoom))
            anchors.top: parent.top
            anchors.topMargin: KetTheme.px(8, KetTheme.zoom)
        }
    }

    AgentEmptyState {
        // The viewport, not the content item: an empty transcript's content is not as tall as the view.
        parent: transcript
        anchors.fill: parent
        workspace: shell
        visible: transcript.messageModel.count === 0 && !transcript.messageModel.materializing && !host.historyState.loading && !host.historyState.error
        z: 2
    }

    Repeater {
        id: rowRepeater
        model: transcript.windowModel
        delegate: TranscriptMessage {
            shell: transcript.shell
            host: transcript.host
            preview: transcript.preview
            view: transcript
            y: transcript.geometryRevision, transcript.rowTop(rowIndex)
        }
    }
}
