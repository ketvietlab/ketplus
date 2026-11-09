import QtQuick
import Ket
import QtQuick.Controls
import QtQuick.Layouts

// Shells and subagents the provider keeps running past a turn, as the daemon reports them.
// The daemon owns the list; this only shows it, opens one task's output and asks to stop one.
Rectangle {
    id: bar
    required property var workspace

    property var dismissedTasks: []
    function taskKey(task) {
        return workspace.transcriptScope + ":" + String(task.task_id) + ":" + String(task.started_at || "") + ":" + String(task.ended_at || "");
    }
    readonly property var tasks: (agentHost.backgroundTasks || []).filter(task => live(task) || dismissedTasks.indexOf(taskKey(task)) < 0)
    readonly property var barRecipe: KetTheme.taskBarRecipe(KetTheme.dark, KetTheme.zoom, false, false, true, false)
    readonly property var pane: agentHost.backgroundTaskOutput || ({})
    readonly property bool offline: agentHost.presentationState.connection === "unavailable"
    readonly property int liveCount: tasks.filter(task => bar.live(task)).length
    property bool collapsed: false
    property string confirmingStop: ""
    property real now: Date.now()
    readonly property bool windowVisible: visible && Window.window !== null
        && Window.window.visible && Window.window.visibility !== Window.Minimized

    function live(task) {
        return task.status === "running" || task.status === "stopping";
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
        if (task.status === "stopping")
            return "Stopping…";
        if (!live(task)) {
            const outcome = task.reason === "worker_restart" ? "KetPlus restarted while it ran" : task.reason === "session_closed" ? "Ended with its conversation" : task.error || task.result || ({
                    completed: "Completed",
                    failed: "Failed",
                    stopped: "Stopped"
                })[task.status];
            return outcome + " · " + elapsed(task);
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
                stopped: KetTheme.t.textMuted
            })[task.status] || KetTheme.t.textMuted;
    }
    function openTask(task) {
        if (pane.taskId === task.task_id)
            agentHost.closeBackgroundTaskOutput();
        else
            agentHost.requestBackgroundTaskOutput(task.task_id);
    }
    readonly property var openedTask: tasks.find(task => task.task_id === pane.taskId)
    // The kit's toggle (`.bg-toggle`) counts every task; the rows show which still run.
    readonly property string headline: offline ? plural(liveCount, "background task") + " · last known" : liveCount > 0 ? (agentHost.sessionRunning ? "" : "Turn finished · ") + plural(tasks.length, "background task") : "Background tasks finished"

    objectName: "backgroundTaskBar"
    visible: tasks.length > 0
    implicitHeight: visible ? column.implicitHeight : 0
    // The mock's strip has no panel of its own: the toggle, then one row per task.
    color: "transparent"
    Accessible.role: Accessible.Grouping
    Accessible.name: "Background tasks"

    Timer {
        // Elapsed time ticks locally; the daemon reports progress only every few seconds.
        objectName: "backgroundTaskElapsedTimer"
        running: bar.windowVisible && bar.liveCount > 0 && !bar.offline
        interval: 1000
        repeat: true
        onTriggered: bar.now = Date.now()
    }
    Timer {
        running: bar.windowVisible && !bar.offline && bar.openedTask !== undefined && bar.openedTask.kind !== "agent" && bar.live(bar.openedTask) && !bar.pane.loading
        interval: 2000
        onTriggered: agentHost.requestBackgroundTaskOutput(bar.pane.taskId)
    }
    onTasksChanged: if (confirmingStop && !tasks.some(task => task.task_id === confirmingStop && task.status === "running"))
        confirmingStop = ""

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
                    name: bar.liveCount > 0 ? "loading" : KetTheme.iconForLucide("circle-check")
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

        Repeater {
            model: bar.collapsed ? [] : bar.tasks
            delegate: ColumnLayout {
                id: row
                required property var modelData
                readonly property bool confirming: bar.confirmingStop === modelData.task_id
                Layout.fillWidth: true
                spacing: KetTheme.px(4, KetTheme.zoom)
                objectName: "backgroundTask_" + modelData.task_id
                KetTaskRow {
                        id: taskRow
                        objectName: "backgroundTaskCard_" + row.modelData.task_id
                        Layout.fillWidth: true
                        // A row opens its output or the subagent's progress, as the mock's
                        // current row; Stop and Dismiss stay on top of it.
                        readonly property string openLabel: (bar.pane.taskId === row.modelData.task_id ? "Hide " : "Show ") + (row.modelData.kind === "agent" ? "progress" : "output")
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: row.modelData.title + ", " + bar.meta(row.modelData)
                        Accessible.description: openLabel
                        Accessible.onPressAction: bar.openTask(row.modelData)
                        Keys.onReturnPressed: bar.openTask(row.modelData)
                        Keys.onSpacePressed: bar.openTask(row.modelData)
                        MouseArea {
                            objectName: "backgroundTaskOpen"
                            anchors.fill: parent
                            z: -1
                            cursorShape: Qt.PointingHandCursor
                            onClicked: bar.openTask(row.modelData)
                        }
                        enabled: !bar.offline || !bar.live(row.modelData)
                        title: row.modelData.title
                        meta: bar.meta(row.modelData)
                        kindIcon: row.modelData.kind === "agent" ? "bot" : "terminal"
                        current: bar.pane.taskId === row.modelData.task_id
                        status: ({
                                running: KetTheme.TaskStatus.Running,
                                stopping: KetTheme.TaskStatus.Stopping,
                                completed: KetTheme.TaskStatus.Completed,
                                failed: KetTheme.TaskStatus.Failed,
                                stopped: KetTheme.TaskStatus.Stopped
                            })[row.modelData.status] ?? KetTheme.TaskStatus.Stopped
                        onStopRequested: if (!bar.offline && row.modelData.status === "running")
                            bar.confirmingStop = row.modelData.task_id
                        onDismissRequested: {
                            if (bar.live(row.modelData))
                                return;
                            const key = bar.taskKey(row.modelData);
                            if (bar.pane.taskId === row.modelData.task_id)
                                agentHost.closeBackgroundTaskOutput();
                            // Updating the model can destroy this delegate immediately.
                            bar.dismissedTasks = bar.dismissedTasks.concat([key]);
                        }
                }
                Text {
                    visible: row.confirming
                    Layout.fillWidth: true
                    text: "Stop “" + row.modelData.title + "”? " + (row.modelData.kind === "agent" ? "The subagent stops after its current step." : "The command is terminated. Output so far stays readable.")
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    visible: row.confirming
                    KetButton {
                        text: "Keep running"
                        controlSize: KetTheme.ControlSize.Small
                        onClicked: bar.confirmingStop = ""
                    }
                    KetButton {
                        objectName: "confirmStopBackgroundTask"
                        text: "Stop task"
                        enabled: !bar.offline
                        controlSize: KetTheme.ControlSize.Small
                        variant: KetTheme.ButtonVariant.Danger
                        onClicked: {
                            bar.confirmingStop = "";
                            agentHost.stopBackgroundTask(row.modelData.task_id);
                        }
                    }
                }
            }
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
                    visible: outputPane.task !== undefined && outputPane.task.kind !== "agent"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(outputText.implicitHeight, KetTheme.px(220, KetTheme.zoom))
                    clip: true
                    TextEdit {
                        id: outputText
                        width: outputScroll.availableWidth
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        text: bar.pane.error ? bar.pane.error : bar.pane.output !== undefined ? (bar.pane.output || "No output yet.") : "Loading output…"
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
                RowLayout {
                    visible: outputScroll.visible && bar.pane.output !== undefined && !bar.pane.error
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: (bar.pane.truncated ? "Last 8 KiB of " + Math.round((bar.pane.totalBytes || 0) / 1024) + " KiB" : "Full output") + (outputPane.task && bar.live(outputPane.task) && !bar.offline ? " · refreshes every 2 s" : "")
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                        elide: Text.ElideRight
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
}
