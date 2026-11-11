// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Repository & worktree: choose where the conversation's next turn runs. The current turn
// keeps its worktree; the choice applies from the next turn.
KetDialog {
    id: picker

    required property var window
    property int repository: 0
    property int worktree: 0
    property string repoQuery
    property string branchQuery
    readonly property var repos: Fixtures.pickerRepositories.filter(r => r.name.includes(repoQuery))
    readonly property var current: repos[Math.min(repository, repos.length - 1)] || null
    readonly property var branches: current ? current.worktrees.filter(w => (w.branch + w.path).includes(branchQuery)) : []

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    title: "Repository & worktree"
    width: px(800)

    // The dialog's body is a layout: sized from the dialog, never from this row's own width, which
    // would grow it on every pass.
    Row {
        Layout.fillWidth: true
        height: px(320)
        spacing: px(12)

        Column {
            width: picker.px(222)
            spacing: picker.px(8)
            KetTextField { width: parent.width; leadingIcon: "search"; placeholderText: "Search repositories…"; onTextChanged: picker.repoQuery = text }
            Repeater {
                model: picker.repos
                KetTreeItem {
                    required property var modelData
                    required property int index
                    width: parent.width
                    text: modelData.name
                    iconName: "folder"
                    count: String(modelData.count)
                    activity: modelData.agent ? "working" : ""
                    activityText: modelData.agent ? "Agent is working here" : ""
                    checked: picker.repository === index
                    onClicked: { picker.repository = index; picker.worktree = 0 }
                }
            }
        }

        Rectangle { width: 1; height: parent.height; color: KetTheme.t.panelBorder }

        Column {
            width: picker.availableWidth - picker.px(222) - 1 - 2 * picker.px(12)
            spacing: picker.px(8)
            KetTextField { width: parent.width; leadingIcon: "search"; placeholderText: "Search branch or folder…"; onTextChanged: picker.branchQuery = text }
            Repeater {
                model: picker.branches
                KetTreeItem {
                    required property var modelData
                    required property int index
                    width: parent.width
                    text: modelData.branch + (modelData.main ? "  · Main" : "")
                    detail: modelData.path
                    iconName: "branch"
                    checked: picker.worktree === index
                    onClicked: picker.worktree = index
                }
            }
            Text {
                visible: picker.branches.length === 0
                text: "No worktree matches."
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: picker.px(13)
            }
        }
    }

    actions: [
        KetButton { text: "Create worktree…"; iconName: "plus"; variant: KetTheme.ButtonVariant.Ghost; onClicked: { picker.reject(); picker.window.createWorktreeDialog.open() } },
        KetButton {
            text: "Use this repository"
            variant: KetTheme.ButtonVariant.Primary
            enabled: picker.branches.length > 0
            onClicked: {
                const branch = picker.branches[Math.min(picker.worktree, picker.branches.length - 1)]
                picker.accept()
                picker.window.notify("Next turn: " + picker.current.name + " · " + branch.branch)
            }
        }
    ]
}
