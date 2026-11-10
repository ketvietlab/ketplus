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
        "editor", "explorer", "explorer/terminal", "explorer/markdown", "explorer/welcome",
        "explorer/windows", "explorer/linux", "search", "search/no-results", "changes",
        "settings", "settings/typography"
    ]
    property string screen: "explorer"
    // The side panel (Explorer, Search or Source Control). Closed when the window opens; its
    // activity-bar button or shortcut opens it, and the same again closes it.
    property bool sidebarOpen: false
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
        // "editor" is the Explorer screen with the side panel closed.
        const name = parts[0] === "editor" ? "explorer" : parts[0]
        const state = parts[1] || ""
        if (name !== "settings")
            sidebarOpen = parts[0] !== "editor"
        platform = state === "windows" || state === "linux" ? state : Mock.chrome
        welcome = state === "welcome"
        if (name === "explorer") {
            editorTerminal = state === "terminal"
            editorFile = state === "markdown" ? "licensing.md" : ""
            editorSource = false
        }
        if (name === "search")
            search.query = state === "no-results" ? "KetGradient" : "theme"
        if (name === "settings")
            settingsTab = state || "appearance"
        screen = name
    }

    // An activity-bar entry or its shortcut: opens that panel, or closes it when it is open.
    function toggleScreen(id) {
        if (screen === id && sidebarOpen && !welcome)
            showScreen("editor")
        else
            showScreen(id)
    }

    function openEveryOverlay() {
        spotlight.open()
        spotlight.close()
        // The open panel's button closes it; pressing it again opens the panel.
        for (const id of ["explorer", "search", "changes"]) {
            showScreen(id)
            toggleScreen(id)
            const closed = !sidebarOpen && screen === "explorer"
            toggleScreen(id)
            if (!closed || !sidebarOpen || screen !== id)
                console.warn("QML sidebar check: " + id + " did not toggle")
        }
        // Closing every terminal tab hides the terminal; showing it again opens a new tab.
        showScreen("editor")
        editorTerminal = true
        while (editor.terminal.tabs.length > 0 && editorTerminal)
            editor.terminal.tabButtons.itemAt(0).closeRequested()
        const hidden = !editorTerminal
        editorTerminal = true
        if (!hidden || editor.terminal.tabs.length !== 1)
            console.warn("QML terminal check: closing every tab left the terminal open")
        editorTerminal = false
        // Every search result opens its file at a line that holds the match.
        showScreen("search")
        for (const group of search.results) {
            for (const match of group.lines) {
                search.lineChosen(group.file, match.line)
                const shown = editor.lines[editor.cursorLine - 1] || ""
                if (editor.current !== group.file || editor.previewing
                        || !shown.toLowerCase().includes(search.query.toLowerCase()))
                    console.warn("QML search check: " + group.file + ":" + match.line
                                 + " opened " + editor.current + ":" + editor.cursorLine)
            }
        }
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
        onActivated: app.toggleScreen("explorer")
    }
    Shortcut {
        sequence: "Ctrl+Shift+F"
        onActivated: app.toggleScreen("search")
    }
    Shortcut {
        sequence: "Ctrl+Shift+G"
        onActivated: app.toggleScreen("changes")
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
                visible: app.screen === "search" && app.sidebarOpen && !app.welcome
                window: app
                sources: editor.sources
                paths: ({ "NavigationRail.qml": "modules/ai/src/agent/qml",
                          "design-system.md": "docs", "licensing.md": "docs" })
                onLineChosen: (file, line) => editor.openAt(file, line)
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: app.welcome ? 3 : ["explorer", "changes", "settings"].indexOf(
                                                    app.screen === "search" ? "explorer" : app.screen)

                EditorScreen {
                    id: editor
                    window: app
                    explorerVisible: app.screen === "explorer" && app.sidebarOpen
                    productName: "KétPlus"
                    singleStatusBar: true
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
                                         && (entry.id === "settings" || app.sidebarOpen)
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
            onClicked: button.entry.id === "settings" ? app.showScreen("settings")
                                                      : app.toggleScreen(button.entry.id)
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
            // The files the editor has a source for, so each one opens with its text.
            const files = Object.keys(editor.sources).map(name => ({
                id: "file:" + name, title: name, detail: search.paths[name] || ""
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
            // The file opens in the editor; an open Explorer or Search stays open.
            if (app.screen !== "explorer" && app.screen !== "search")
                app.showScreen("editor")
            editor.openFile(item.id.slice(5))
        }
    }

    property alias spotlight: spotlight

    KetToast { id: toast }
}
