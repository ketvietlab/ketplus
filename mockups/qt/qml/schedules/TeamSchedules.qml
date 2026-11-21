// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native
import "ScheduleRules.js" as Rules

Column {
    id: section
    property string language: "en"
    function copy(source) { return Mock.teamText(section.language, source) }
    function scheduleText(value) {
        const words = { "Once": copy("Once"), "Daily": copy("Daily"), "Weekdays": copy("Weekdays"),
            "Every": copy("Every"), "hours": copy("hours"), "Sun": copy("Sun"), "Mon": copy("Mon"),
            "Tue": copy("Tue"), "Wed": copy("Wed"), "Thu": copy("Thu"), "Fri": copy("Fri"), "Sat": copy("Sat"),
            "at": copy("at"), "today": copy("today"), "tomorrow": copy("tomorrow"), "choose a day": copy("choose a day") }
        return value.replace(/choose a day|\b(Once|Daily|Weekdays|Every|hours|Sun|Mon|Tue|Wed|Thu|Fri|Sat|at|today|tomorrow)\b/g, word => words[word])
    }
    required property var window
    required property var team
    readonly property var entries: team ? window.scheduleStore.forTeam(team.id) : []
    readonly property var start: entries.find(e => e.kind === "team-start") || null
    readonly property var recurring: entries.filter(e => e.kind !== "team-start")
    spacing: KetTheme.px(12, KetTheme.zoom)
    visible: !!team
    Column {
        width: parent.width; visible: !!section.start
        spacing: KetTheme.px(8, KetTheme.zoom)
        KetBanner { width: parent.width; title: section.copy("Starts %1").arg(section.start ? section.scheduleText(Rules.nextLabel(section.start).replace("today ", "today at ").replace("tomorrow ", "tomorrow at ")) : ""); message: section.copy("The leader will start planning at the scheduled time.") }
        Flow {
            width: parent.width; spacing: KetTheme.px(8, KetTheme.zoom)
            KetButton {
                text: section.copy("Start now"); iconName: "resume"
                onClicked: { section.window.scheduleStore.remove(section.start.id); section.window.notify(section.copy("The leader has started planning")) }
            }
            KetButton { text: section.copy("Change time"); onClicked: section.window.editExistingSchedule(section.start) }
            KetButton { text: section.copy("Cancel start"); onClicked: cancelStart.open() }
        }
    }
    RowLayout {
        width: parent.width
        ScheduleText { Layout.fillWidth: true; text: section.copy("Schedules"); font.pixelSize: KetTheme.px(14, KetTheme.zoom); font.weight: Font.DemiBold }
    }
    Repeater {
        model: section.recurring
        Column {
            required property var modelData
            width: parent.width
            spacing: KetTheme.px(6, KetTheme.zoom)
            ScheduleText { width: parent.width; text: modelData.name; font.weight: Font.Medium }
            ScheduleText { width: parent.width; text: section.scheduleText(Rules.summary(modelData)) + (modelData.enabled ? section.copy(" · next: ") + section.scheduleText(Rules.nextLabel(modelData)) : section.copy(" · paused")); color: KetTheme.t.textSecondary }
            Flow {
                width: parent.width; spacing: KetTheme.px(8, KetTheme.zoom)
                KetButton { text: section.copy("Edit"); onClicked: section.window.editExistingSchedule(modelData) }
                KetButton { text: modelData.enabled ? section.copy("Pause") : section.copy("Resume"); onClicked: section.window.scheduleStore.patch(modelData.id, { enabled: !modelData.enabled }) }
                KetButton { text: section.copy("Open in Schedules"); onClicked: section.window.openSchedules(modelData.id, section.team.id) }
            }
        }
    }
    ScheduleText { visible: section.recurring.length === 0; text: section.copy("No recurring schedules for this team yet."); color: KetTheme.t.textSecondary }
    KetDialog {
        id: cancelStart
        title: section.copy("Cancel the scheduled team start?")
        ScheduleText { Layout.fillWidth: true; text: section.copy("The team and its conversations will be kept. Choose Start now when you want the leader to begin.") }
        actions: [
            KetButton { text: section.copy("Keep scheduled start"); onClicked: cancelStart.reject() },
            KetButton { text: section.copy("Cancel start"); variant: KetTheme.ButtonVariant.Danger; onClicked: { section.window.scheduleStore.remove(section.start.id); cancelStart.accept() } }
        ]
    }
}
