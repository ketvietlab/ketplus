// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import "ScheduleRules.js" as Rules

// One reusable, non-modal window. List selection, edits and history share the same store.
Basic.ApplicationWindow {
    id: schedules
    objectName: "schedulesWindow"
    required property var window
    required property var store
    signal saved()
    signal editorClosed()
    property bool requestedVisible: false
    function present() { if (requestedVisible) { show(); raise(); requestActivate() } }
    property string selectedId: "web"
    property string teamFilter: ""
    property bool expandedHistory: false
    readonly property var selected: store.find(selectedId)
    readonly property var shown: store.entries.filter(e => (!teamFilter || e.teamId === teamFilter)
        && (filter.currentIndex === 0 || (filter.currentIndex === 1 ? e.enabled : filter.currentIndex === 2 ? !e.enabled : e.creator.startsWith("Agent")))
        && (search.text.trim() === "" || (e.name + " " + store.target(e)).toLowerCase().includes(search.text.trim().toLowerCase())))
    readonly property var history: selected ? (selected.history || (selected.id === "ci" ? [
        { status: "✗ Failed: provider is not signed in", at: "13/11 08:30", conversation: "", detail: "Codex is not signed in. Sign in, then choose Run now to retry." },
        { status: "✓ Succeeded", at: "12/11 08:30", conversation: "9410", detail: "Summarized 3 workflows that need attention." }
    ] : selected.kind === "team-start" ? [] : [
        { status: "✓ Succeeded", at: "13/11 08:00", conversation: "4821", detail: "Draft sent to the leader for approval." },
        { status: "↷ Skipped: team paused", at: "12/11 08:00", conversation: "", detail: "Release 1.4 is paused." },
        { status: "⚠ Missed (computer asleep), caught up at 08:47", at: "11/11 08:00", conversation: "4821", detail: "One catch-up run; earlier missed runs are not queued." },
        { status: "✗ Failed: provider is not signed in", at: "10/11 08:00", conversation: "", detail: "Codex is not signed in." },
        { status: "✓ Succeeded", at: "09/11 08:00", conversation: "4821", detail: "Draft sent." }
    ])) : []
    onShownChanged: {
        if (!shown.some(e => e.id === selectedId)) selectedId = shown.length ? shown[0].id : ""
    }
    function px(v) { return KetTheme.px(v, KetTheme.zoom) }
    function showSchedule(id, team) {
        teamFilter = team || ""
        filter.currentIndex = 0; search.text = ""
        selectedId = id || (shown.length ? shown[0].id : "")
        expandedHistory = false
        if (!visible) { x = window.x + px(60); y = window.y + px(24) }
        requestedVisible = true
        if (window.presentationReady) present()
    }
    function edit(entry, target, number, once) { showSchedule(entry ? entry.id : "", ""); editor.load(entry, target, number, once) }
    function newTeamSchedule(id) {
        showSchedule("", id)
        editor.load(null, 2, "4821", false)
        editor.selectTeam(id)
    }
    function setPrompt(text) { editor.setPrompt(text) }
    function checks() {
        store.reset()
        showSchedule("web", "")
        const same = schedules
        showSchedule("ci", "")
        let errors = 0
        function check(ok, text) { if (!ok) { console.warn("QML schedule check: " + text); errors++ } }
        check(schedules === same && selectedId === "ci" && modality === Qt.NonModal, "one non-modal window is reused")
        editor.load(store.find("web"), 2, "4821", false)
        check(editor.draft.target === 2 && editor.draft.rule === 1, "team editor uses current target and rule")
        editor.reject()
        editor.load(null, 0, "8116", false)
        check(editor.draft.conversation === "8116" && editor.draft.rule === 1, "repeat menu prefills this conversation and daily")
        check(!editor.attemptSave() && editor.visible, "invalid empty form stays open")
        editor.reject()
        for (const state of ["new", "weekly", "new-conversation", "validation", "delete"]) {
            showState(state); editor.close(); deleteDialog.close()
        }
        store.reset()
        showSchedule("web", "release")
        check(shown.length === 3 && shown.every(e => e.teamId === "release"), "team menu filters the window")
        search.text = "no matching schedules"
        check(shown.length === 0 && selected === null, "search empty state clears the stale detail")
        showSchedule("weekly", "")
        const before = store.entries.length
        deleteDialog.open(); deleteDialog.reject()
        check(store.entries.length === before, "cancel deletion preserves the schedule")
        deleteDialog.open(); confirmDelete.clicked()
        check(store.entries.length === before - 1 && !store.find("weekly"), "confirm deletion removes only the selected schedule")
        editor.load(Object.assign({}, store.find("web"), { id: "", name: "A new daily draft" }), 2, "4821", false)
        check(editor.attemptSave() && store.entries.some(e => e.name === "A new daily draft"), "valid editor creates a schedule")
        store.reset(); hide()
        return errors
    }
    function showState(state) {
        store.reset(); showSchedule(state === "failure" ? "ci" : "web", state === "team" ? "release" : "")
        if (state === "empty") { store.entries = []; selectedId = "" }
        if (["new", "edit", "weekly", "new-conversation", "validation", "policies"].includes(state)) {
            editor.load(state === "edit" || state === "weekly" || state === "policies" ? store.find(state === "weekly" ? "weekly" : "web") : null,
                        state === "new-conversation" ? 1 : 2, "8116", false)
            if (state === "validation") { editor.tried = true }
            if (state === "policies") Qt.callLater(() => editor.scrollToEnd())
        }
        if (state === "delete") deleteDialog.open()
    }
    function showFailure(id, message) { failureToast.scheduleId = id; failureToast.show(message) }
    ScheduleFailureToast { id: failureToast; onOpenRequested: id => schedules.showSchedule(id, "") }
    function runNow() {
        if (!selected) return
        if (selected.id === "ci") { window.scheduleFailure(); return }
        if (selected.id === "retry") { window.scheduleSkipped(); return }
        const entry = selected
        store.patch(entry.id, { last: "✓ Just now", history: [{ status: "✓ Succeeded", at: "13/11 15:00", conversation: "4821", detail: "Started with Run now." }].concat(history) })
    }
    title: "Schedules"
    transientParent: window
    modality: Qt.NonModal
    width: px(1160); height: px(800)
    minimumWidth: px(900); minimumHeight: px(600)
    visible: false
    color: KetTheme.t.pageBg
    font.family: KetTheme.fontFamily
    onClosing: { editor.close(); deleteDialog.close() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: schedules.px(16)
            spacing: schedules.px(10)
            ScheduleText { text: "Schedules"; font.pixelSize: schedules.px(20); font.weight: Font.DemiBold }
            KetBadge { text: schedules.teamFilter ? "Team Release 1.4" : store.entries.length + " schedules" }
            Item { Layout.fillWidth: true }
            KetSelect { id: filter; Layout.preferredWidth: schedules.px(150); model: ["All schedules", "Enabled", "Paused", "Agent-created"]; Accessible.name: "Filter schedules" }
            KetTextField { id: search; Layout.preferredWidth: schedules.px(220); placeholderText: "Search schedules…"; Accessible.name: "Search schedules" }
            KetButton { text: "New schedule"; iconName: "plus"; variant: KetTheme.ButtonVariant.Primary; onClicked: editor.load(null, schedules.teamFilter ? 2 : 0, schedules.window.conversation.number, false) }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: KetTheme.t.panelBorder }
        KetEmptyState {
            visible: schedules.shown.length === 0
            Layout.fillWidth: true; Layout.fillHeight: true
            iconName: "clock"
            title: store.entries.length === 0 ? "No schedules yet" : "No schedules found"
            message: store.entries.length === 0 ? "Put agents on a schedule: every morning, on weekdays or every week…" : "Try another name or change your filters."
            actionText: store.entries.length === 0 ? "Create your first schedule" : "Clear filters"
            actionIcon: store.entries.length === 0 ? "plus" : "search"
            onActionTriggered: {
                if (store.entries.length === 0) editor.load(null, 0, schedules.window.conversation.number, false)
                else { search.text = ""; filter.currentIndex = 0; schedules.teamFilter = "" }
            }
        }
        RowLayout {
            visible: schedules.shown.length > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0
            Basic.ScrollView {
                Layout.preferredWidth: schedules.px(350); Layout.fillHeight: true
                contentWidth: availableWidth; clip: true
                Column {
                    width: parent.width
                    Repeater {
                        model: schedules.shown
                        Rectangle {
                            id: row
                            required property var modelData
                            width: parent.width
                            height: content.implicitHeight + schedules.px(24)
                            color: schedules.selectedId === modelData.id ? KetTheme.t.navSelected : KetTheme.t.panelBg
                            RowLayout {
                                id: content
                                anchors.fill: parent; anchors.margins: schedules.px(12)
                                spacing: schedules.px(8)
                                KetButton {
                                    Layout.fillWidth: true; Layout.preferredHeight: contentItem.implicitHeight
                                    variant: KetTheme.ButtonVariant.Ghost
                                    Accessible.name: row.modelData.name + ", " + store.target(row.modelData)
                                    onClicked: { schedules.selectedId = row.modelData.id; schedules.expandedHistory = false }
                                    contentItem: Column {
                                        spacing: schedules.px(5)
                                        ScheduleText { width: parent.width; text: row.modelData.name; font.weight: Font.DemiBold }
                                        ScheduleText { width: parent.width; text: (row.modelData.target === 2 ? "👥 " : "💬 ") + store.target(row.modelData); color: KetTheme.t.textSecondary; font.pixelSize: schedules.px(12) }
                                        ScheduleText { width: parent.width; text: Rules.summary(row.modelData) + " · " + row.modelData.zone; color: KetTheme.t.textSecondary; font.pixelSize: schedules.px(12) }
                                        ScheduleText { width: parent.width; text: (row.modelData.enabled ? "Next: " + Rules.nextLabel(row.modelData) : "Paused") + " · Last: " + row.modelData.last; font.pixelSize: schedules.px(12); color: KetTheme.t.textMuted }
                                    }
                                }
                                KetCheckbox {
                                    Layout.alignment: Qt.AlignTop
                                    checked: row.modelData.enabled
                                    Accessible.name: (checked ? "Pause " : "Enable ") + row.modelData.name
                                    onToggled: store.patch(row.modelData.id, { enabled: checked })
                                }
                            }
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                        }
                    }
                }
            }
            Rectangle { Layout.fillHeight: true; width: 1; color: KetTheme.t.panelBorder }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: 0
                Basic.ScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    contentWidth: availableWidth; clip: true
                    Column {
                        x: schedules.px(24); y: schedules.px(20)
                        width: parent.width - schedules.px(48)
                        spacing: schedules.px(16)
                        RowLayout {
                            width: parent.width
                            ScheduleText { Layout.fillWidth: true; text: schedules.selected ? schedules.selected.name : "Select a schedule"; font.pixelSize: schedules.px(20); font.weight: Font.DemiBold }
                            KetCheckbox { visible: !!schedules.selected; text: checked ? "Enabled" : "Paused"; checked: schedules.selected ? schedules.selected.enabled : false; onToggled: if (schedules.selected) store.patch(schedules.selectedId, { enabled: checked }) }
                        }
                        Column {
                            width: parent.width; spacing: schedules.px(10)
                            ScheduleText { width: parent.width; text: "Target: " + (schedules.selected ? store.target(schedules.selected) : ""); font.weight: Font.Medium }
                            ScheduleText { width: parent.width; text: "Repeat: " + (schedules.selected ? Rules.summary(schedules.selected) + " · " + schedules.selected.zone : "") }
                            ScheduleText { width: parent.width; text: schedules.selected ? "When busy: " + (schedules.selected.busy === 0 ? "Queue" : "Skip") + " · When missed: " + (schedules.selected.missed === 0 ? "Catch up once" : "Skip") : ""; color: KetTheme.t.textSecondary }
                            ScheduleText { width: parent.width; text: "Created by: " + (schedules.selected ? schedules.selected.creator : ""); color: KetTheme.t.textSecondary }
                            ScheduleText { width: parent.width; text: "Next run: " + (schedules.selected ? (schedules.selected.enabled ? Rules.nextLabel(schedules.selected) : "Paused — enable to resume") : ""); color: KetTheme.t.accentText }
                        }
                        KetFieldRow {
                            width: parent.width; label: "Prompt"; stacked: true
                            // The kit's field supports selection/copy; changes are committed only in Edit.
                            KetTextArea { width: parent.width; text: schedules.selected ? schedules.selected.prompt : ""; minimumLines: 3; maximumLines: 4; input.readOnly: true; Accessible.name: "Schedule prompt" }
                        }
                        ScheduleText { text: "Run history"; font.pixelSize: schedules.px(14); font.weight: Font.DemiBold }
                        ScheduleText { visible: schedules.history.length === 0; width: parent.width; text: "No runs yet. The leader will start planning at the scheduled time."; color: KetTheme.t.textSecondary }
                        Repeater {
                            model: schedules.expandedHistory ? schedules.history : schedules.history.slice(0, 4)
                            Column {
                                required property var modelData
                                width: parent.width; spacing: schedules.px(5)
                                ScheduleText { width: parent.width; text: modelData.at + "  ·  " + modelData.status; font.weight: Font.Medium }
                                ScheduleText { width: parent.width; text: modelData.detail; color: KetTheme.t.textSecondary; font.pixelSize: schedules.px(12) }
                                KetButton { visible: modelData.conversation !== ""; text: "Conversation #" + modelData.conversation + " →"; variant: KetTheme.ButtonVariant.Ghost; onClicked: schedules.window.openScheduledConversation(modelData.conversation, schedules.selectedId) }
                                Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                            }
                        }
                        KetButton { visible: schedules.history.length > 4; text: schedules.expandedHistory ? "Show less" : "Show more"; variant: KetTheme.ButtonVariant.Ghost; onClicked: schedules.expandedHistory = !schedules.expandedHistory }
                        Item { width: 1; height: schedules.px(16) }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: KetTheme.t.panelBorder }
                Flow {
                    Layout.fillWidth: true; Layout.margins: schedules.px(12)
                    spacing: schedules.px(8)
                    KetButton { text: "Run now"; iconName: "resume"; enabled: !!schedules.selected; onClicked: schedules.runNow() }
                    KetButton { text: schedules.selected && schedules.selected.enabled ? "Pause" : "Resume"; iconName: "pause-circle"; enabled: !!schedules.selected; onClicked: store.patch(schedules.selectedId, { enabled: !schedules.selected.enabled }) }
                    KetButton { text: "Edit"; iconName: "edit"; enabled: !!schedules.selected; onClicked: editor.load(schedules.selected, 0, "", false) }
                    KetButton { text: "Delete"; iconName: "trash"; enabled: !!schedules.selected; onClicked: deleteDialog.open() }
                }
            }
        }
    }
    ScheduleEditor {
        id: editor; window: schedules.window; store: schedules.store
        onSaved: id => { schedules.selectedId = id; schedules.saved() }
        onRejected: schedules.editorClosed()
    }
    KetDialog {
        id: deleteDialog
        title: "Delete schedule?"
        ScheduleText { Layout.fillWidth: true; text: "Delete “" + (schedules.selected ? schedules.selected.name : "") + "”? Existing conversations will be kept. This schedule will not run again." }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: deleteDialog.reject() },
            KetButton { id: confirmDelete; text: "Delete schedule"; variant: KetTheme.ButtonVariant.Danger; onClicked: { store.remove(schedules.selectedId); schedules.selectedId = schedules.shown.length ? schedules.shown[0].id : ""; deleteDialog.accept() } }
        ]
    }
}
