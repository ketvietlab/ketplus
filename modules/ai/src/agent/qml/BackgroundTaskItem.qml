import QtQuick
import Ket
import QtQuick.Layouts

ColumnLayout {
    id: row
    required property var modelData
    required property var taskBar
    readonly property var action: (agentHost.backgroundTaskActions || {})[modelData.task_id] || ({})
    readonly property bool confirming: taskBar.confirmingStop === modelData.task_id
    // Notes under a card line up with its content.
    readonly property real noteInset: taskRow.recipe.paddingStart
    readonly property real notePixels: taskRow.recipe.metaPixels
    Layout.fillWidth: true
    spacing: KetTheme.px(4, KetTheme.zoom)
    objectName: "backgroundTask_" + modelData.task_id
    KetTaskRow {
        id: taskRow
        objectName: "backgroundTaskCard_" + row.modelData.task_id
        Layout.fillWidth: true
        // A row opens its output or the subagent's progress, as the mock's
        // current row; Stop and Dismiss stay on top of it.
        readonly property string openLabel: (taskBar.pane.taskId === row.modelData.task_id ? "Hide " : "Show ") + (row.modelData.kind === "agent" ? "progress" : "output")
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: row.modelData.title + ", " + taskBar.meta(row.modelData)
        Accessible.description: openLabel
        Accessible.onPressAction: taskBar.openTask(row.modelData)
        // Only the row's own focus opens it: Return or Space on Stop or
        // Dismiss reaches here too and must keep that button's meaning.
        function activateFromKeys(event) {
            if (!taskRow.activeFocus) {
                event.accepted = false;
                return;
            }
            taskBar.openTask(row.modelData);
        }
        Keys.onReturnPressed: event => activateFromKeys(event)
        Keys.onSpacePressed: event => activateFromKeys(event)
        // The keyboard focus ring sits outside the row, apart from the
        // `current` border that marks the open output.
        Rectangle {
            objectName: "backgroundTaskFocusRing"
            anchors.fill: parent
            anchors.margins: -2
            z: 1
            visible: taskRow.activeFocus
            radius: taskRow.radius + 2
            color: "transparent"
            border.width: 2
            border.color: KetTheme.t.focusBorder
        }
        MouseArea {
            objectName: "backgroundTaskOpen"
            anchors.fill: parent
            z: -1
            cursorShape: Qt.PointingHandCursor
            onClicked: taskBar.openTask(row.modelData)
        }
        enabled: !taskBar.live(row.modelData) || (!taskBar.offline && taskBar.canControl && !row.action.pending)
        title: row.modelData.title
        meta: row.action.pending ? "Requesting stop…" : taskBar.meta(row.modelData)
        kindIcon: row.modelData.kind === "agent" ? "bot" : "terminal"
        current: taskBar.pane.taskId === row.modelData.task_id
        status: taskBar.rowStatus(row.modelData)
        onStopRequested: if (taskBar.canStop(row.modelData) && !row.action.pending)
            taskBar.confirmingStop = row.modelData.task_id
        onDismissRequested: {
            if (taskBar.live(row.modelData))
                return;
            const key = taskBar.taskKey(row.modelData);
            if (taskBar.pane.taskId === row.modelData.task_id)
                agentHost.closeBackgroundTaskOutput();
            // Updating the model can destroy this delegate immediately.
            taskBar.dismissedTasks = taskBar.dismissedTasks.concat([key]);
        }
    }
    Flow {
        visible: !taskBar.canControl && taskBar.canRead
        Layout.fillWidth: true
        spacing: KetTheme.px(4, KetTheme.zoom)
        KetButton {
            text: taskBar.pane.taskId === row.modelData.task_id ? "Hide output" : "Show output"
            controlSize: KetTheme.ControlSize.Small
            enabled: !taskBar.offline
            onClicked: taskBar.openTask(row.modelData)
        }
    }
    Text {
        objectName: "backgroundTaskActionError_" + row.modelData.task_id
        Layout.fillWidth: true
        visible: !!row.action.error
        leftPadding: row.noteInset
        text: row.action.error || ""
        textFormat: Text.PlainText
        color: KetTheme.t.danger
        font.family: KetTheme.fontFamily
        font.pixelSize: row.notePixels
        wrapMode: Text.Wrap
    }
    // A note on this task at the card's meta size, in the muted note colour: a failed card's
    // red meta line is not the colour of its follow-up notes.
    Text {
        objectName: "backgroundTaskDetails_" + row.modelData.task_id
        Layout.fillWidth: true
        visible: text.length > 0
        leftPadding: row.noteInset
        rightPadding: taskRow.recipe.paddingEnd
        text: taskBar.details(row.modelData)
        textFormat: Text.PlainText
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: row.notePixels
        wrapMode: Text.Wrap
    }
    Text {
        visible: row.confirming
        Layout.fillWidth: true
        leftPadding: row.noteInset
        text: "Stop “" + row.modelData.title + "”? " + (row.modelData.kind === "agent" ? "The subagent stops after its current step." : "The command is terminated. Output so far stays readable.")
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: row.notePixels
        wrapMode: Text.Wrap
    }
    RowLayout {
        Layout.leftMargin: row.noteInset
        visible: row.confirming
        KetButton {
            text: "Keep running"
            controlSize: KetTheme.ControlSize.Small
            onClicked: taskBar.confirmingStop = ""
        }
        KetButton {
            objectName: "confirmStopBackgroundTask"
            text: "Stop task"
            enabled: !taskBar.offline && taskBar.canControl && !row.action.pending
            controlSize: KetTheme.ControlSize.Small
            variant: KetTheme.ButtonVariant.Danger
            onClicked: {
                taskBar.confirmingStop = "";
                agentHost.stopBackgroundTask(row.modelData.task_id);
            }
        }
    }
}
