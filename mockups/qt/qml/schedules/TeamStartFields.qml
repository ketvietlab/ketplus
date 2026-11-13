// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket
import "ScheduleRules.js" as Rules

Column {
    id: fields
    property string startHint: "The leader starts planning at "
    property bool showStartChoice: true
    property bool scheduled: false
    property bool tried: false
    property alias date: dateField.text
    property alias time: timeField.text
    readonly property string zone: Rules.zones[zoneField.currentIndex]
    readonly property string problem: scheduled ? Rules.startError(date, time, zone) : ""
    spacing: KetTheme.px(8, KetTheme.zoom)
    function reset() { scheduled = false; date = "2026-11-13"; time = "22:00"; zoneField.currentIndex = 0; tried = false }
    function setZone(zone) { zoneField.currentIndex = Math.max(0, Rules.zones.indexOf(zone)) }
    function preset(tomorrow) { scheduled = true; date = tomorrow ? "2026-11-14" : "2026-11-13"; time = tomorrow ? "08:00" : "22:00" }
    ScheduleText { visible: fields.showStartChoice; text: "Start"; font.weight: Font.Medium }
    Row {
        visible: fields.showStartChoice
        spacing: KetTheme.px(20, KetTheme.zoom)
        KetRadio { text: "Now"; checked: !fields.scheduled; onClicked: fields.scheduled = false }
        KetRadio { text: "Scheduled"; checked: fields.scheduled; onClicked: fields.scheduled = true }
    }
    Column {
        visible: fields.scheduled
        width: parent.width
        spacing: KetTheme.px(10, KetTheme.zoom)
        Flow {
            width: parent.width
            spacing: KetTheme.px(6, KetTheme.zoom)
            KetButton { text: "Tonight at 22:00"; onClicked: fields.preset(false) }
            KetButton { text: "Tomorrow at 08:00"; onClicked: fields.preset(true) }
            KetButton { text: "Custom"; onClicked: dateField.forceActiveFocus() }
        }
        KetFieldRow {
            width: parent.width; label: "Date"; help: "YYYY-MM-DD · 1 minute to 30 days from now"
            KetTextField { id: dateField; width: parent.width; text: "2026-11-13"; Accessible.name: "Start date" }
        }
        KetFieldRow {
            width: parent.width; label: "Time"
            KetTextField { id: timeField; width: parent.width; text: "22:00"; Accessible.name: "Start time" }
        }
        KetFieldRow {
            width: parent.width; label: "Time zone"
            KetSelect { id: zoneField; width: parent.width; model: Rules.zones; Accessible.name: "Start time zone" }
        }
        ScheduleText {
            width: parent.width
            text: fields.tried && fields.problem ? fields.problem : "→ " + fields.startHint + fields.time
            color: fields.tried && fields.problem ? KetTheme.t.danger : KetTheme.t.textSecondary
            Accessible.role: fields.tried && fields.problem ? Accessible.AlertMessage : Accessible.StaticText
        }
    }
}
