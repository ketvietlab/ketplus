import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shells and subagents the provider keeps running past a turn, as the daemon reports them.
// The daemon owns the list; this only shows it, opens one task's output and asks to stop one.
Rectangle {
    id: bar
    required property var workspace
    readonly property var t: workspace.t
    readonly property var tasks: agentHost.backgroundTasks || []
    readonly property var pane: agentHost.backgroundTaskOutput || ({})
    readonly property bool offline: agentHost.presentationState.connection === "unavailable"
    readonly property int liveCount: tasks.filter(task => bar.live(task)).length
    property bool collapsed: false
    property string confirmingStop: ""
    property real now: Date.now()

    function live(task) { return task.status === "running" || task.status === "stopping" }
    function plural(count, word) { return count + " " + word + (count === 1 ? "" : "s") }
    function elapsed(task) {
        const ms = task.duration_ms !== undefined && !live(task) ? task.duration_ms
                 : task.started_at ? (task.ended_at || bar.now) - task.started_at : 0
        const seconds = Math.max(0, Math.round(ms / 1000))
        return seconds < 60 ? seconds + "s"
             : Math.floor(seconds / 60) + "m " + (seconds % 60 < 10 ? "0" : "") + seconds % 60 + "s"
    }
    function tokens(count) { return count >= 1000 ? Math.round(count / 1000) + "k" : String(count) }
    function meta(task) {
        if (task.status === "stopping")
            return "Stopping…"
        if (!live(task)) {
            const outcome = task.reason === "worker_restart" ? "KetPlus restarted while it ran"
                          : task.reason === "session_closed" ? "Ended with its conversation"
                          : task.error || task.result
                            || ({completed: "Completed", failed: "Failed", stopped: "Stopped"})[task.status]
            return outcome + " · " + elapsed(task)
        }
        if (task.kind === "agent")
            return "Subagent · " + plural(task.tool_uses || 0, "tool") + " · "
                   + tokens(task.total_tokens || 0) + " tokens · " + elapsed(task)
        return "Shell · " + elapsed(task)
    }
    function statusColor(task) {
        if (bar.offline && live(task))
            return t.textMuted
        return ({running: t.accent, stopping: t.warning, completed: t.positive,
                 failed: t.danger, stopped: t.textMuted})[task.status] || t.textMuted
    }
    function openTask(task) {
        if (pane.taskId === task.task_id)
            agentHost.closeBackgroundTaskOutput()
        else
            agentHost.requestBackgroundTaskOutput(task.task_id)
    }
    readonly property var openedTask: tasks.find(task => task.task_id === pane.taskId)
    readonly property string headline: offline
        ? plural(liveCount, "background task") + " · last known"
        : liveCount > 0 ? (agentHost.sessionRunning ? "" : "Turn finished · ")
                          + plural(liveCount, "task") + " running in background"
                        : "Background tasks finished"

    objectName: "backgroundTaskBar"
    visible: tasks.length > 0
    implicitHeight: visible ? column.implicitHeight + Metrics.px(12, t) : 0
    radius: Metrics.px(12, t)
    color: t.surfaceRaised
    border.width: 1
    border.color: t.border
    Accessible.role: Accessible.Grouping
    Accessible.name: "Background tasks"

    Timer {
        // Elapsed time ticks locally; the daemon reports progress only every few seconds.
        running: bar.visible && bar.liveCount > 0 && !bar.offline
        interval: 1000
        repeat: true
        onTriggered: bar.now = Date.now()
    }
    Timer {
        running: bar.visible && !bar.offline && bar.openedTask !== undefined
                 && bar.openedTask.kind !== "agent" && bar.live(bar.openedTask) && !bar.pane.loading
        interval: 2000
        onTriggered: agentHost.requestBackgroundTaskOutput(bar.pane.taskId)
    }
    onTasksChanged: if (confirmingStop && !tasks.some(task => task.task_id === confirmingStop
                                                             && task.status === "running"))
                        confirmingStop = ""

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Metrics.px(6, bar.t)
        spacing: Metrics.px(2, bar.t)

        AbstractButton {
            objectName: "backgroundTaskBarHeader"
            Layout.fillWidth: true
            implicitHeight: Metrics.px(26, bar.t)
            Accessible.name: bar.headline + (bar.collapsed ? ", collapsed" : ", expanded")
            onClicked: bar.collapsed = !bar.collapsed
            contentItem: RowLayout {
                spacing: Metrics.px(6, bar.t)
                TintedIcon {
                    Layout.leftMargin: Metrics.px(4, bar.t)
                    Layout.preferredWidth: Metrics.px(14, bar.t)
                    Layout.preferredHeight: Metrics.px(14, bar.t)
                    source: bar.collapsed ? "qrc:/agent/icons/chevron-right.svg"
                                          : "qrc:/agent/icons/chevron-down.svg"
                    color: bar.t.textMuted
                }
                Text {
                    Layout.fillWidth: true
                    text: bar.headline
                    color: bar.t.textSecondary
                    font.family: bar.t.uiFont
                    font.pixelSize: bar.t.caption
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
            }
            background: Rectangle {
                radius: Metrics.px(7, bar.t)
                color: parent.hovered ? bar.t.surfaceHover : "transparent"
            }
        }

        Repeater {
            model: bar.collapsed ? [] : bar.tasks
            delegate: RowLayout {
                id: row
                required property var modelData
                readonly property bool confirming: bar.confirmingStop === modelData.task_id
                Layout.fillWidth: true
                Layout.leftMargin: Metrics.px(6, bar.t)
                spacing: Metrics.px(8, bar.t)
                objectName: "backgroundTask_" + modelData.task_id

                Rectangle {
                    Layout.preferredWidth: Metrics.px(8, bar.t)
                    Layout.preferredHeight: Metrics.px(8, bar.t)
                    radius: width / 2
                    color: bar.statusColor(row.modelData)
                    SequentialAnimation on opacity {
                        running: row.modelData.status === "running" && !bar.offline
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.35; duration: 700 }
                        NumberAnimation { to: 1; duration: 700 }
                    }
                }
                TintedIcon {
                    Layout.preferredWidth: Metrics.px(14, bar.t)
                    Layout.preferredHeight: Metrics.px(14, bar.t)
                    source: row.modelData.kind === "agent" ? "qrc:/agent/icons/bot.svg"
                                                           : "qrc:/agent/icons/square-terminal.svg"
                    color: bar.t.textMuted
                }
                Column {
                    Layout.fillWidth: true
                    Text {
                        width: parent.width
                        text: row.confirming ? "Stop “" + row.modelData.title + "”?"
                                             : row.modelData.title
                        color: bar.t.textMain
                        font.family: bar.t.uiFont
                        font.pixelSize: bar.t.body
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: row.confirming
                              ? (row.modelData.kind === "agent"
                                 ? "The subagent stops after its current step."
                                 : "The command is terminated. Output so far stays readable.")
                              : bar.meta(row.modelData)
                        color: bar.t.textMuted
                        font.family: bar.t.uiFont
                        font.pixelSize: bar.t.caption
                        elide: Text.ElideRight
                    }
                }
                Button {
                    visible: !row.confirming
                    flat: true
                    text: bar.pane.taskId === row.modelData.task_id ? "Hide"
                        : row.modelData.kind === "agent" ? "Progress" : "Output"
                    font.pixelSize: bar.t.caption
                    onClicked: bar.openTask(row.modelData)
                }
                Button {
                    visible: !row.confirming && row.modelData.status === "running"
                    enabled: !bar.offline
                    flat: true
                    text: "Stop"
                    font.pixelSize: bar.t.caption
                    Accessible.name: "Stop " + row.modelData.title
                    ToolTip.visible: hovered && bar.offline
                    ToolTip.text: "Stopping needs a live connection to the daemon."
                    onClicked: bar.confirmingStop = row.modelData.task_id
                }
                Button {
                    visible: row.confirming
                    flat: true
                    text: "Keep running"
                    font.pixelSize: bar.t.caption
                    onClicked: bar.confirmingStop = ""
                }
                Button {
                    objectName: "confirmStopBackgroundTask"
                    visible: row.confirming
                    text: "Stop task"
                    font.pixelSize: bar.t.caption
                    palette.buttonText: bar.t.danger
                    onClicked: {
                        bar.confirmingStop = ""
                        agentHost.stopBackgroundTask(row.modelData.task_id)
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
            Layout.topMargin: Metrics.px(4, bar.t)
            Layout.preferredHeight: visible ? paneColumn.implicitHeight + Metrics.px(16, bar.t) : 0
            radius: Metrics.px(8, bar.t)
            color: bar.t.codeBlockBackground
            border.width: 1
            border.color: bar.t.codeBlockBorder

            ColumnLayout {
                id: paneColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Metrics.px(8, bar.t)
                spacing: Metrics.px(6, bar.t)

                Text {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: !outputPane.task ? ""
                        : outputPane.task.kind === "agent"
                          ? [outputPane.task.summary,
                             outputPane.task.last_tool ? "Last tool: " + outputPane.task.last_tool : "",
                             outputPane.task.result || outputPane.task.error || ""]
                            .filter(line => line).join("\n")
                          : (outputPane.task.command || "")
                    color: bar.t.textSecondary
                    font.family: outputPane.task && outputPane.task.kind === "agent" ? bar.t.uiFont
                                                                                    : bar.t.monoFont
                    font.pixelSize: bar.t.caption
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
                ScrollView {
                    id: outputScroll
                    visible: outputPane.task !== undefined && outputPane.task.kind !== "agent"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(outputText.implicitHeight, Metrics.px(220, bar.t))
                    clip: true
                    TextEdit {
                        id: outputText
                        width: outputScroll.availableWidth
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        text: bar.pane.error ? bar.pane.error
                            : bar.pane.output !== undefined ? (bar.pane.output || "No output yet.")
                                                            : "Loading output…"
                        color: bar.pane.error ? bar.t.danger : bar.t.textMain
                        font.family: bar.t.monoFont
                        font.pixelSize: bar.t.caption
                        // New output lands at the bottom; keep it in view as it refreshes.
                        onTextChanged: Qt.callLater(() => {
                            const flick = outputScroll.contentItem
                            flick.contentY = Math.max(0, flick.contentHeight - flick.height)
                        })
                    }
                }
                RowLayout {
                    visible: outputScroll.visible && bar.pane.output !== undefined && !bar.pane.error
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: (bar.pane.truncated ? "Last 8 KiB of " + Math.round((bar.pane.totalBytes || 0) / 1024) + " KiB"
                                                  : "Full output")
                              + (outputPane.task && bar.live(outputPane.task) && !bar.offline
                                 ? " · refreshes every 2 s" : "")
                        color: bar.t.textMuted
                        font.family: bar.t.uiFont
                        font.pixelSize: bar.t.caption
                        elide: Text.ElideRight
                    }
                    Button {
                        flat: true
                        text: "Copy output"
                        font.pixelSize: bar.t.caption
                        onClicked: agentHost.copyText(bar.pane.output || "")
                    }
                }
            }
        }

        Text {
            visible: bar.offline && !bar.collapsed
            Layout.fillWidth: true
            Layout.margins: Metrics.px(4, bar.t)
            text: "The daemon is unreachable. These tasks may have ended; Stop and live output return when it reconnects."
            color: bar.t.textMuted
            font.family: bar.t.uiFont
            font.pixelSize: bar.t.caption
            wrapMode: Text.Wrap
        }
    }
}
