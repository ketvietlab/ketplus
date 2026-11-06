// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

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
    readonly property string providerId: (conversation && conversation.providerId) || "claude"
    // The provider cannot start a turn (Fixtures.providerSetup, blocking): its guide sits above
    // the composer, which keeps the draft instead of sending it.
    readonly property string setupId: {
        const id = window.providerIssues[providerId] || ""
        const setup = Fixtures.providerSetup[id]
        return setup && setup.blocking ? id : ""
    }
    property bool showTasks: running

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function openComposerPanel(name) {
        if (name === "reviewer")
            conversationHeader.openReviewerMenu()
        else
            composerBar.openPanel(name)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ConversationHeader {
                id: conversationHeader
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
                // --scroll: start the transcript part of the way down, for review captures.
                Timer {
                    running: Mock.initialScroll >= 0
                    interval: 250
                    onTriggered: scroll.contentItem.contentY = Mock.initialScroll
                                 * Math.max(0, scroll.contentItem.contentHeight - scroll.height)
                }

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

                Loader {
                    active: screen.setupId !== ""
                    visible: active
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - screen.px(48), screen.px(760))
                    sourceComponent: ProviderSetupCard {
                        setupId: screen.setupId
                        window: screen.window
                    }
                }

                ComposerBar {
                    id: composerBar
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - screen.px(48), screen.px(760))
                    window: screen.window
                    busy: screen.running
                    // The conversation's provider and model, when its fixture names them.
                    provider: screen.providerId
                    blocked: screen.setupId !== ""
                    model: (screen.conversation && screen.conversation.model) || "Opus 5.5"
                    effort: (screen.conversation && screen.conversation.effort) || "Medium"
                    placeholder: blocked ? "Write your message now; it sends once " + providerLabel + " is set up"
                               : screen.kind === "welcome" ? "Describe what you want to build…"
                               : "Message " + providerLabel + "…"
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
            // Half of the workspace beside the rule: the pane matches the conversation's width.
            Layout.preferredWidth: Math.floor((screen.width - 1) / 2)
            window: screen.window
            tab: screen.detailsTab
            onTabChosen: tab => screen.detailsTab = tab
            onCloseRequested: screen.detailsOpen = false
        }
    }
}
