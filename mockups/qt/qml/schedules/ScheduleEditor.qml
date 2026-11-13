// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket
import KetPlusMock
import "ScheduleRules.js" as Rules

KetDialog {
    id: editor
    required property var window
    required property var store
    property var original: ({})
    property int target: 0
    property string conversation: "8116"
    property string provider: "codex"
    property string model: "GPT-5.6 Sol"
    property string effort: "Medium"
    property var days: [1,2,3,4,5]
    property bool tried: false
    property bool teamStart: false
    readonly property var teams: window.teamList
    readonly property var selectedTeam: teams[teamPicker.currentIndex] || null
    readonly property var draft: ({
        id: original.id || "", name: nameField.text, prompt: promptField.text,
        target: target, conversation: target === 2 && selectedTeam ? selectedTeam.members[0].number : conversation,
        teamId: target === 2 && selectedTeam ? selectedTeam.id : "",
        teamName: teamPicker.currentText, provider: provider, model: model, effort: effort,
        repository: repoPicker.currentText, worktree: worktree.checked,
        rule: rule.currentIndex, time: timeField.text, date: dateField.text,
        zone: Rules.zones[zonePicker.currentIndex], days: days, hours: hoursField.text,
        busy: busyPicker.currentIndex, missed: missedPicker.currentIndex,
        creator: original.creator || "You", enabled: original.enabled !== false,
        kind: teamStart ? "team-start" : ""
    })
    signal saved(string id)
    function px(v) { return KetTheme.px(v, KetTheme.zoom) }
    function load(entry, requestedTarget, number, once) {
        if (!entry) return
        body.followEnd = false; body.contentY = 0
        original = entry
        teamStart = original.kind === "team-start"
        target = entry ? entry.target : (requestedTarget || 0)
        conversation = entry ? entry.conversation : (number || "8116")
        provider = original.provider || "codex"; model = original.model || "GPT-5.6 Sol"; effort = original.effort || "Medium"
        days = (original.days || [1,2,3,4,5]).slice()
        nameField.text = original.name || ""
        promptField.text = original.prompt || ""
        rule.currentIndex = entry ? entry.rule : (once ? 0 : 1)
        timeField.text = original.time || "08:00"
        dateField.text = original.date || "2026-11-14"
        zonePicker.currentIndex = Math.max(0, Rules.zones.indexOf(original.zone || "Asia/Ho_Chi_Minh"))
        hoursField.text = original.hours || "4"
        busyPicker.currentIndex = original.busy || 0; missedPicker.currentIndex = original.missed || 0
        selectTeam(original.teamId)
        repoPicker.currentIndex = original.repository === "website" ? 1 : 0
        worktree.checked = original.worktree !== false
        tried = false
        open()
    }
    function scrollToEnd() { body.scrollToEnd() }
    function policiesVisible() { return visible && body.fullyShows(busyPicker) && body.fullyShows(missedPicker) }
    function selectTeam(id) { teamPicker.currentIndex = Math.max(0, teams.findIndex(t => t.id === id)) }
    function setPrompt(text) { promptField.text = text }
    function attemptSave() {
        tried = true
        if (Rules.error(draft)) return false
        const id = store.save(draft)
        if (!id) return false
        accept(); saved(id); return true
    }
    title: "Edit schedule"
    width: parent ? Math.min(px(720), parent.width - px(32)) : px(720)
    ScheduleBody {
        id: body
        KetFieldRow {
            width: parent.width; label: "Name"
            error: editor.tried && !nameField.text.trim() ? "Name is required" : ""
            KetTextField { id: nameField; width: parent.width; placeholderText: "Daily website article"; invalid: editor.tried && !text.trim(); Accessible.name: "Schedule name" }
        }
        KetFieldRow {
            width: parent.width; label: "Send to"; stacked: true
            Column {
                width: parent.width; spacing: editor.px(6)
                KetRadio { text: "This conversation · #" + editor.conversation; checked: editor.target === 0; enabled: !editor.teamStart; onClicked: editor.target = 0 }
                KetRadio { text: "New conversation each run"; checked: editor.target === 1; enabled: !editor.teamStart; onClicked: editor.target = 1 }
                KetRadio { text: "Team"; checked: editor.target === 2; onClicked: editor.target = 2 }
            }
        }
        Column {
            width: parent.width; visible: editor.target === 1; spacing: editor.px(10)
            KetFieldRow {
                width: parent.width; label: "Repo"
                KetSelect { id: repoPicker; width: parent.width; model: ["ketplus", "website"]; Accessible.name: "Repository for new conversations" }
            }
            KetFieldRow {
                width: parent.width; label: "Model & context"
                KetButton { width: parent.width; text: (editor.provider === "codex" ? "Codex" : editor.provider === "claude" ? "Claude" : "Cursor") + " · " + editor.model + " · " + editor.effort + "  ▾"; onClicked: modelPicker.open() }
            }
            KetCheckbox { id: worktree; text: "Separate worktree"; checked: true }
        }
        KetFieldRow {
            width: parent.width; visible: editor.target === 2; label: "Team"; help: "→ The leader receives this task"
            KetSelect { id: teamPicker; width: parent.width; model: editor.teams.map(t => t.name); Accessible.name: "Target team" }
        }
        KetFieldRow {
            width: parent.width; label: "Prompt"; stacked: true
            error: editor.tried && !promptField.text.trim() ? "Prompt is required" : ""
            KetTextArea { id: promptField; width: parent.width; minimumLines: 3; maximumLines: 6; placeholderText: "What should the agent do when it is time?"; invalid: editor.tried && !text.trim(); Accessible.name: "Prompt" }
        }
        Column {
            width: parent.width; spacing: editor.px(8)
            ScheduleText { text: "Repeat"; font.weight: Font.Medium }
            KetSegmentedControl { id: rule; width: parent.width; options: Rules.rules; expanding: true; enabled: !editor.teamStart; Accessible.name: "Repeat rule" }
            Flow {
                visible: rule.currentIndex === 3
                width: parent.width; spacing: editor.px(4)
                Repeater {
                    model: [1,2,3,4,5,6,0]
                    KetButton {
                        required property int modelData
                        text: Rules.days[modelData]
                        variant: editor.days.includes(modelData) ? KetTheme.ButtonVariant.Primary : KetTheme.ButtonVariant.Secondary
                        Accessible.name: text + (editor.days.includes(modelData) ? ", selected" : ", not selected")
                        onClicked: editor.days = editor.days.includes(modelData) ? editor.days.filter(d => d !== modelData) : editor.days.concat([modelData])
                    }
                }
            }
            KetFieldRow {
                width: parent.width; visible: rule.currentIndex === 0; label: "Date"
                KetTextField { id: dateField; width: parent.width; Accessible.name: "Run date (YYYY-MM-DD)" }
            }
            KetFieldRow {
                width: parent.width; visible: rule.currentIndex !== 4; label: "Time"
                KetTextField { id: timeField; width: parent.width; Accessible.name: "Run time (HH:mm)" }
            }
            KetFieldRow {
                width: parent.width; visible: rule.currentIndex === 4; label: "Every N hours"
                KetTextField { id: hoursField; width: parent.width; Accessible.name: "Hours between runs" }
            }
            KetFieldRow {
                width: parent.width; label: "Time zone"
                KetSelect { id: zonePicker; width: parent.width; model: Rules.zones; Accessible.name: "Time zone" }
            }
            ScheduleText { width: parent.width; text: Rules.liveSummary(editor.draft); color: KetTheme.t.accentText; font.pixelSize: editor.px(12) }
        }
        KetFieldRow {
            width: parent.width; label: "When busy"
            KetSelect { id: busyPicker; width: parent.width; model: ["Queue until finished", "Skip this run"]; Accessible.name: "When busy" }
        }
        KetFieldRow {
            width: parent.width; label: "When missed"
            KetSelect { id: missedPicker; width: parent.width; model: ["Catch up once when the computer wakes", "Skip"]; Accessible.name: "When missed" }
        }
        ScheduleText { width: parent.width; visible: editor.tried && text !== ""; text: Rules.error(editor.draft); color: KetTheme.t.danger; Accessible.role: Accessible.AlertMessage }
    }
    actions: [
        KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: editor.reject() },
        KetButton { text: "Save schedule"; variant: KetTheme.ButtonVariant.Primary; onClicked: editor.attemptSave() }
    ]
    ModelContextDialog {
        id: modelPicker
        window: editor.window
        provider: editor.provider; model: editor.model; effort: editor.effort
        onChosen: (provider, model, effort) => { editor.provider = provider; editor.model = model; editor.effort = effort }
    }
}
