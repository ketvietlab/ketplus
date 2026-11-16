import QtQuick
import Ket
import QtQuick.Controls
import QtQuick.Layouts

// Daemon-managed jobs and legacy drain-only tasks share one owner-scoped strip.
// The daemon owns the list; this only shows it, opens one task's output and asks to stop one.
Rectangle {
    id: bar
    required property var workspace

    property var dismissedTasks: []
    function taskKey(task) {
        return taskScope + ":" + String(task.task_id) + ":" + String(task.started_at || "") + ":" + String(task.ended_at || "");
    }
    readonly property var tasks: (agentHost.backgroundTasks || []).filter(task => live(task) || dismissedTasks.indexOf(taskKey(task)) < 0)
    readonly property var barRecipe: KetTheme.taskBarRecipe(KetTheme.dark, KetTheme.zoom, false, false, true, false)
    readonly property var pane: agentHost.backgroundTaskOutput || ({})
    readonly property bool offline: registry.connected === false || agentHost.presentationState.connection === "unavailable"
    readonly property int liveCount: tasks.filter(task => bar.live(task)).length
    property bool collapsed: false
    property string confirmingStop: ""
    property real now: Date.now()
    readonly property bool windowVisible: visible && Window.window !== null && Window.window.visible && Window.window.visibility !== Window.Minimized

    readonly property var registry: agentHost.backgroundTaskState || ({})
    readonly property string taskScope: String(registry.scope || workspace.transcriptScope || "")
    property string confirmationScope: ""
    onRegistryChanged: {
        const scope = String(registry.scope || "");
        if (scope !== confirmationScope) {
            confirmingStop = "";
            if (cleanupDialog.visible)
                cleanupDialog.reject();
            confirmationScope = scope;
        }
    }
    readonly property bool canRead: registry.canRead !== false
    readonly property bool canControl: registry.canControl !== false
    readonly property bool canResolve: registry.canResolve === true
    readonly property int tickingCount: tasks.filter(task => live(task) && !attention(task)).length
    readonly property int attentionCount: tasks.filter(task => attention(task)).length
    function state(task) {
        return task.state || task.status || "unknown";
    }
    function attention(task) {
        return state(task) === "needs_attention";
    }
    function live(task) {
        return ["queued", "starting", "running", "stopping", "needs_attention"].indexOf(state(task)) >= 0;
    }
    function canStop(task) {
        return !offline && canControl && ["queued", "starting", "running"].indexOf(state(task)) >= 0;
    }
    function rowStatus(task) {
        if (attention(task) || (live(task) && !canStop(task)))
            return KetTheme.TaskStatus.Stopping;
        return ({
                queued: KetTheme.TaskStatus.Running,
                starting: KetTheme.TaskStatus.Running,
                running: KetTheme.TaskStatus.Running,
                stopping: KetTheme.TaskStatus.Stopping,
                completed: KetTheme.TaskStatus.Completed,
                failed: KetTheme.TaskStatus.Failed,
                timed_out: KetTheme.TaskStatus.Failed
            })[state(task)] ?? KetTheme.TaskStatus.Stopped;
    }
    function details(task) {
        const lines = [];
        if (task.containment_mode === "native_best_effort")
            lines.push(task.cleanup_status === "best_effort_complete" ? "Native macOS · best effort complete. Unobserved descendants may remain." : "Native macOS · best effort. Cleanup is not guaranteed.");
        if (attention(task)) {
            lines.push("Cleanup unverified. " + (task.manual_guidance || "Check the command and its descendants, then retry cleanup or acknowledge your manual check."));
            if (task.exit_code !== undefined && task.exit_code !== null)
                lines.push("Root exit code: " + task.exit_code + " · cleanup remains unverified");
        } else if (task.reason === "cleanup_acknowledged")
            lines.push("Cleanup acknowledged by user · assurance remains unverified");
        const completion = task.completion || ({});
        if (completion.status)
            lines.push(({
                    pending: "Agent follow-up pending",
                    delivered: "Agent follow-up delivered",
                    suppressed: "Agent follow-up suppressed"
                })[completion.status] + (completion.reason ? " · " + completion.reason : ""));
        if (task.schema_version !== undefined)
            lines.push("Task " + task.task_id);
        return lines.join("\n");
    }
    function confirmCleanup(task) {
        cleanupDialog.taskId = task.task_id;
        cleanupDialog.taskRevision = task.revisionToken || String(task.revision);
        cleanupDialog.open();
    }
    function plural(count, word) {
        return count + " " + word + (count === 1 ? "" : "s");
    }
    function elapsed(task) {
        const ms = task.duration_ms !== undefined && !live(task) ? task.duration_ms : task.started_at ? (task.ended_at || bar.now) - task.started_at : 0;
        const seconds = Math.max(0, Math.round(ms / 1000));
        return seconds < 60 ? seconds + "s" : Math.floor(seconds / 60) + "m " + (seconds % 60 < 10 ? "0" : "") + seconds % 60 + "s";
    }
    function tokens(count) {
        return count >= 1000 ? Math.round(count / 1000) + "k" : String(count);
    }
    function meta(task) {
        if (state(task) === "stopping")
            return "Stopping…";
        if (attention(task))
            return "Cleanup unverified · action needed";
        if (state(task) === "queued")
            return "Queued · waiting to start";
        if (state(task) === "starting")
            return "Starting…";
        if (!live(task)) {
            // As the mock words a finished task: what it reported, else how long it took.
            const reported = task.reason === "worker_restart" ? "KetPlus restarted while it ran" : task.reason === "session_closed" ? "Ended with its conversation" : task.error || task.result || "";
            if (reported.length > 0)
                return reported;
            return ({
                    completed: "Completed in ",
                    failed: "Failed after ",
                    stopped: "Stopped after ",
                    cancelled: "Cancelled after ",
                    timed_out: "Timed out after ",
                    interrupted: "Interrupted after "
                })[state(task)] + elapsed(task);
        }
        if (task.kind === "agent")
            return "Subagent · " + plural(task.tool_uses || 0, "tool") + " · " + tokens(task.total_tokens || 0) + " tokens · " + elapsed(task);
        return "Shell · " + elapsed(task);
    }
    function statusColor(task) {
        if (bar.offline && live(task))
            return KetTheme.t.textMuted;
        return ({
                running: KetTheme.t.accent,
                stopping: KetTheme.t.warning,
                completed: KetTheme.t.positive,
                failed: KetTheme.t.danger,
                stopped: KetTheme.t.textMuted,
                cancelled: KetTheme.t.textMuted,
                timed_out: KetTheme.t.danger,
                interrupted: KetTheme.t.warning,
                needs_attention: KetTheme.t.warning
            })[state(task)] || KetTheme.t.textMuted;
    }
    function openTask(task) {
        if (pane.taskId === task.task_id)
            agentHost.closeBackgroundTaskOutput();
        else
            agentHost.requestBackgroundTaskOutput(task.task_id);
    }
    readonly property var openedTask: tasks.find(task => task.task_id === pane.taskId)
    // The kit's toggle (`.bg-toggle`) counts every task; the rows show which still run.
    readonly property string headline: attentionCount > 0 ? plural(attentionCount, "task") + " · cleanup needs attention" : offline ? plural(liveCount, "background task") + " · last known" : liveCount > 0 ? (agentHost.sessionRunning ? "" : "Turn finished · ") + plural(tasks.length, "background task") : "Background tasks finished"

    objectName: "backgroundTaskBar"
    visible: tasks.length > 0 || !!registry.error || !!registry.loading
    implicitHeight: visible ? column.implicitHeight : 0
    // The mock's strip has no panel of its own: the toggle, then one row per task.
    color: "transparent"
    Accessible.role: Accessible.Grouping
    Accessible.name: "Background tasks"

    function revealTaskFocus() {
        const hostWindow = bar.Window.window;
        const focused = hostWindow ? hostWindow.activeFocusItem : null;
        let ancestor = focused;
        while (ancestor && ancestor !== taskRows)
            ancestor = ancestor.parent;
        if (!ancestor)
            return;
        const flick = taskScroll.contentItem;
        const top = focused.mapToItem(taskRows, 0, 0).y;
        const bottom = top + focused.height;
        const desired = top < flick.contentY ? top : bottom > flick.contentY + flick.height ? bottom - flick.height : flick.contentY;
        flick.contentY = Math.max(0, Math.min(desired, flick.contentHeight - flick.height));
    }
    Connections {
        target: bar.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(bar.revealTaskFocus);
        }
    }

    Timer {
        // Elapsed time ticks locally; the daemon reports progress only every few seconds.
        objectName: "backgroundTaskElapsedTimer"
        running: bar.windowVisible && bar.tickingCount > 0 && !bar.offline
        interval: 1000
        repeat: true
        onTriggered: bar.now = Date.now()
    }
    Timer {
        running: bar.windowVisible && !bar.offline && bar.openedTask !== undefined && (bar.openedTask.kind !== "agent" || bar.openedTask.schema_version !== undefined) && bar.live(bar.openedTask) && !bar.attention(bar.openedTask) && !bar.pane.loading && !bar.pane.error
        interval: 2000
        onTriggered: agentHost.requestBackgroundTaskOutput(bar.pane.taskId)
    }
    onTasksChanged: {
        if (confirmingStop && !tasks.some(task => task.task_id === confirmingStop && bar.canStop(task)))
            confirmingStop = "";
        if (cleanupDialog.visible && !tasks.some(task => task.task_id === cleanupDialog.taskId && attention(task)))
            cleanupDialog.reject();
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: bar.barRecipe.rowGap

        AbstractButton {
            id: toggle
            objectName: "backgroundTaskBarHeader"
            readonly property var recipe: KetTheme.taskBarRecipe(KetTheme.dark, KetTheme.zoom, hovered, down, enabled, visualFocus)
            Layout.maximumWidth: parent.width
            hoverEnabled: true
            opacity: recipe.opacity
            leftPadding: recipe.paddingInline
            rightPadding: recipe.paddingInline
            implicitWidth: leftPadding + rightPadding + toggleContent.implicitWidth
            implicitHeight: recipe.height
            Accessible.name: bar.headline + (bar.collapsed ? ", collapsed" : ", expanded")
            onClicked: bar.collapsed = !bar.collapsed
            contentItem: Row {
                id: toggleContent
                spacing: toggle.recipe.spacing
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: bar.attentionCount > 0 ? "warning" : bar.liveCount > 0 ? "loading" : KetTheme.iconForLucide("circle-check")
                    box: toggle.recipe.iconSize
                    color: toggle.recipe.text
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, toggle.Layout.maximumWidth - toggle.leftPadding - toggle.rightPadding - 2 * toggle.recipe.spacing - toggle.recipe.iconSize - toggle.recipe.chevronSize)
                    text: bar.headline
                    color: toggle.recipe.text
                    font.family: KetTheme.fontFamily
                    font.pixelSize: toggle.recipe.fontPixels
                    elide: Text.ElideRight
                }
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: bar.collapsed ? "chevron-up" : "chevron-down"
                    box: toggle.recipe.chevronSize
                    color: toggle.recipe.text
                }
            }
            background: Rectangle {
                radius: toggle.recipe.radius
                color: toggle.recipe.background
                border.width: toggle.recipe.focusRing.a > 0 ? 2 : 0
                border.color: toggle.recipe.focusRing
            }
        }

        ScrollView {
            id: taskScroll
            objectName: "backgroundTaskRows"
            Layout.fillWidth: true
            Layout.preferredHeight: bar.collapsed ? 0 : Math.min(taskRows.implicitHeight, KetTheme.px(bar.openedTask ? 160 : 240, KetTheme.zoom))
            visible: !bar.collapsed
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                id: taskRows
                width: taskScroll.availableWidth
                spacing: bar.barRecipe.rowGap
                Repeater {
                    model: bar.collapsed ? [] : bar.tasks
                    delegate: BackgroundTaskItem {
                        taskBar: bar
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !bar.collapsed && !!bar.registry.nextCursor
            text: (bar.registry.activeCount !== undefined ? plural(bar.registry.activeCount, "active task") + " · " : "") + plural(bar.tasks.length, "task") + " shown · more available"
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            wrapMode: Text.Wrap
        }
        Flow {
            Layout.fillWidth: true
            visible: !bar.collapsed && (!!bar.registry.nextCursor || !!bar.registry.error || !!bar.registry.loading)
            spacing: KetTheme.px(4, KetTheme.zoom)
            KetButton {
                objectName: "backgroundTasksLoadMore"
                visible: !!bar.registry.nextCursor
                text: "Load more tasks"
                enabled: !bar.offline && bar.canRead && !bar.registry.loading
                controlSize: KetTheme.ControlSize.Small
                onClicked: agentHost.loadBackgroundTasks(true)
            }
            KetButton {
                objectName: "backgroundTasksRetry"
                visible: !!bar.registry.error
                text: "Retry task list"
                enabled: !bar.offline && bar.canRead && !bar.registry.loading
                controlSize: KetTheme.ControlSize.Small
                onClicked: agentHost.loadBackgroundTasks(false)
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !bar.collapsed && (!!bar.registry.error || !!bar.registry.loading)
            text: bar.registry.error || "Loading task registry…"
            textFormat: Text.PlainText
            color: bar.registry.error ? KetTheme.t.danger : KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            wrapMode: Text.Wrap
        }

        Rectangle {
            id: outputPane
            objectName: "backgroundTaskOutput"
            readonly property var task: bar.openedTask
            visible: !bar.collapsed && task !== undefined
            Layout.fillWidth: true
            Layout.topMargin: KetTheme.px(4, KetTheme.zoom)
            Layout.preferredHeight: visible ? paneColumn.implicitHeight + KetTheme.px(16, KetTheme.zoom) : 0
            radius: KetTheme.px(8, KetTheme.zoom)
            color: KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom).background
            border.width: 1
            border.color: KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom).border

            ColumnLayout {
                id: paneColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: KetTheme.px(8, KetTheme.zoom)
                spacing: KetTheme.px(6, KetTheme.zoom)

                Text {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: !outputPane.task ? "" : outputPane.task.kind === "agent" ? [outputPane.task.summary, outputPane.task.last_tool ? "Last tool: " + outputPane.task.last_tool : "", outputPane.task.result || outputPane.task.error || ""].filter(line => line).join("\n") : (outputPane.task.command || "")
                    color: KetTheme.t.textSecondary
                    font.family: outputPane.task && outputPane.task.kind === "agent" ? KetTheme.fontFamily : agentHost.monoFontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
                ScrollView {
                    id: outputScroll
                    visible: outputPane.task !== undefined && (outputPane.task.kind !== "agent" || outputPane.task.schema_version !== undefined)
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(outputText.implicitHeight, KetTheme.px(220, KetTheme.zoom))
                    clip: true
                    TextEdit {
                        id: outputText
                        objectName: "backgroundTaskOutputText"
                        width: outputScroll.availableWidth
                        readOnly: true
                        textFormat: TextEdit.PlainText
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        text: bar.pane.output !== undefined ? (bar.pane.output || "No output yet.") : bar.pane.loading ? "Loading output…" : "No output read."
                        color: bar.pane.error ? KetTheme.t.danger : KetTheme.t.textMain
                        font.family: agentHost.monoFontFamily
                        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                        // New output lands at the bottom; keep it in view as it refreshes.
                        onTextChanged: Qt.callLater(() => {
                            const flick = outputScroll.contentItem;
                            flick.contentY = Math.max(0, flick.contentHeight - flick.height);
                        })
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: !!bar.pane.error
                    text: bar.pane.error || ""
                    textFormat: Text.PlainText
                    color: KetTheme.t.danger
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                    wrapMode: Text.Wrap
                }
                KetButton {
                    objectName: "backgroundTaskOutputRetry"
                    visible: !!bar.pane.error
                    text: "Retry output"
                    controlSize: KetTheme.ControlSize.Small
                    enabled: !bar.offline && bar.canRead && !bar.pane.loading
                    onClicked: agentHost.requestBackgroundTaskOutput(bar.pane.taskId)
                }
                RowLayout {
                    visible: outputScroll.visible && bar.pane.output !== undefined
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: (bar.pane.truncated ? "Some output is no longer retained" : bar.pane.hasMore ? "Output page · more available" : "Output read") + (bar.pane.displayTruncated ? " · showing the latest 64K characters" : "") + (outputPane.task && bar.live(outputPane.task) && !bar.offline ? " · refreshes every 2 s" : "")
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                        elide: Text.ElideRight
                    }
                    KetButton {
                        objectName: "backgroundTaskOutputMore"
                        visible: !!bar.pane.nextCursor
                        text: bar.pane.loading ? "Loading…" : bar.pane.hasMore ? "Read more" : "Refresh"
                        enabled: !bar.offline && bar.canRead && !bar.pane.loading && !bar.pane.error
                        controlSize: KetTheme.ControlSize.Small
                        onClicked: agentHost.requestBackgroundTaskOutput(bar.pane.taskId)
                    }
                    KetButton {
                        variant: KetTheme.ButtonVariant.Ghost
                        text: "Copy output"
                        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                        onClicked: agentHost.copyText(bar.pane.output || "")
                    }
                }
            }
        }

        Text {
            visible: bar.offline && !bar.collapsed
            Layout.fillWidth: true
            Layout.margins: KetTheme.px(4, KetTheme.zoom)
            text: "The daemon is unreachable. These tasks may have ended; Stop and live output return when it reconnects."
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            wrapMode: Text.Wrap
        }
    }
    KetDialog {
        id: cleanupDialog
        objectName: "backgroundTaskCleanupDialog"
        property string taskId: ""
        property string taskRevision: ""
        title: "Acknowledge manual cleanup?"
        Text {
            Layout.fillWidth: true
            text: "Confirm only after you have checked and handled the command and its descendants. This records your acknowledgement; KetPlus cannot verify that every process stopped. It will not restart the agent."
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            wrapMode: Text.Wrap
        }
        actions: [
            KetButton {
                text: "Keep warning"
                variant: KetTheme.ButtonVariant.Ghost
                onClicked: cleanupDialog.reject()
            },
            KetButton {
                objectName: "confirmTaskCleanup"
                text: "I've handled cleanup"
                enabled: !bar.offline && bar.canResolve
                onClicked: {
                    agentHost.resolveBackgroundTaskCleanup(cleanupDialog.taskId, cleanupDialog.taskRevision);
                    cleanupDialog.accept();
                }
            }
        ]
    }
}
