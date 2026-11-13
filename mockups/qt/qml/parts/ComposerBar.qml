// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import "../schedules/ScheduleRules.js" as Rules

// The composer as KetPlus desktop lays it out: the card holds only the message and its
// send/stop circle; the row below holds attachments and the mode on the left, then the
// model with its effort and the Context & limits ring on the right. The model button opens
// Model & context; the ring opens its panel in place.
Item {
    id: bar

    required property var window
    property var scheduleDrafts: ({})
    readonly property string conversationNumber: window.conversation.number
    readonly property var existingSchedule: window.scheduleStore.clockFor(conversationNumber) || null
    readonly property bool newConversation: window.conversation.transcript === "welcome" && !existingSchedule
    readonly property var scheduleDraft: scheduleDrafts[conversationNumber] || null
    readonly property bool scheduleSet: newConversation && !!scheduleDraft
    function setScheduleDraft(plan) {
        const next = Object.assign({}, scheduleDrafts)
        if (plan) next[conversationNumber] = Object.assign({}, plan)
        else delete next[conversationNumber]
        scheduleDrafts = next
    }
    function submitSchedule(text) {
        if (!scheduleSet || !text.trim()) return false
        const id = window.scheduleStore.save(Object.assign({}, scheduleDraft, {
            name: text.split("\n")[0].slice(0, 64), prompt: text, target: scheduleDraft.newConversation ? 1 : 0,
            conversation: conversationNumber, sourceConversation: conversationNumber,
            provider: provider, model: model, effort: effort, repository: window.repository,
            worktree: !!scheduleDraft.newConversation, creator: "You"
        }))
        if (!id) return false
        setScheduleDraft(null); composer.text = ""; window.openSchedules(id); return true
    }
    function openSchedule() {
        if (newConversation) { firstSchedule.prepare(scheduleDraft); firstSchedule.openAbove(scheduleButton) }
        else if (existingSchedule) existingMenu.popup(scheduleButton, scheduleButton.width - existingMenu.width, -existingMenu.height - px(6))
    }
    property bool busy: false
    signal stopRequested()
    // The provider cannot start a turn yet: Send keeps the draft and says why.
    property bool blocked: false
    // The provider's setup state (Fixtures.providerSetup), shown on the model button.
    property var setup: null
    property bool compact: false // narrow columns keep only the mode's icon
    property string provider: "claude"
    property string model: "Opus 5.5"
    property string placeholder: "Message " + providerLabel + "…"
    property string effort: "Medium"
    property string permission: "Workspace"
    property real contextUsed: 0.16
    readonly property var mode: {
        for (const entry of Fixtures.permissionModes)
            if (entry.label === bar.permission)
                return entry
        return Fixtures.permissionModes[1]
    }
    readonly property string providerLabel: {
        for (const entry of Fixtures.providers)
            if (entry.id === bar.provider)
                return entry.label
        return ""
    }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    // Review evidence: --open=model|mode|limits; limits-cost opens Cost details and
    // limits-api-key shows a conversation on an API key, which has no such row.
    function openPanel(name) {
        if (["schedule-new", "schedule-ready", "schedule-more", "schedule-weekly", "schedule-at", "schedule-existing"].includes(name)) {
            if (name !== "schedule-existing") composer.text = "Write a new post for the website"
            openSchedule()
            if (newConversation && name !== "schedule-new") {
                firstSchedule.preview(name === "schedule-weekly" ? "weekly" : name === "schedule-at" ? "at" : name === "schedule-more" ? "more" : "repeat")
                if (name === "schedule-ready") firstSchedule.apply()
            }
        }
        else if (name === "model")
            modelDialog.open()
        else if (name === "mode")
            permissionButton.clicked()
        else if (["limits", "limits-cost", "limits-api-key"].includes(name)) {
            limits.apiKey = name === "limits-api-key"
            limits.toggleAbove(ring)
            limits.costExpanded = name === "limits-cost"
        }
        else if (name === "composer")
            composer.focusInput()
        // The suggestion popovers, for the side by side with the app's composer.
        else if (["mention", "agent-mention", "skill", "slash"].includes(name)) {
            composer.text = ({ "mention": "look at @mai", "agent-mention": "ask @agent/82", "skill": "use $rev", "slash": "/" })[name]
            suggestions.suggestions = ({
                "mention": [{ kind: "file", path: "src/app/main.cpp" }, { kind: "file", path: "src/app/MainWindow.cpp" }, { kind: "file", path: "src/main", directory: true }],
                "agent-mention": [{ kind: "conversation", mention: "@agent/8213", description: "Finish the repository picker", detail: "Claude · ketplus-desktop" }, { kind: "conversation", mention: "@agent/8215", description: "Follow up on the new UI", detail: "Codex · ketplus-desktop" }],
                "skill": [{ kind: "skill", mention: "$review", description: "Review the current diff for bugs", detail: "Project skill" }, { kind: "skill", mention: "$release-notes", description: "Draft release notes from merged PRs", detail: "User skill" }],
                "slash": [{ kind: "command", mention: "/compact", description: "Summarize the conversation to free context" }, { kind: "command", mention: "/clear", description: "Start a new conversation" }, { kind: "command", mention: "/model", description: "Choose the model and effort" }]
            })[name]
            composer.focusInput()
        }
    }
    function choosePermission(index) {
        bar.permission = Fixtures.permissionModes[index].label
        permissionMenu.close()
    }

    implicitHeight: composer.implicitHeight + footer.anchors.topMargin + footer.height

    KetComposer {
        id: composer
        width: parent.width - (bar.scheduleSet ? scheduledSend.width + bar.px(6) : 0)
        busy: bar.busy
        placeholderText: bar.placeholder
        actionButton.visible: !bar.scheduleSet
        onSubmitted: text => {
            if (bar.scheduleSet) { if (!bar.submitSchedule(text)) composer.text = text; return }
            if (bar.blocked) {
                // KetComposer clears its input before it emits submitted; put the draft back.
                composer.text = text
                bar.window.showSetup(bar.provider)
                bar.window.notify("Set up " + bar.providerLabel + " first. Your message is kept.")
                return
            }
            bar.window.notify("Sent to " + bar.providerLabel)
            composer.text = ""
        }
        onStopRequested: { bar.stopRequested(); bar.window.notify("Stopped the turn") }
    }

    // The configured first message has a labelled primary action instead of Send.
    KetButton {
        id: scheduledSend
        visible: bar.scheduleSet
        anchors.right: parent.right
        anchors.verticalCenter: composer.verticalCenter
        text: "Schedule"; variant: KetTheme.ButtonVariant.Primary
        enabled: composer.text.trim() !== ""
        onClicked: bar.submitSchedule(composer.text)
    }

    ComposerSuggestions {
        id: suggestions
        anchors.left: composer.left
        anchors.right: composer.right
        anchors.bottom: composer.top
        anchors.bottomMargin: bar.px(6)
        z: 20
    }

    RowLayout {
        id: footer
        anchors.top: composer.bottom
        anchors.topMargin: bar.px(4)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: bar.px(2)
        anchors.rightMargin: bar.px(2)
        height: bar.px(28)
        spacing: bar.px(2)

        KetIconButton {
            id: attach
            controlSize: KetTheme.ControlSize.Small
            iconWeight: KetTheme.IconWeight.Regular
            glyphSize: composer.recipe.attachIconSize
            iconName: "plus"
            text: "Add images or text files"
            onClicked: attachMenu.popup(attach, 0, -attachMenu.height - bar.px(4))
        }

        ComposerFooterButton {
            id: permissionButton
            iconName: bar.mode.icon
            text: bar.mode.label
            warning: bar.mode.elevated
            showLabel: !bar.compact
            open: permissionMenu.visible
            Accessible.description: "Permissions for the next message. Restricted actions are blocked without approval prompts."
            onClicked: permissionMenu.popup(permissionButton, 0, -permissionMenu.height - bar.px(4))
        }

        Item { Layout.fillWidth: true }

        ComposerFooterButton {
            id: modelButton
            Layout.maximumWidth: Math.max(bar.px(40), bar.width - bar.px(bar.compact ? 150 : 270))
            text: bar.providerLabel + " · " + bar.model + "   " + bar.effort
            iconName: bar.setup ? "warning" : ""
            warning: !!bar.setup
            open: modelDialog.visible
            Accessible.name: "Choose model and reasoning effort"
            Accessible.description: bar.providerLabel + " · " + bar.model + " · " + bar.effort + " effort"
                                    + (bar.setup ? ". " + bar.setup.title : "")
            onClicked: modelDialog.open()
        }

        KetUsageRing {
            id: ring
            objectName: "composerContextRing"
            Layout.alignment: Qt.AlignVCenter
            value: bar.contextUsed
            onClicked: limits.toggleAbove(ring)
        }
        KetIconButton {
            id: scheduleButton
            objectName: "composerScheduleClock"
            Layout.alignment: Qt.AlignVCenter
            controlSize: KetTheme.ControlSize.Small
            // The clock symbol has optical margins; match its visible circle to the ring.
            glyphSize: ring.recipe.size * 1.24
            visible: bar.newConversation || !!bar.existingSchedule
            iconName: "clock"; checkable: true
            checked: bar.scheduleSet || (!!bar.existingSchedule && bar.existingSchedule.enabled)
            text: bar.newConversation ? (bar.scheduleSet ? "Scheduled · " + Rules.nextLabel(bar.scheduleDraft) : "Schedule first message")
                  : bar.existingSchedule ? (bar.existingSchedule.enabled ? "Next run: " + Rules.nextLabel(bar.existingSchedule) : "Schedule paused") : "Schedule"
            onClicked: bar.openSchedule()
        }
    }

    ConversationSchedulePopover { id: firstSchedule; onConfigured: plan => bar.setScheduleDraft(plan) }
    KetMenu {
        id: existingMenu
        KetMenuSection { text: bar.existingSchedule ? bar.existingSchedule.name : "Schedule" }
        KetMenuItem { text: "Edit"; iconName: "edit"; onTriggered: bar.window.editExistingSchedule(bar.existingSchedule) }
        KetMenuItem { text: bar.existingSchedule && bar.existingSchedule.enabled ? "Pause" : "Resume"; onTriggered: if (bar.existingSchedule) bar.window.scheduleStore.patch(bar.existingSchedule.id, { enabled: !bar.existingSchedule.enabled }) }
        KetMenuItem { text: "Open in Schedules"; onTriggered: if (bar.existingSchedule) bar.window.openSchedules(bar.existingSchedule.id) }
    }
    function scheduleEvidenceChecks() {
        const geometry = scheduleButton.visible && scheduleButton.x >= ring.x + ring.width
            && scheduleButton.x + scheduleButton.width <= footer.width + 1
            && Math.abs(KetTheme.iconDrawSize(scheduleButton.glyphSize) * 0.817 - ring.width) < 1
            && Math.abs(scheduleButton.y + scheduleButton.height / 2 - ring.y - ring.height / 2) <= 0.5
        if (newConversation) {
            const valid = geometry && (firstSchedule.geometryVisible() || (scheduleSet && scheduledSend.visible))
            if (!valid) console.warn("QML schedule geometry: ring=" + ring.x + "/" + ring.width + " center=" + (ring.y + ring.height / 2) + ", clock=" + scheduleButton.x + "/" + footer.width + " glyph=" + scheduleButton.glyphSize + " center=" + (scheduleButton.y + scheduleButton.height / 2) + ", popup=" + firstSchedule.x + "," + firstSchedule.y + " " + firstSchedule.width + "x" + firstSchedule.height + ", overlay=" + (firstSchedule.parent ? firstSchedule.parent.width + "x" + firstSchedule.parent.height : "none"))
            return valid
        }
        return geometry && !!existingSchedule && existingMenu.visible
    }
    function scheduleChecks() {
        if (!newConversation) return false
        firstSchedule.prepare(null); firstSchedule.preview("repeat")
        firstSchedule.apply()
        const scheduled = scheduleSet && scheduledSend.visible
        const saved = submitSchedule("A scheduled first message")
        return scheduled && saved && !!existingSchedule && !newConversation
    }
    KetMenu {
        id: attachMenu
        KetMenuItem { text: "Attach files…"; iconName: "attachment"; onTriggered: bar.window.notify("Attached design-system.md") }
        KetMenuItem { text: "Add a screenshot"; iconName: "image"; onTriggered: bar.window.notify("Added a screenshot") }
        KetMenuItem { text: "Mention a conversation"; iconName: "chat"; shortcut: "@"; onTriggered: composer.text += "@agent/" }
    }

    KetMenu {
        id: permissionMenu
        width: Math.min(bar.px(340), bar.window.width - bar.px(24))
        KetMenuSection { text: "Mode" }
        Repeater {
            model: Fixtures.permissionModes
            KetMenuItem {
                required property var modelData
                required property int index
                text: modelData.label
                description: modelData.description
                iconName: modelData.icon
                warning: modelData.elevated
                shortcut: String(index + 1)
                checkable: true
                checked: bar.permission === modelData.label
                onTriggered: bar.choosePermission(index)
                // The highlighted item has focus: 1–3 choose from anywhere in the menu.
                Keys.onPressed: event => {
                    const choice = event.key - Qt.Key_1
                    if (choice >= 0 && choice < Fixtures.permissionModes.length) {
                        bar.choosePermission(choice)
                        event.accepted = true
                    }
                }
            }
        }
    }

    ModelContextDialog {
        id: modelDialog
        parent: Basic.Overlay.overlay
        provider: bar.provider
        model: bar.model
        window: bar.window
        effort: bar.effort
        // The choice changes the conversation, so its setup state and the composer follow it.
        onChosen: (provider, model, effort) => bar.window.chooseModel(provider, model, effort)
    }

    ContextLimitsPopover {
        id: limits
        provider: bar.provider
        contextUsed: bar.contextUsed
        // A used-up limit shows the same quota and reset as its setup guide.
        rows: bar.setup && bar.setup.limits ? bar.setup.limits : null
        onCompactRequested: bar.window.notify("Compacting the conversation")
    }
}
