// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// Manage worktrees: each worktree's conversations, activity and Git state, with create,
// manage and the archive.
Item {
    id: screen

    required property var window
    property var worktrees: Fixtures.worktrees
    property var managed: null

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    Column {
        x: screen.px(42)
        y: screen.px(38)
        width: parent.width - 2 * x
        spacing: screen.px(24)

        PageHeading {
            width: parent.width
            title: "Worktrees"
            description: "Independent files, conversations and drafts."
            KetButton {
                text: "Create worktree"
                iconName: "plus"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: createDialog.open()
            }
        }

        Rectangle {
            width: parent.width
            height: list.implicitHeight
            radius: screen.px(7)
            color: KetTheme.t.panelBg
            border.color: KetTheme.t.panelBorder

            Column {
                id: list
                width: parent.width
                Repeater {
                    model: screen.worktrees
                    Item {
                        id: row
                        required property var modelData
                        required property int index
                        width: list.width
                        height: screen.px(90)
                        Rectangle { visible: row.index > 0; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                        KetIcon { x: screen.px(22); y: screen.px(28); name: "branch"; box: screen.px(14); color: KetTheme.t.textSecondary }
                        Column {
                            x: screen.px(46)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(6)
                            Text { text: row.modelData.name; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(15); font.weight: Font.Medium }
                            Text { text: row.modelData.conversations + " conversations"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        }
                        Row {
                            x: screen.px(222)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(20)
                            Text { anchors.verticalCenter: parent.verticalCenter; text: row.modelData.working + " working"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14) }
                            KetBadge { anchors.verticalCenter: parent.verticalCenter; text: row.modelData.state; tone: row.modelData.dirty ? KetTheme.BadgeTone.Warning : KetTheme.BadgeTone.Neutral }
                        }
                        KetButton {
                            id: manage
                            anchors.right: parent.right
                            anchors.rightMargin: screen.px(16)
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Manage"
                            iconName: "more"
                            variant: KetTheme.ButtonVariant.Ghost
                            onClicked: {
                                screen.managed = row.modelData
                                manageMenu.popup(manage, 0, manage.height)
                            }
                        }
                    }
                }
            }
        }

        KetButton {
            text: "Archived worktrees and conversations"
            iconName: "archive"
            variant: KetTheme.ButtonVariant.Ghost
            onClicked: screen.window.showScreen("settings/archived")
        }
    }

    KetMenu {
        id: manageMenu
        KetMenuItem { text: "New conversation here"; iconName: "compose"; onTriggered: screen.window.showScreen("workspace/welcome") }
        KetMenuItem { text: "Open in Files & Editor"; iconName: "folder-open"; onTriggered: screen.window.showScreen("editor") }
        KetMenuItem { text: "Reveal in Finder"; iconName: "open"; description: screen.managed ? screen.managed.path : ""; onTriggered: screen.window.notify("Revealed in Finder") }
        KetMenuSeparator {}
        KetMenuItem { text: "Archive worktree"; iconName: "archive"; onTriggered: screen.window.notify("Archived " + screen.managed.name) }
        KetMenuItem {
            text: "Delete worktree…"
            iconName: "trash"
            danger: true
            enabled: screen.managed && screen.managed.name !== "develop"
            onTriggered: screen.window.notify("Deleting needs a clean worktree with no running Agent")
        }
    }

    KetDialog {
        id: createDialog
        title: "Create worktree"
        KetFieldRow {
            width: parent.width
            label: "Branch name"
            help: "A new folder next to the repository: ketplus-desktop-<name>."
            KetTextField { id: branchName; width: parent.width; placeholderText: "feature/settings-search" }
        }
        KetFieldRow {
            width: parent.width
            label: "Start from"
            KetSelect { width: parent.width; model: ["develop", "integration", "main"] }
        }
        KetChoice { width: parent.width; text: "Start a conversation in it"; description: "Opens Agent Workspace on the new worktree"; checked: true }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: createDialog.reject() },
            KetButton {
                text: "Create"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    const name = branchName.text || "feature/settings-search"
                    screen.worktrees = screen.worktrees.concat([{ name: name, conversations: 0, working: 0, state: "Clean", dirty: false, path: "/workspace/ketplus-desktop-" + name }])
                    createDialog.accept()
                    screen.window.notify("Created worktree " + name)
                }
            }
        ]
    }
}
