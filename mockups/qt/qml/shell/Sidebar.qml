// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// The main sidebar: brand, Quick search, the four destinations, the machine and its
// repositories with their conversations, the update notice and the account.
Rectangle {
    id: sidebar

    required property var window
    property var expanded: ({})
    property var showAll: ({})
    readonly property real inset: KetTheme.px(6, KetTheme.zoom)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    color: KetTheme.t.sidebarBg

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Brand.
        Row {
            Layout.leftMargin: sidebar.px(11)
            Layout.topMargin: sidebar.px(16)
            Layout.bottomMargin: sidebar.px(14)
            spacing: sidebar.px(6)

            Image {
                anchors.verticalCenter: parent.verticalCenter
                // The bare KétPlus Pro mark (Két Symbol, CC BY 4.0): two Chàm tones on light,
                // Chàm and a near-white plus on dark.
                source: "qrc:/qt/qml/KetPlusMock/assets/ketplus-mark-bare-"
                        + (KetTheme.dark ? "dark" : "light") + ".svg"
                sourceSize: Qt.size(width * 2, height * 2)
                width: sidebar.px(24)
                height: width
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.StyledText
                // The wordmark is Bold (700); "Pro" is Regular.
                text: "<b>Két<font color='" + KetTheme.t.accentText + "'>Plus</font></b> Pro"
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: sidebar.px(17)
                font.weight: Font.Normal
            }
        }

        // Quick search opens the command palette.
        Basic.AbstractButton {
            id: search
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.preferredHeight: sidebar.px(28)
            hoverEnabled: true
            text: "Quick search"
            Accessible.name: "Quick search"
            onClicked: sidebar.window.spotlight.open()

            background: Rectangle {
                radius: sidebar.px(6)
                color: search.hovered ? KetTheme.t.inputBgHover : KetTheme.t.inputBg
                border.color: search.visualFocus ? KetTheme.t.focusBorder : KetTheme.t.inputBorder
                border.width: search.visualFocus ? 2 : 1
            }

            contentItem: Item {
                KetIcon {
                    x: sidebar.px(9)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    box: sidebar.px(12)
                    color: KetTheme.t.textMuted
                }
                Text {
                    x: sidebar.px(29)
                    anchors.verticalCenter: parent.verticalCenter
                    text: search.text
                    font.weight: Font.Medium
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: sidebar.px(12)
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: sidebar.px(9)
                    anchors.verticalCenter: parent.verticalCenter
                    text: "⌘ K"
                    font.weight: Font.Medium
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: sidebar.px(11)
                }
            }
        }

        // Destinations.
        Column {
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.topMargin: sidebar.px(8)
            spacing: sidebar.px(2)

            Repeater {
                model: [
                    { id: "workspace", text: "Agent Workspace", icon: "chats" },
                    { id: "editor", text: "Files & Editor", icon: "folder" },
                    { id: "changes", text: "Source Control", icon: "branch", count: 3 },
                    { id: "connections", text: "Connections", icon: "network" }
                ]

                KetNavItem {
                    required property var modelData
                    width: parent.width
                    font.weight: Font.Medium
                    text: modelData.text
                    iconName: modelData.icon
                    count: modelData.count || -1
                    checkable: false
                    checked: sidebar.window.screen === modelData.id
                             || (modelData.id === "workspace"
                                 && ["worktrees", "atlas", "teams"].includes(sidebar.window.screen))
                    onClicked: sidebar.window.showScreen(modelData.id)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.topMargin: sidebar.px(8)
            Layout.preferredHeight: 1
            color: KetTheme.t.sidebarBorder
        }

        // The machine whose repositories the list shows.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: sidebar.px(40)

            Text {
                x: sidebar.px(14)
                anchors.verticalCenter: parent.verticalCenter
                text: sidebar.window.machine
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: sidebar.px(13)
                font.weight: Font.Medium
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: sidebar.px(12)
                anchors.verticalCenter: parent.verticalCenter
                spacing: sidebar.px(2)

                KetIconButton {
                    id: machineButton
                    controlSize: KetTheme.ControlSize.Small
                    iconName: "chevron-down"
                    text: "Choose machine"
                    onClicked: machineMenu.popup(machineButton, 0, machineButton.height)
                }
                KetIconButton {
                    controlSize: KetTheme.ControlSize.Small
                    iconName: "plus"
                    text: "Open repository"
                    onClicked: sidebar.window.picker.open()
                }
            }

            KetMenu {
                id: machineMenu
                Repeater {
                    model: Fixtures.machines
                    KetMenuItem {
                        required property string modelData
                        text: modelData
                        iconName: modelData === "This Mac" ? "display" : "terminal"
                        checkable: true
                        checked: sidebar.window.machine === modelData
                        onTriggered: sidebar.window.machine = modelData
                    }
                }
                KetMenuSeparator {}
                KetMenuItem {
                    text: "Manage connections"
                    iconName: "network"
                    onTriggered: sidebar.window.showScreen("connections")
                }
            }
        }

        // An active tag filter.
        Rectangle {
            visible: sidebar.window.tagFilter !== ""
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.bottomMargin: sidebar.px(6)
            Layout.preferredHeight: sidebar.px(32)
            radius: sidebar.px(6)
            color: KetTheme.t.accentSubtle
            border.color: KetTheme.t.accentBorder
            Row {
                x: sidebar.px(10)
                anchors.verticalCenter: parent.verticalCenter
                spacing: sidebar.px(6)
                KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "filter"; box: sidebar.px(12); color: KetTheme.t.accentText }
                Text { anchors.verticalCenter: parent.verticalCenter; text: "Tagged " + sidebar.window.tagFilter; color: KetTheme.t.accentText; font.family: KetTheme.fontFamily; font.pixelSize: sidebar.px(12); font.weight: Font.Medium }
            }
            KetIconButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                controlSize: KetTheme.ControlSize.Small
                iconName: "close"
                text: "Clear filter"
                onClicked: sidebar.window.tagFilter = ""
            }
        }

        // Repositories and conversations.
        Basic.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Column {
                width: parent.width

                Repeater {
                    model: sidebar.window.machine === "This Mac" ? Fixtures.repositories
                                                                 : Fixtures.repositories.slice(0, 1)

                    Column {
                        id: repo
                        required property var modelData
                        readonly property bool open: sidebar.expanded[modelData.name] !== false
                        readonly property bool all: !!sidebar.showAll[modelData.name]
                        readonly property int limit: 7
                        // Its teams come first, as folders; a conversation of a team shows there
                        // and not among the repository's own.
                        readonly property var teams: sidebar.window.teamList.filter(t => t.repository === modelData.name)
                        readonly property var loose: {
                            const taken = []
                            for (const t of teams)
                                for (const m of t.members)
                                    taken.push(m.number)
                            return modelData.conversations.filter(c => !taken.includes(c.number))
                        }
                        width: parent.width

                        KetGroupHeader {
                            x: sidebar.inset
                            width: parent.width - 2 * sidebar.inset
                            name: repo.modelData.name
                            expanded: repo.open
                            selected: sidebar.window.repository === repo.modelData.name
                            onExpandedChanged: {
                                const next = Object.assign({}, sidebar.expanded)
                                next[repo.modelData.name] = expanded
                                sidebar.expanded = next
                            }
                            onActionTriggered: {
                                sidebar.window.repository = repo.modelData.name
                                sidebar.window.showScreen("workspace/welcome")
                            }
                        }

                        Repeater {
                            model: repo.open ? repo.teams : []

                            TeamFolder {
                                required property var modelData
                                x: sidebar.inset
                                width: repo.width - 2 * sidebar.inset
                                window: sidebar.window
                                team: modelData
                                conversations: repo.modelData.conversations
                                onContextMenuRequested: conversation => {
                                    sidebar.window.conversation = conversation
                                    rowMenu.popup()
                                }
                            }
                        }

                        Repeater {
                            model: !repo.open ? []
                                   : sidebar.window.tagFilter !== ""
                                     ? repo.loose.filter(c => c.tags.some(t => t.label === sidebar.window.tagFilter))
                                   : repo.all ? repo.loose
                                   : repo.loose.slice(0, repo.limit)

                            KetConversationRow {
                                id: row
                                required property var modelData
                                x: sidebar.inset
                                width: repo.width - 2 * sidebar.inset
                                text: modelData.title
                                number: modelData.number
                                tags: modelData.tags
                                status: sidebar.window.planStatus ? sidebar.window.planStatus(modelData.number, modelData.status) : modelData.status
                                checked: sidebar.window.screen === "workspace"
                                         && sidebar.window.conversation.number === modelData.number
                                // The worktree hint waits for a resting pointer, and a press hides it
                                // until the pointer leaves the row: clicking never raises it.
                                property bool tipDismissed: false
                                onHoveredChanged: if (!hovered) tipDismissed = false
                                onPressedChanged: if (pressed) tipDismissed = true
                                KetToolTip {
                                    visible: row.hovered && !row.pressed && !row.tipDismissed
                                    delay: 1200
                                    text: row.modelData.worktree
                                }
                                onClicked: sidebar.window.openConversation(repo.modelData.name,
                                                                           modelData)
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: {
                                        sidebar.window.conversation = row.modelData
                                        rowMenu.popup()
                                    }
                                }
                            }
                        }

                        Basic.AbstractButton {
                            visible: repo.open && !repo.all && sidebar.window.tagFilter === ""
                                     && repo.loose.length > repo.limit
                            x: sidebar.px(28)
                            height: sidebar.px(28)
                            hoverEnabled: true
                            text: "Show more"
                            onClicked: {
                                const next = Object.assign({}, sidebar.showAll)
                                next[repo.modelData.name] = true
                                sidebar.showAll = next
                            }
                            contentItem: Text {
                                verticalAlignment: Text.AlignVCenter
                                text: parent.text
                                color: parent.hovered ? KetTheme.t.textMain : KetTheme.t.textSecondary
                                font.family: KetTheme.fontFamily
                                font.pixelSize: sidebar.px(13)
                                font.weight: Font.Medium
                            }
                            background: null
                        }

                        Item { width: 1; height: sidebar.px(6) }
                    }
                }
            }
        }

        // The journal passed its size limit; the banner opens Settings → Storage.
        KetBanner {
            id: storageBanner
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.bottomMargin: sidebar.px(6)
            visible: sidebar.window.storageWarning
            tone: KetTheme.BannerTone.Warning
            title: "Journal is " + Fixtures.journalStorage.totalGb.toFixed(1) + " GB"
            message: "Over its " + Fixtures.journalStorage.limitGb + " GB limit"
            Accessible.role: Accessible.Button
            TapHandler { onTapped: sidebar.window.showScreen("settings/storage") }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        KetUpdatePill {
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.bottomMargin: sidebar.px(6)
            visible: sidebar.window.updateReady
            text: "Update ready"
            detail: "KétPlus Pro 1.8.0 · Restart to install"
            onClicked: sidebar.window.showScreen("settings/updates")
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: KetTheme.t.sidebarBorder
        }

        // Account.
        Basic.AbstractButton {
            id: account
            Layout.fillWidth: true
            Layout.preferredHeight: sidebar.px(48)
            hoverEnabled: true
            text: "An Nguyen"
            onClicked: accountMenu.popup(account, sidebar.px(8), -accountMenu.height)
            background: Rectangle {
                color: account.hovered ? KetTheme.t.surfaceHover : "transparent"
            }
            contentItem: Item {
                KetAvatar {
                    id: avatar
                    x: sidebar.px(13)
                    anchors.verticalCenter: parent.verticalCenter
                    name: account.text
                }
                Column {
                    x: avatar.x + avatar.width + sidebar.px(8)
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: account.text
                        color: KetTheme.t.textMain
                        font.family: KetTheme.fontFamily
                        font.pixelSize: sidebar.px(12)
                        font.weight: Font.Medium
                    }
                    Text {
                        text: "Personal workspace"
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: sidebar.px(10)
                        font.weight: Font.Medium
                    }
                }
                KetIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: sidebar.px(16)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-down"
                    box: sidebar.px(14)
                    color: KetTheme.t.textMuted
                }
            }
        }
    }

    KetMenu {
        id: accountMenu
        KetMenuItem { text: "Settings"; iconName: "settings"; shortcut: "⌘ ,"; onTriggered: sidebar.window.showScreen("settings") }
        KetMenuItem { text: "Account & devices"; iconName: "smartphone"; onTriggered: sidebar.window.showScreen("settings/account") }
        KetMenuItem { text: "Compact window"; iconName: "panel-left"; shortcut: "⇧⌘M"; onTriggered: sidebar.window.setCompact(true) }
        KetMenuItem { text: "Check for updates"; iconName: "download"; onTriggered: sidebar.window.showScreen("settings/updates") }
        KetMenuSeparator {}
        KetMenuItem { text: "Sign out"; iconName: "log-out"; onTriggered: sidebar.window.notify("Signed out of the demo account") }
    }

    KetMenu {
        id: rowMenu
        KetMenuItem { text: "Rename…"; iconName: "edit"; onTriggered: sidebar.window.renameDialog.open() }
        KetMenuItem { text: "Fork conversation"; iconName: "branch"; onTriggered: sidebar.window.notify("Forked into a new conversation") }
        KetMenuItem { text: "Change worktree…"; iconName: "branch"; onTriggered: sidebar.window.picker.open() }
        KetMenuItem { text: "Copy reference"; iconName: "copy"; shortcut: "@agent/" + sidebar.window.conversation.number; onTriggered: sidebar.window.notify("Copied @agent/" + sidebar.window.conversation.number) }
        KetMenuSeparator {}
        KetMenuItem { text: "Archive"; iconName: "archive"; onTriggered: sidebar.window.notify("Conversation archived") }
        KetMenuItem { text: "Delete…"; iconName: "trash"; danger: true; onTriggered: sidebar.window.deleteDialog.open() }
    }
}
