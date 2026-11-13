// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

Column {
    id: message
    required property var window
    readonly property var entry: window.scheduleStore.find(window.scheduledRunId)
    width: parent.width
    spacing: KetTheme.px(8, KetTheme.zoom)
    Item { width: 1; height: KetTheme.px(24, KetTheme.zoom) }
    KetButton {
        anchors.right: parent.right
        text: "Scheduled run: " + (message.entry ? message.entry.name : "Schedule") + " · 13/11 08:00"
        iconName: "clock"; variant: KetTheme.ButtonVariant.Ghost
        onClicked: message.window.openSchedules(message.entry ? message.entry.id : "")
    }
    KetUserMessage { width: parent.width; text: message.entry ? message.entry.prompt : "Run the scheduled task." }
    KetAssistantMessage {
        width: parent.width; name: "Leader · Codex"; time: "08:00"
        text: message.entry && message.entry.target === 2 ? "The “Daily website article” schedule has started. Today we will explain how to read CI failures. I will assign research and editing to the team, then send you the draft for approval." : "The scheduled run has started. I will check the CI workflows and report the results in this conversation."
    }
}
