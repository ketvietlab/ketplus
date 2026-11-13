// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket
import "ScheduleRules.js" as Rules

Column {
    id: fields
    property bool allowOff: true
    property int selection: 0
    property alias time: timeField.text
    property alias hours: hoursField.text
    property var days: [1,2,3,4,5]
    readonly property string zone: Rules.zones[zonePicker.currentIndex]
    readonly property var draft: ({ rule: selection, time: time, zone: zone, hours: hours, days: days })
    spacing: KetTheme.px(8, KetTheme.zoom)
    function setZone(zone) { zonePicker.currentIndex = Math.max(0, Rules.zones.indexOf(zone)) }
    function reset() { selection = 0; time = "08:00"; hours = "4"; days = [1,2,3,4,5]; zonePicker.currentIndex = 0 }
    KetFieldRow {
        width: parent.width; label: "Repeat"
        KetSelect { id: repeatPicker; width: parent.width; model: fields.allowOff ? ["Off", "Every day", "Weekdays", "Weekly", "Every N hours"] : ["Every day", "Weekdays", "Weekly", "Every N hours"]; currentIndex: fields.allowOff ? fields.selection : Math.max(0, fields.selection - 1); onActivated: index => fields.selection = index + (fields.allowOff ? 0 : 1); Accessible.name: "Repeat" }
    }
    Column {
        width: parent.width; visible: fields.selection > 0; spacing: KetTheme.px(8, KetTheme.zoom)
        Flow {
            visible: fields.selection === 3; width: parent.width; spacing: KetTheme.px(4, KetTheme.zoom)
            Repeater {
                model: [1,2,3,4,5,6,0]
                KetButton {
                    required property int modelData
                    text: Rules.days[modelData]
                    variant: fields.days.includes(modelData) ? KetTheme.ButtonVariant.Primary : KetTheme.ButtonVariant.Secondary
                    Accessible.name: text + (fields.days.includes(modelData) ? ", selected" : ", not selected")
                    onClicked: fields.days = fields.days.includes(modelData) ? fields.days.filter(d => d !== modelData) : fields.days.concat([modelData])
                }
            }
        }
        KetFieldRow {
            width: parent.width; visible: fields.selection === 4; label: "Every N hours"
            KetTextField { id: hoursField; width: parent.width; text: "4"; Accessible.name: "Hours between runs" }
        }
        KetFieldRow {
            width: parent.width; label: "Time"
            KetTextField { id: timeField; width: parent.width; text: "08:00"; Accessible.name: "Repeat time" }
        }
        KetFieldRow {
            width: parent.width; label: "Time zone"
            KetSelect { id: zonePicker; width: parent.width; model: Rules.zones; Accessible.name: "Repeat time zone" }
        }
        ScheduleText { width: parent.width; text: Rules.liveSummary(fields.draft); color: KetTheme.t.accentText; font.pixelSize: KetTheme.px(12, KetTheme.zoom) }
    }
}
