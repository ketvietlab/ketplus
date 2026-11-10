// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// One editor-style tab per open document/session, scoped to the conversation.
Rectangle {
    id: pane

    required property var window
    property var session: null
    property var planRun: null
    readonly property var document: session ? session.current : null
    readonly property string tab: document ? document.kind : ""
    signal closeRequested()
    signal planRequested()
    function px(n) { return KetTheme.px(n, KetTheme.zoom) }
    function icon(kind) {
        return ({ terminal: "terminal", plan: "check-all", report: "view-list", diff: "compare", changes: "compare", atlas: "layers", file: "file-code" })[kind] || "file-text"
    }
    function openFile(name) {
        const code = name === "NavigationRail.qml" ? Fixtures.navigationRail
            : "// WorktreeItem.qml · sample source\nimport QtQuick\n\nItem {\n    id: worktreeRow\n    property string branch: \"develop\"\n    property bool selected: false\n}\n"
        session.open("file", "modules/ai/src/agent/qml/" + name, name, code)
    }
    function openContent(kind) {
        if (!session) return
        if (kind === "terminal") session.newTerminal()
        else if (kind === "plan") session.open("plan", "conversation-plan", "Plan", "")
        else if (kind === "atlas") session.open("atlas", "onboarding.ketatlas", "onboarding.ketatlas", "")
        else if (kind === "report") session.open("report", "conversation-report", "Report", "")
        else session.open("changes", "conversation-changes", "Changes", "")
    }
    function openTabMenu() { tabMenu.popup(allTabsButton, 0, allTabsButton.height) }

    color: KetTheme.t.panelBg

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            KetTabBar {
                id: documentTabs
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                currentIndex: pane.session ? pane.session.tabs.findIndex(t => t.id === pane.session.currentId) : -1
                // Repeater insertion finishes after the initial currentIndex binding.
                // Reveal the selected document after the strip has laid out its tabs.
                function revealCurrent() {
                    if (currentIndex < 0) return
                    contentItem.forceLayout()
                    contentItem.positionViewAtIndex(currentIndex, ListView.Contain)
                }
                function selectRelative(delta) {
                    if (!pane.session || !pane.session.tabs.length) return
                    const next = Math.max(0, Math.min(pane.session.tabs.length - 1, currentIndex + delta))
                    pane.session.select(pane.session.tabs[next].id)
                }
                onCurrentIndexChanged: Qt.callLater(revealCurrent)
                onCountChanged: Qt.callLater(revealCurrent)
                onWidthChanged: Qt.callLater(revealCurrent)
                Component.onCompleted: Qt.callLater(revealCurrent)
                Repeater {
                    model: pane.session ? pane.session.tabs : []
                    KetTabButton {
                        id: documentTab
                        required property var modelData
                        text: modelData.title
                        iconName: pane.icon(modelData.kind)
                        closable: true
                        width: implicitWidth
                        onClicked: pane.session.select(modelData.id)
                        onCloseRequested: pane.session.close(modelData.id)
                        Keys.onLeftPressed: event => { documentTabs.selectRelative(-1); event.accepted = true }
                        Keys.onRightPressed: event => { documentTabs.selectRelative(1); event.accepted = true }
                        KetToolTip { visible: documentTab.hovered; text: modelData.kind === "terminal" ? modelData.title : modelData.path }
                    }
                }
            }
            Rectangle {
                readonly property var strip: KetTheme.tabBarRecipe(KetTheme.dark, KetTheme.zoom)
                implicitHeight: strip.height
                implicitWidth: actions.implicitWidth + pane.px(8)
                color: strip.background
                Rectangle { width: 1; height: parent.height; color: parent.strip.border }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: parent.strip.border }
                Row {
                    id: actions
                    anchors.centerIn: parent
                    KetIconButton { id: addButton; iconName: "plus"; text: "Open content"; onClicked: addMenu.popup(addButton, 0, addButton.height) }
                    KetIconButton { id: allTabsButton; iconName: "chevron-down"; text: "Open tabs"; onClicked: pane.openTabMenu() }
                    KetIconButton { iconName: "panel-right-close"; text: "Hide content pane"; onClicked: pane.closeRequested() }
                }
            }
        }

        Basic.ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            visible: ["changes", "diff", "atlas", "report"].includes(pane.tab)

            Column {
                x: pane.px(16)
                y: pane.px(16)
                width: scroll.availableWidth - 2 * x
                spacing: pane.px(14)

                // The conversation's work report: every item reported, by group.
                TurnReportCard {
                    visible: pane.tab === "report"
                    width: parent.width
                    report: Fixtures.turnReport
                    ledger: true
                }
                KetChangedFiles {
                    visible: pane.tab === "changes"
                    width: parent.width
                    files: Fixtures.changedFiles
                    note: "develop · sample changes"
                    onFileActivated: index => {
                        const file = Fixtures.changedFiles[index]
                        pane.session.open("diff", file.path + "/" + file.name, file.name + " · diff", Fixtures.sampleDiff)
                    }
                    footer: KetButton {
                        text: "Open Source Control"
                        controlSize: KetTheme.ControlSize.Small
                        onClicked: pane.window.showScreen("changes")
                    }
                }
                KetDiffView {
                    id: diff
                    visible: pane.tab === "diff"
                    width: parent.width
                    lines: parseUnified(pane.document ? pane.document.body : "")
                }

                Repeater {
                    model: pane.tab === "atlas" ? [
                        { title: "Onboarding experience", detail: "2 flows · 4 screens · sample Atlas" }
                    ] : []
                    Rectangle {
                        required property var modelData
                        width: parent.width
                        height: atlasColumn.implicitHeight + pane.px(28)
                        radius: pane.px(7)
                        color: KetTheme.t.pageBg
                        border.color: KetTheme.t.panelBorder
                        Column {
                            id: atlasColumn
                            x: pane.px(14)
                            y: pane.px(14)
                            spacing: pane.px(8)
                            Row {
                                spacing: pane.px(8)
                                KetIcon { name: "layers"; box: pane.px(16); color: KetTheme.t.accentText }
                                Text { text: modelData.title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: pane.px(14); font.weight: Font.Medium }
                            }
                            Text { text: modelData.detail; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: pane.px(12) }
                            Row {
                                spacing: pane.px(6)
                                KetButton { text: "Edit"; controlSize: KetTheme.ControlSize.Small; iconName: "edit"; onClicked: pane.window.showScreen("atlas") }
                                KetButton { text: "Open in browser"; controlSize: KetTheme.ControlSize.Small; variant: KetTheme.ButtonVariant.Ghost; iconName: "open"; onClicked: pane.window.notify("Opened in the browser") }
                            }
                        }
                    }
                }
            }
        }

        Loader {
            active: !!pane.planRun
            visible: active && pane.tab === "plan"
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: ConversationPlan { window: pane.window; run: pane.planRun }
        }

        Loader {
            active: pane.tab === "file" || pane.tab === "terminal"
            visible: active
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: ContentDocument { session: pane.session }
        }
        Item {
            visible: !pane.document
            Layout.fillWidth: true
            Layout.fillHeight: true
            Column {
                anchors.centerIn: parent
                spacing: pane.px(12)
                Text { text: "No open content"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: pane.px(14) }
                KetButton { text: "New terminal"; iconName: "terminal"; onClicked: pane.openContent("terminal") }
            }
        }
    }
    KetMenu {
        id: addMenu
        KetMenuItem { text: "New terminal"; iconName: "terminal"; onTriggered: pane.openContent("terminal") }
        KetMenuItem { text: "NavigationRail.qml"; iconName: "file-code"; onTriggered: pane.openFile("NavigationRail.qml") }
        KetMenuItem { text: "WorktreeItem.qml"; iconName: "file-code"; onTriggered: pane.openFile("WorktreeItem.qml") }
        KetMenuItem { text: "Conversation plan"; iconName: "check-all"; enabled: !!pane.planRun; onTriggered: pane.planRequested() }
        KetMenuItem { text: "Changes"; iconName: "compare"; onTriggered: pane.openContent("changes") }
        KetMenuItem { text: "Work report"; iconName: "view-list"; onTriggered: pane.openContent("report") }
        KetMenuItem { text: "onboarding.ketatlas"; iconName: "layers"; onTriggered: pane.openContent("atlas") }
    }
    KetMenu {
        id: tabMenu
        Repeater {
            model: pane.session ? pane.session.tabs : []
            KetMenuItem {
                required property var modelData
                text: modelData.title
                iconName: pane.icon(modelData.kind)
                checkable: true
                checked: pane.session.currentId === modelData.id
                onTriggered: pane.session.select(modelData.id)
            }
        }
    }
}
