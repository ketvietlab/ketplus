// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

// The KétPlus (Lite) window: the editor of core/src/app with one side panel at a time
// (Explorer, Search in Files or Source Control), the terminal under the editor and
// Settings with the pages Lite has. On Windows and Linux the window draws its own title bar
// and menu bar (LiteWindowChrome); on macOS the system draws both.
Basic.ApplicationWindow {
    id: app

    // Screens, in the order of the activity bar. A state follows a slash: "explorer/terminal".
    readonly property var screenIds: [
        "explorer", "explorer/terminal", "explorer/markdown", "explorer/welcome",
        "explorer/windows", "explorer/linux", "search", "search/no-results", "changes",
        "settings", "settings/typography"
    ]
    property string screen: "explorer"
    // The platform the window chrome follows; --chrome sets it, "/windows" and "/linux" too.
    property string platform: Mock.chrome
    property bool welcome: false

    // What the reused Pro screens read from their window.
    property bool editorTerminal: false
    property string editorFile: ""
    property bool editorSource: false
    property string settingsTab: "appearance"
    property string repository: Fixtures.repositories[0].name
    property bool updateReady: false
    property bool storageWarning: false
    property var providerIssues: ({})
    property var archived: []
    function openAgentModels() { notify("KétPlus has no agents") }

    readonly property string modKey: platform === "macos" ? "⌘ " : "Ctrl "

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    function showScreen(id) {
        const parts = String(id).split("/")
        const name = parts[0]
        const state = parts[1] || ""
        platform = state === "windows" || state === "linux" ? state : Mock.chrome
        welcome = state === "welcome"
        if (name === "explorer") {
            editorTerminal = state === "terminal"
            editorFile = state === "markdown" ? "licensing.md" : ""
            editorSource = false
        }
        if (name === "search")
            search.query = state === "no-results" ? "KetGradient" : "KetTheme"
        if (name === "settings")
            settingsTab = state || "appearance"
        screen = name
    }

    function openEveryOverlay() {
        spotlight.open()
        spotlight.close()
        toast.show("Smoke test")
    }

    function notify(message) { toast.show(message) }

    width: 1280
    height: 820
    minimumWidth: 800
    minimumHeight: 560
    visible: true
    title: "KétPlus · Mock"
    flags: platform === "macos" ? Qt.Window : Qt.Window | Qt.FramelessWindowHint
    color: KetTheme.t.pageBg
    font.family: KetTheme.fontFamily

    Component.onCompleted: showScreen(Mock.initialScreen + (Mock.initialState ? "/" + Mock.initialState : ""))

    Shortcut {
        sequences: ["Ctrl+P", "Ctrl+Shift+P"]
        onActivated: spotlight.open()
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: app.showScreen("settings")
    }
    Shortcut {
        sequence: "Ctrl+B"
        onActivated: app.showScreen("explorer")
    }
    Shortcut {
        sequence: "Ctrl+Shift+F"
        onActivated: app.showScreen("search")
    }
    Shortcut {
        sequence: "Ctrl+Shift+G"
        onActivated: app.showScreen("changes")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        LiteWindowChrome {
            Layout.fillWidth: true
            visible: app.platform !== "macos"
            window: app
            platform: app.platform === "linux" ? "linux" : "windows"
            title: (app.welcome ? "" : (app.screen === "settings" ? "Settings" : editor.current) + " — ")
                   + app.repository + " — KétPlus"
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // The activity bar: one side panel at a time, Settings at the bottom.
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: app.px(48)
                color: KetTheme.t.sidebarBg

                Column {
                    anchors.top: parent.top
                    anchors.topMargin: app.px(KetTheme.t.space2)
                    width: parent.width
                    spacing: app.px(KetTheme.t.space1)
                    Repeater {
                        model: [
                            { id: "explorer", icon: "folder", text: "Explorer (" + app.modKey + "B)" },
                            { id: "search", icon: "search", text: "Search in Files (" + app.modKey + "Shift F)" },
                            { id: "changes", icon: "branch", text: "Source Control (" + app.modKey + "Shift G)" }
                        ]
                        ActivityButton { required property var modelData; entry: modelData }
                    }
                }
                ActivityButton {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: app.px(KetTheme.t.space2)
                    entry: ({ id: "settings", icon: "settings", text: "Settings (" + app.modKey + ",)" })
                }
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: KetTheme.t.sidebarBorder }
            }

            SearchPane {
                id: search
                Layout.fillHeight: true
                Layout.preferredWidth: app.px(280)
                visible: app.screen === "search" && !app.welcome
                window: app
                onLineChosen: (file, line) => {
                    if (!editor.tabs.includes(file))
                        editor.tabs = editor.tabs.concat([file])
                    editor.current = file
                    editor.cursorLine = line
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: app.welcome ? 3 : ["explorer", "changes", "settings"].indexOf(
                                                    app.screen === "search" ? "explorer" : app.screen)

                EditorScreen {
                    id: editor
                    window: app
                    explorerVisible: app.screen === "explorer"
                    productName: "KétPlus"
                    quickOpenKey: app.modKey + "P"
                }
                SourceControlScreen { window: app }
                SettingsScreen { window: app; tabIds: ["appearance", "typography"]; shortcut: app.modKey + "," }

                // No folder open: the first run, or after File → Close Folder.
                Rectangle {
                    color: KetTheme.t.pageBg
                    Column {
                        anchors.centerIn: parent
                        width: app.px(360)
                        spacing: app.px(KetTheme.t.space6)

                        KetEmptyState {
                            width: parent.width
                            iconName: "folder-open"
                            title: "Open a folder to start"
                            message: "KétPlus shows its files, searches them and tracks their changes."
                            actionText: "Open Folder…"
                            actionIcon: "folder"
                            onActionTriggered: app.showScreen("explorer")
                        }

                        Column {
                            width: parent.width
                            spacing: app.px(KetTheme.t.space1)
                            Text {
                                text: "Recent folders"
                                color: KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: app.px(KetTheme.t.textSm)
                            }
                            Repeater {
                                model: [
                                    { name: "ketplus-desktop", path: "~/dev/ketplus-desktop" },
                                    { name: "ket-qt", path: "~/dev/ket-qt" },
                                    { name: "ket-sans-font", path: "~/dev/ket-sans-font" }
                                ]
                                KetFileRow {
                                    required property var modelData
                                    width: parent.width
                                    iconName: "folder"
                                    text: modelData.name + "   " + modelData.path
                                    onClicked: {
                                        app.repository = modelData.name
                                        app.showScreen("explorer")
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // An activity-bar entry: selected when its panel is the one shown.
    component ActivityButton: Item {
        id: button
        required property var entry
        readonly property bool selected: app.screen === entry.id && !app.welcome
        width: app.px(48)
        height: app.px(40)

        Rectangle {
            anchors.left: parent.left
            width: 2
            height: parent.height
            visible: button.selected
            color: KetTheme.t.accent
        }
        KetIconButton {
            anchors.centerIn: parent
            iconName: button.entry.icon
            text: button.entry.text
            onClicked: app.showScreen(button.entry.id)
            background: Rectangle {
                radius: app.px(KetTheme.t.radiusMd)
                color: button.selected ? KetTheme.t.navSelected
                       : parent.hovered ? KetTheme.t.navSelectedHover : "transparent"
            }
        }
    }

    KetCommandPalette {
        id: spotlight
        groups: {
            const query = spotlight.query.toLowerCase()
            const files = Fixtures.explorer.filter(f => !f.folder).map(f => ({
                id: "file:" + f.name, title: f.name, detail: f.path || "", screen: "explorer"
            }))
            const all = [{ title: "Files", items: files }]
            if (!query)
                return all
            return all.map(group => ({
                title: group.title,
                items: group.items.filter(item => (item.title + " " + item.detail).toLowerCase().includes(query))
            }))
        }
        placeholderText: "Go to file"
        onActivated: item => {
            app.showScreen(item.screen)
            const file = item.id.slice(5)
            if (!editor.tabs.includes(file))
                editor.tabs = editor.tabs.concat([file])
            editor.current = file
        }
    }

    property alias spotlight: spotlight

    KetToast { id: toast }
}
