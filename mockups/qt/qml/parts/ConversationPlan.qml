// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock
import KetPlusMock.Native

// Proposed plan-following harness, rendered with the native kit. All evidence is simulated.
Item {
    id: screen
    required property var window
    function px(n) { return KetTheme.px(n, KetTheme.zoom) }
    required property var run
    readonly property string stateLabel: ({ ready: "Ready", planning: "Planning", running: "Working", continuing: "Continuing",
        blocked: "Blocked", paused: "Stopped by you", limit: "Limit reached", complete: "Complete" })[run.status]
    component Label: Text {
        color: KetTheme.t.textMain
        font.family: KetTheme.fontFamily
        font.pixelSize: screen.px(13)
        wrapMode: Text.Wrap
    }
    component Surface: Rectangle {
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.panelBorder
        radius: screen.px(8)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: screen.px(16)
        spacing: screen.px(12)
        RowLayout {
            Layout.fillWidth: true
            Label { text: run.planCreated ? "Plan v1" : "Automatic planning"; font.weight: Font.DemiBold }
            KetBadge { visible: run.planCreated; text: run.completed + "/4 verified"; tone: run.completed === 4 ? KetTheme.BadgeTone.Success : KetTheme.BadgeTone.Neutral }
            Label { visible: run.planCreated; text: "Follow-ups " + run.followUps + "/" + run.followUpLimit; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
            Item { Layout.fillWidth: true }
            KetButton { visible: run.status === "ready"; text: "Send sample prompt"; iconName: "arrow-right"; onClicked: { run.presented = true; run.submitPrompt() } }
            KetButton { visible: run.active; text: "Stop run"; iconName: "stop-circle"; variant: KetTheme.ButtonVariant.Ghost; onClicked: run.stop() }
            KetButton { visible: run.status === "paused"; text: "Resume plan"; onClicked: run.resume() }
            KetButton { visible: run.status === "blocked"; text: "Retry check"; onClicked: run.retryBlocker() }
            KetButton { visible: run.status === "limit"; text: "Allow +1 follow-up"; onClicked: run.extendLimit() }
        }
        KetBanner {
            Layout.fillWidth: true
            title: run.status === "continuing" ? "Turn ended early. Continuing task " + (run.completed + 1) + " in this conversation."
                : run.status === "blocked" ? "Tests blocked · repository owner must restore the fixture."
                : run.status === "limit" ? "Follow-up limit reached. Remaining tasks stay open."
                : run.status === "paused" ? "Stopped by you. No automatic follow-up will start."
                : run.status === "complete" ? "4/4 verified · plan complete. Deployment was not in scope."
                : run.status === "running" ? "Working on task " + (run.completed + 1) + ". Progress stays with this conversation."
                : run.status === "planning" ? (run.contextRead ? "Repo context read. Creating a plan for this request." : "Multi-step work detected. Reading repo memory and relevant open issues.")
                : "Send a request. The agent creates a plan when the work needs one."
            tone: ["blocked", "limit"].includes(run.status) ? KetTheme.BannerTone.Warning : KetTheme.BannerTone.Info
        }
        Basic.ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            Column {
                width: scroll.availableWidth
                spacing: screen.px(16)
                Column {
                    width: parent.width
                    KetActivityRow {
                        id: contextToggle
                        width: parent.width
                        text: run.contextRead ? "Repo context · AGENTS.md · docs/release.md · GitHub #241" : "Repo context · memory and open issues · " + (run.status === "ready" ? "not read yet" : "reading")
                        iconName: "chevron-right"
                        expandable: true
                    }
                    Repeater {
                        model: contextToggle.checked ? run.sources : []
                        Label {
                            required property var modelData
                            x: screen.px(24)
                            width: parent.width - x
                            text: modelData.title + " — " + (run.contextRead ? modelData.detail : "Pending read")
                            font.pixelSize: screen.px(12)
                            color: KetTheme.t.textSecondary
                            bottomPadding: screen.px(6)
                        }
                    }
                }
                Surface {
                    visible: run.planCreated
                    width: parent.width
                    height: tasksColumn.implicitHeight + screen.px(16)
                    Column {
                        id: tasksColumn
                        x: screen.px(8)
                        y: screen.px(8)
                        width: parent.width - 2*x
                        spacing: 0
                        Repeater {
                            model: run.tasks
                            Column {
                                id: task
                                required property var modelData
                                required property int index
                                readonly property bool done: index < run.completed
                                readonly property string statusText: done ? "Verified" : index === run.completed && run.status === "running" ? "Working"
                                    : index === run.completed && run.status === "blocked" ? "Blocked" : "Open"
                                width: tasksColumn.width
                                RowLayout {
                                    width: parent.width
                                    spacing: screen.px(12)
                                    KetActivityRow {
                                        id: taskToggle
                                        checked: task.index === 0 && Mock.initialPanel === "plan-evidence"
                                        Layout.fillWidth: true
                                        text: (task.index + 1) + ". " + task.modelData.title
                                        iconName: task.done ? "success" : "circle"
                                        expandable: true
                                    }
                                    Label {
                                        Layout.preferredWidth: screen.px(100)
                                        text: task.modelData.owner
                                        color: KetTheme.t.textMuted
                                        font.pixelSize: screen.px(12)
                                    }
                                    Label {
                                        Layout.preferredWidth: screen.px(64)
                                        text: task.statusText
                                        color: task.done ? KetTheme.t.success : KetTheme.t.textSecondary
                                        font.pixelSize: screen.px(12)
                                    }
                                }
                                Column {
                                    visible: taskToggle.checked
                                    width: parent.width - screen.px(48)
                                    x: screen.px(24)
                                    topPadding: screen.px(4)
                                    bottomPadding: screen.px(12)
                                    spacing: screen.px(4)
                                    Label { width: parent.width; text: "Accept: " + task.modelData.criterion; color: KetTheme.t.textSecondary }
                                    Label { width: parent.width; text: "Source: " + task.modelData.source; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                                    Label { width: parent.width; text: task.done ? "Evidence: " + task.modelData.evidence : "Evidence pending"; color: task.done ? KetTheme.t.success : KetTheme.t.textMuted }
                                }
                                Rectangle { visible: task.index < 3; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                            }
                        }
                    }
                }
                Column {
                    width: parent.width
                    spacing: screen.px(4)
                    Label { visible: run.events.length > 0; text: "Run activity"; font.weight: Font.DemiBold }
                    Repeater {
                        model: run.events
                        Column {
                            required property var modelData
                            width: parent.width
                            KetActivityRow {
                                id: eventToggle
                                width: parent.width
                                text: modelData.actor + " · " + modelData.title
                                iconName: modelData.actor === "Daemon" ? "refresh" : "chevron-right"
                                expandable: true
                            }
                            Label {
                                visible: eventToggle.checked
                                x: screen.px(24)
                                width: parent.width - x
                                topPadding: screen.px(4)
                                bottomPadding: screen.px(8)
                                text: modelData.detail
                                color: KetTheme.t.textSecondary
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Preview"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
            KetSelect {
                Layout.preferredWidth: screen.px(145)
                model: ["Before prompt", "Planning", "Working", "Early finish", "Blocked", "Stopped", "Limit reached", "Complete"]
                currentIndex: ["ready", "planning", "running", "continuing", "blocked", "paused", "limit", "complete"].indexOf(run.status)
                // A preview state replays the run in the transcript as well.
                onActivated: index => {
                    run.load(["ready", "planning", "running", "continuing", "blocked", "paused", "limit", "complete"][index])
                    run.presented = true
                }
            }
            KetButton {
                visible: (run.status === "continuing" || run.status === "planning") && !run.autoplay
                text: "Play"; variant: KetTheme.ButtonVariant.Ghost
                onClicked: { run.presented = true; run.autoplay = true }
            }
            Item { Layout.fillWidth: true }
            Label { text: "Concept · sample data"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
        }
    }
}
