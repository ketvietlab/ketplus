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

    readonly property real toastBottomAvoidance: composerBar.visible
        ? composerBar.parent.height - composerBar.y : 0
    required property var window
    property bool detailsOpen: false
    property var contentSession: null
    readonly property string detailsTab: contentSession && contentSession.current ? contentSession.current.kind : ""
    ContentTabsStore { id: contentStore }
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
    readonly property var providerSetup: Fixtures.providerSetup[window.providerIssues[providerId] || ""] || null
    property bool showTasks: running

    property var planRun: null
    readonly property string planState: planRun ? planRun.status : ""
    ConversationPlanStore { id: planStore }
    function syncPlan() {
        contentSession = contentStore.get(conversation.number)
        planRun = conversation && conversation.hasPlan ? planStore.get(conversation.number) : null
    }
    // A plan-* preview state replays the plan run in the transcript; opening the Plan tab
    // from the conversation keeps the conversation's own transcript.
    function openPlan(state) {
        syncPlan()
        if (!planRun) return
        if (state) {
            planRun.load(state)
            planRun.presented = true
        }
        detailsPane.openContent("plan")
        detailsOpen = true
    }
    // Header actions toggle: the open document's own action closes the pane.
    function toggleContent(kind) {
        if (detailsOpen && detailsTab === kind)
            detailsOpen = false
        else
            openContent(kind)
    }
    function openContent(kind) {
        if (kind === "plan") { openPlan(""); return }
        detailsPane.openContent(kind)
        detailsOpen = true
    }
    function openContentDemo(kind) {
        contentSession.tabs = []; contentSession.currentId = ""; contentSession.terminalSerial = 0
        detailsPane.openFile("NavigationRail.qml")
        detailsPane.openFile("WorktreeItem.qml")
        const first = contentSession.newTerminal()
        contentSession.patch(first, { draft: "cmake --build build" })
        const second = contentSession.newTerminal()
        contentSession.patch(second, { draft: "ctest --test-dir build" })
        openPlan("continuing")
        if (kind === "terminal") contentSession.select(second)
        if (kind === "file") detailsPane.openFile("NavigationRail.qml")
    }
    function planStatus(number, fallback) {
        const model = planStore.plans[number]
        // A plan that was never created leaves the conversation's own status.
        if (!model || model.status === "ready") return fallback
        if (model.active) return KetTheme.ConversationStatus.Working
        if (model.status === "complete") return KetTheme.ConversationStatus.Completed
        if (model.status === "paused") return KetTheme.ConversationStatus.Stopped
        return KetTheme.ConversationStatus.NeedsInput
    }
    onConversationChanged: { detailsOpen = false; syncPlan() }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function openComposerPanel(name) {
        if (name === "content-tabs") detailsPane.openTabMenu()
        // The header's Plan ring, and the Atlas action pressed twice (it closes the pane again).
        else if (name === "plan") toggleContent("plan")
        else if (name === "atlas-toggle") { toggleContent("atlas"); toggleContent("atlas") }
        else if (name === "reviewer")
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
                planRun: screen.planRun
                status: screen.planStatus(screen.conversation.number, screen.conversation.status)
                detailsOpen: screen.detailsOpen
                onToggleDetails: tab => screen.toggleContent(tab)
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

                Column {
                    width: scroll.availableWidth
                    Loader {
                        active: !!screen.planRun && screen.planRun.presented
                        visible: active
                        width: parent.width
                        sourceComponent: ConversationPlanTranscript {
                            window: screen.window
                            run: screen.planRun
                        }
                    }
                    Transcript {
                        visible: !screen.planRun || !screen.planRun.presented
                        width: parent.width
                        window: screen.window
                        kind: screen.kind
                        conversation: screen.conversation
                        onOpenDetails: tab => screen.openContent(tab)
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
                    active: screen.setupId !== "" && !screen.window.hiddenSetups[screen.providerId]
                    visible: active
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - screen.px(48), screen.px(760))
                    sourceComponent: ProviderSetupCard {
                        setupId: screen.setupId
                        window: screen.window
                        canHide: true
                    }
                }

                ComposerBar {
                    id: composerBar
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - screen.px(48), screen.px(760))
                    window: screen.window
                    busy: screen.running || (!!screen.planRun && screen.planRun.presented && screen.planRun.active)
                    onStopRequested: { if (screen.planRun && screen.planRun.presented) screen.planRun.stop() }
                    // The conversation's provider and model, when its fixture names them.
                    provider: screen.providerId
                    blocked: screen.setupId !== ""
                    setup: screen.providerSetup
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
            id: detailsPane
            visible: screen.detailsOpen
            Layout.fillHeight: true
            // Half of the workspace beside the rule: the pane matches the conversation's width.
            Layout.preferredWidth: Math.floor((screen.width - 1) / 2)
            window: screen.window
            planRun: screen.planRun
            session: screen.contentSession
            onPlanRequested: screen.openPlan("")
            onCloseRequested: screen.detailsOpen = false
        }
    }
}
