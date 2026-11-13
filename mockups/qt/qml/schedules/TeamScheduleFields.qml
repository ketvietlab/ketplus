// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import "ScheduleRules.js" as Rules

Column {
    id: fields
    property string goal: ""
    property bool tried: false
    property alias scheduled: start.scheduled
    property alias date: start.date
    property alias time: start.time
    readonly property string zone: start.zone
    readonly property var recurringDraft: repeat.draft
    readonly property bool recurring: repeat.selection > 0
    readonly property string workOrder: order.text
    readonly property string problem: start.problem || (recurring ? Rules.error(Object.assign({}, recurringDraft, { name: "Team work order", prompt: workOrder })) : "")
    function reset() { tried = false; start.reset(); repeat.reset(); order.text = Qt.binding(() => fields.goal) }
    function preset(tomorrow) { start.preset(tomorrow) }
    function previewRepeat() { repeat.selection = 1 }
    spacing: KetTheme.px(12, KetTheme.zoom)
    ScheduleText { text: "Schedule"; font.weight: Font.DemiBold }
    TeamStartFields { id: start; width: parent.width; tried: fields.tried }
    ScheduleRepeatFields { id: repeat; width: parent.width }
    KetFieldRow {
        width: parent.width; visible: fields.recurring; label: "Work order each run"; stacked: true
        help: "The leader receives this work order on every run."
        error: fields.tried && !order.text.trim() ? "Work order is required" : ""
        KetTextArea { id: order; width: parent.width; text: fields.goal; minimumLines: 2; maximumLines: 4; Accessible.name: "Work order each run" }
    }
    ScheduleText { visible: fields.tried && fields.problem !== ""; width: parent.width; text: fields.problem; color: KetTheme.t.danger }
}
