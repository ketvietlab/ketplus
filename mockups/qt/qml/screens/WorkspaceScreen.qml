// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Agent Workspace: the conversation's 44 px header, its transcript, background tasks and the
// composer, with an optional details pane (changes and Atlases) on the right.
Item {
    id: screen

    required property var window
    property bool detailsOpen: false
    property string detailsTab: "changes"
    readonly property var conversation: window.conversation
    readonly property string kind: conversation.transcript
    readonly property bool running: kind === "running"
    property bool showTasks: running

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ConversationHeader {
                Layout.fillWidth: true
                window: screen.window
                conversation: screen.conversation
                detailsOpen: screen.detailsOpen
                onToggleDetails: tab => {
                    if (screen.detailsOpen && screen.detailsTab === tab)
                        screen.detailsOpen = false
                    else {
                        screen.detailsTab = tab
                        screen.detailsOpen = true
                    }
                }
            }

            Basic.ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                Transcript {
                    width: scroll.availableWidth
                    window: screen.window
                    kind: screen.kind
                    conversation: screen.conversation
                    onOpenDetails: tab => {
                        screen.detailsTab = tab
                        screen.detailsOpen = true
                    }
                }
            }

            // The composer column matches the transcript's reading width.
            Column {
                Layout.fillWidth: true
                Layout.bottomMargin: screen.px(12)
                spacing: screen.px(8)

                KetTaskBar {
                    id: tasks
                    visible: screen.showTasks && tasks.tasks.length > 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - screen.px(48), screen.px(760))
                    tasks: Fixtures.backgroundTasks
                    onStopRequested: index => {
                        const next = tasks.tasks.slice()
                        next[index] = Object.assign({}, next[index],
                                                    { status: KetTheme.TaskStatus.Stopped,
                                                      meta: "Stopped" })
                        tasks.tasks = next
                        screen.window.notify("Stopped " + next[index].title)
                    }
                    onDismissRequested: index => tasks.tasks = tasks.tasks.filter((t, i) => i !== index)
                }

                ComposerBar {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - screen.px(48), screen.px(760))
                    window: screen.window
                    busy: screen.running
                    placeholder: screen.kind === "welcome" ? "Describe what you want to build…"
                                                           : "Message Codex…"
                }
            }
        }

        Rectangle {
            visible: screen.detailsOpen
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: KetTheme.t.panelBorder
        }

        DetailsPane {
            visible: screen.detailsOpen
            Layout.fillHeight: true
            Layout.preferredWidth: screen.px(460)
            window: screen.window
            tab: screen.detailsTab
            onTabChosen: tab => screen.detailsTab = tab
            onCloseRequested: screen.detailsOpen = false
        }
    }
}
