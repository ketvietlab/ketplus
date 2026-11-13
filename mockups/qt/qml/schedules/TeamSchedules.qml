// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket
import KetPlusMock
import "ScheduleRules.js" as Rules

Column {
    id: section
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
        KetBanner { width: parent.width; title: "⏱ Starts " + (section.start ? Rules.nextLabel(section.start).replace("today ", "today at ").replace("tomorrow ", "tomorrow at ") : ""); message: "The leader will start planning at the scheduled time." }
        Flow {
            width: parent.width; spacing: KetTheme.px(8, KetTheme.zoom)
            KetButton {
                text: "Start now"; iconName: "resume"
                onClicked: { section.window.scheduleStore.remove(section.start.id); section.window.notify("The leader has started planning") }
            }
            KetButton { text: "Change time"; onClicked: section.window.editExistingSchedule(section.start) }
            KetButton { text: "Cancel start"; onClicked: cancelStart.open() }
        }
    }
    RowLayout {
        width: parent.width
        ScheduleText { Layout.fillWidth: true; text: "Team schedules"; font.pixelSize: KetTheme.px(14, KetTheme.zoom); font.weight: Font.DemiBold }
        KetButton { text: "Add recurring schedule"; iconName: "plus"; onClicked: section.window.newTeamSchedule(section.team.id) }
    }
    Repeater {
        model: section.recurring
        RowLayout {
            required property var modelData
            width: parent.width
            KetButton {
                Layout.fillWidth: true
                text: modelData.name + " · " + Rules.summary(modelData) + (modelData.enabled ? " · next: " + Rules.nextLabel(modelData) : " · paused")
                variant: KetTheme.ButtonVariant.Ghost
                onClicked: section.window.openSchedules(modelData.id, section.team.id)
            }
            KetCheckbox { checked: modelData.enabled; Accessible.name: "Enable or disable " + modelData.name; onToggled: section.window.scheduleStore.patch(modelData.id, { enabled: checked }) }
        }
    }
    ScheduleText { visible: section.recurring.length === 0; text: "No recurring schedules for this team yet."; color: KetTheme.t.textSecondary }
    KetButton { text: "Team schedules…"; variant: KetTheme.ButtonVariant.Ghost; onClicked: section.window.openSchedules("", section.team.id) }
    KetDialog {
        id: cancelStart
        title: "Cancel the scheduled team start?"
        ScheduleText { Layout.fillWidth: true; text: "The team and its conversations will be kept. Choose Start now when you want the leader to begin." }
        actions: [
            KetButton { text: "Keep scheduled start"; onClicked: cancelStart.reject() },
            KetButton { text: "Cancel start"; variant: KetTheme.ButtonVariant.Danger; onClicked: { section.window.scheduleStore.remove(section.start.id); cancelStart.accept() } }
        ]
    }
}
