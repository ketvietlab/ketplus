// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

// The KetPlus Pro window: the sidebar and the current screen, with the app-wide overlays
// (Quick search, the repository picker, dialogs and toasts).
Basic.ApplicationWindow {
    id: app

    // Screens, in sidebar order. A state follows a slash: "workspace/welcome".
    readonly property var screenIds: [
        "workspace", "workspace/welcome", "workspace/running", "workspace/question",
        "workspace/error", "workspace/atlas", "workspace/details", "editor", "changes",
        "worktrees", "connections", "settings", "settings/typography", "settings/account",
        "settings/providers", "settings/archived", "settings/updates", "atlas", "mobile-picker",
        "workspace/compact"
    ]
    property string screen: "workspace"
    property string settingsTab: "appearance"
    property var conversation: Fixtures.repositories[0].conversations[0]
    property string repository: Fixtures.repositories[0].name
    property string machine: Fixtures.machines[0]
    property bool updateReady: true
    property string tagFilter
    property bool compact: false
    property size wideSize: Qt.size(1440, 900)

    function setCompact(on) {
        if (on === compact)
            return
        if (on) {
            wideSize = Qt.size(width, height)
            workspace.detailsOpen = false
            screen = "workspace"
        }
        compact = on
        width = on ? KetTheme.px(480, KetTheme.zoom) : wideSize.width
        height = on ? Math.min(height, KetTheme.px(760, KetTheme.zoom)) : wideSize.height
    }
    property var archived: [
        { number: "1022", title: "Spike the relay handshake", repository: "ketviet", when: "Oct 28" },
        { number: "0871", title: "Old onboarding copy", repository: "ketplus-desktop", when: "Oct 12" }
    ]

    function showScreen(id) {
        const parts = String(id).split("/")
        const name = parts[0]
        const state = parts[1] || ""
        if (state === "compact") {
            setCompact(true)
            return
        }
        setCompact(false)
        if (name === "workspace") {
            const kind = state === "details" ? "activity" : state
            if (kind) {
                for (const r of Fixtures.repositories) {
                    const match = r.conversations.find(c => c.transcript === kind)
                    if (match) {
                        openConversation(r.name, match)
                        break
                    }
                }
            }
            workspace.detailsOpen = state === "details"
        }
        if (name === "settings")
            settingsTab = state || "appearance"
        screen = name
    }

    function openConversation(repositoryName, item) {
        repository = repositoryName
        conversation = item
        screen = "workspace"
    }

    function openEveryOverlay() {
        spotlight.open()
        spotlight.close()
        picker.open()
        picker.close()
        renameDialog.open()
        renameDialog.close()
        deleteDialog.open()
        deleteDialog.close()
        toast.show("Smoke test")
    }

    function notify(message) { toast.show(message) }

    width: 1440
    height: 900
    minimumWidth: compact ? 360 : 1024
    minimumHeight: 640
    visible: true
    title: "KétPlus Pro · Mock"
    color: KetTheme.t.pageBg
    font.family: KetTheme.fontFamily

    Component.onCompleted: {
        showScreen(Mock.initialScreen + (Mock.initialState ? "/" + Mock.initialState : ""))
        if (Mock.initialPanel && screen === "workspace")
            Qt.callLater(() => workspace.openComposerPanel(Mock.initialPanel))
    }

    Shortcut {
        sequences: ["Ctrl+K", "Meta+K"]
        onActivated: spotlight.open()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+M", "Meta+Shift+M"]
        onActivated: app.setCompact(!app.compact)
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: app.showScreen("settings")
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            id: sidebar
            Layout.fillHeight: true
            Layout.preferredWidth: KetTheme.px(240, KetTheme.zoom) // 283 less 15%
            visible: app.screen !== "mobile-picker" && !app.compact
            window: app
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            visible: sidebar.visible
            color: KetTheme.t.sidebarBorder
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: ["workspace", "editor", "changes", "worktrees", "connections", "settings",
                           "atlas", "mobile-picker"].indexOf(app.screen)

            WorkspaceScreen { id: workspace; window: app }
            EditorScreen { window: app }
            SourceControlScreen { window: app }
            WorktreesScreen { window: app }
            ConnectionsScreen { window: app }
            SettingsScreen { window: app }
            AtlasBuilderScreen { window: app }
            MobilePickerScreen { window: app }
        }
    }

    KetCommandPalette {
        id: spotlight
        groups: {
            const query = spotlight.query.toLowerCase()
            if (!query)
                return Fixtures.spotlightGroups
            return Fixtures.spotlightGroups.map(group => ({
                title: group.title,
                items: group.items.filter(item => (item.title + " " + item.detail)
                                                      .toLowerCase().includes(query))
            }))
        }
        placeholderText: "Search files, conversations, Atlases and commands"
        onActivated: item => {
            app.showScreen(item.screen)
            if (item.id.startsWith("chat:")) {
                const number = item.id.slice(5)
                for (const r of Fixtures.repositories)
                    for (const c of r.conversations)
                        if (c.number === number)
                            app.openConversation(r.name, c)
            }
        }
    }

    RepositoryPicker {
        id: picker
        window: app
    }

    KetDialog {
        id: renameDialog
        title: "Rename conversation"
        KetFieldRow {
            width: parent.width
            label: "Name"
            KetTextField {
                id: renameField
                width: parent.width
                text: app.conversation.title
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: renameDialog.reject() },
            KetButton {
                text: "Rename"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    renameDialog.accept()
                    app.notify("Renamed to “" + renameField.text + "”")
                }
            }
        ]
    }

    KetDialog {
        id: deleteDialog
        title: "Delete conversation?"
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "@agent/" + app.conversation.number + " and its transcript are removed from this "
                  + "Mac. Its worktree and files stay. Archive keeps a conversation you may need again."
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(KetTheme.t.textMd, KetTheme.zoom)
            lineHeight: 1.4
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: deleteDialog.reject() },
            KetButton { text: "Archive instead"; onClicked: { deleteDialog.reject(); app.notify("Conversation archived") } },
            KetButton {
                text: "Delete"
                variant: KetTheme.ButtonVariant.Danger
                onClicked: { deleteDialog.accept(); app.notify("Conversation deleted") }
            }
        ]
    }

    property alias renameDialog: renameDialog
    property alias deleteDialog: deleteDialog
    property alias spotlight: spotlight
    property alias picker: picker

    KetToast { id: toast }
}
