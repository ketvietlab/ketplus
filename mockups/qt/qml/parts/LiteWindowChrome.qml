// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import Ket

// The top of the KétPlus window on Windows and Linux, where the system has no global menu
// bar: the title bar (drawn here because the mock window is frameless) and the menu bar in
// the window, with the menus of core/src/app/MainWindowActions.cpp (File, Edit, Go, Git,
// View). Windows puts the title on the left and square caption buttons on the right; Linux
// (GNOME) centres the title and draws round caption buttons.
Column {
    id: chrome

    required property var window
    property string platform: "windows" // or "linux"
    property string title: "KétPlus"

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    readonly property real barHeight: px(32)

    // Title bar.
    Rectangle {
        width: chrome.width
        height: chrome.barHeight
        color: KetTheme.t.sidebarBg

        Text {
            anchors.verticalCenter: parent.verticalCenter
            x: chrome.platform === "linux" ? (parent.width - width) / 2 : chrome.px(KetTheme.t.space3)
            text: chrome.title
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: chrome.px(KetTheme.t.textSm)
            font.weight: chrome.platform === "linux" ? Font.Medium : Font.Normal
        }

        // Windows caption buttons: 46 × 32, close turns red on hover.
        Row {
            visible: chrome.platform === "windows"
            anchors.right: parent.right
            height: parent.height
            Repeater {
                model: ["minimize", "maximize", "close"]
                Rectangle {
                    id: caption
                    required property string modelData
                    width: chrome.px(46)
                    height: parent.height
                    color: !hover.hovered ? "transparent"
                           : modelData === "close" ? KetTheme.t.danger : KetTheme.t.surfaceHover
                    HoverHandler { id: hover }
                    Rectangle {
                        visible: caption.modelData === "minimize"
                        anchors.centerIn: parent
                        width: chrome.px(10); height: 1
                        color: KetTheme.t.textMain
                    }
                    Rectangle {
                        visible: caption.modelData === "maximize"
                        anchors.centerIn: parent
                        width: chrome.px(10); height: chrome.px(10)
                        color: "transparent"
                        border.width: 1
                        border.color: KetTheme.t.textMain
                    }
                    KetIcon {
                        visible: caption.modelData === "close"
                        anchors.centerIn: parent
                        name: "close"
                        size: 14
                        color: hover.hovered ? "white" : KetTheme.t.textMain
                    }
                    TapHandler { onTapped: chrome.window.notify("The system handles " + caption.modelData) }
                }
            }
        }

        // Linux (GNOME) caption buttons: round, 24 px, 8 px apart.
        Row {
            visible: chrome.platform === "linux"
            anchors.right: parent.right
            anchors.rightMargin: chrome.px(KetTheme.t.space2)
            anchors.verticalCenter: parent.verticalCenter
            spacing: chrome.px(KetTheme.t.space2)
            Repeater {
                model: ["minimize", "maximize", "close"]
                Rectangle {
                    id: round
                    required property string modelData
                    width: chrome.px(24); height: width
                    radius: width / 2
                    color: roundHover.hovered ? KetTheme.t.interactiveHover : KetTheme.t.interactiveActive
                    HoverHandler { id: roundHover }
                    Rectangle {
                        visible: round.modelData === "minimize"
                        anchors.centerIn: parent
                        width: chrome.px(8); height: 1
                        color: KetTheme.t.textMain
                    }
                    Rectangle {
                        visible: round.modelData === "maximize"
                        anchors.centerIn: parent
                        width: chrome.px(8); height: chrome.px(8)
                        color: "transparent"
                        border.width: 1
                        border.color: KetTheme.t.textMain
                    }
                    KetIcon {
                        visible: round.modelData === "close"
                        anchors.centerIn: parent
                        name: "close"
                        size: 12
                    }
                    TapHandler { onTapped: chrome.window.notify("The system handles " + round.modelData) }
                }
            }
        }
    }

    // Menu bar.
    Rectangle {
        width: chrome.width
        height: chrome.px(28)
        color: KetTheme.t.sidebarBg

        Row {
            anchors.left: parent.left
            anchors.leftMargin: chrome.px(KetTheme.t.space1)
            height: parent.height

            Repeater {
                model: [
                    { title: "File", items: [
                        { text: "New Tab", shortcut: "Ctrl+T" },
                        { text: "Open…", shortcut: "Ctrl+O" },
                        { text: "Open Folder…", shortcut: "Ctrl+K, Ctrl+O", screen: "explorer/welcome" },
                        { separator: true },
                        { text: "Close Tab", shortcut: "Ctrl+W" },
                        { text: "Reopen Closed Tab", shortcut: "Ctrl+Shift+T" },
                        { separator: true },
                        { text: "Save", shortcut: "Ctrl+S" },
                        { text: "Save As…", shortcut: "Ctrl+Shift+S" },
                        { text: "Save All", shortcut: "Ctrl+Alt+S" },
                        { separator: true },
                        { text: "Quit", shortcut: "Ctrl+Q" }
                    ] },
                    { title: "Edit", items: [
                        { text: "Undo", shortcut: "Ctrl+Z" },
                        { text: "Redo", shortcut: "Ctrl+Shift+Z" },
                        { separator: true },
                        { text: "Cut", shortcut: "Ctrl+X" },
                        { text: "Copy", shortcut: "Ctrl+C" },
                        { text: "Paste", shortcut: "Ctrl+V" },
                        { text: "Select All", shortcut: "Ctrl+A" },
                        { separator: true },
                        { text: "Find…", shortcut: "Ctrl+F" },
                        { text: "Replace…", shortcut: "Ctrl+H" },
                        { separator: true },
                        { text: "Add Next Occurrence", shortcut: "Ctrl+D" },
                        { text: "Add Cursor Below", shortcut: "Ctrl+Alt+Down" },
                        { separator: true },
                        { text: "Settings…", shortcut: "Ctrl+,", screen: "settings" }
                    ] },
                    { title: "Go", items: [
                        { text: "Command Palette…", shortcut: "Ctrl+Shift+P", palette: true },
                        { text: "Go to File…", shortcut: "Ctrl+P", palette: true },
                        { text: "Go to Symbol…", shortcut: "Ctrl+Shift+O" },
                        { text: "Go to Definition", shortcut: "F12" },
                        { text: "Go to Line…", shortcut: "Ctrl+L" },
                        { separator: true },
                        { text: "Back", shortcut: "Alt+Left" },
                        { text: "Forward", shortcut: "Alt+Right" }
                    ] },
                    { title: "Git", items: [
                        { text: "Source Control", shortcut: "Ctrl+Shift+G", screen: "changes" },
                        { text: "Switch Worktree" }
                    ] },
                    { title: "View", items: [
                        { text: "Explorer", shortcut: "Ctrl+B", screen: "explorer" },
                        { text: "Search in Files", shortcut: "Ctrl+Shift+F", screen: "search" },
                        { separator: true },
                        { text: "Terminal", shortcut: "Ctrl+`", terminal: true },
                        { separator: true },
                        { text: "Zoom In", shortcut: "Ctrl+=" },
                        { text: "Zoom Out", shortcut: "Ctrl+-" },
                        { text: "Reset Zoom", shortcut: "Ctrl+0" }
                    ] }
                ]

                Rectangle {
                    id: entry
                    required property var modelData
                    width: label.implicitWidth + chrome.px(KetTheme.t.space4)
                    height: parent.height
                    radius: chrome.px(KetTheme.t.radiusSm)
                    color: menu.visible ? KetTheme.t.interactiveActive
                           : entryHover.hovered ? KetTheme.t.surfaceHover : "transparent"
                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: entry.modelData.title
                        color: KetTheme.t.textMain
                        font.family: KetTheme.fontFamily
                        font.pixelSize: chrome.px(KetTheme.t.textSm)
                    }
                    HoverHandler { id: entryHover }
                    TapHandler { onTapped: menu.popup(entry, 0, entry.height) }

                    KetMenu {
                        id: menu
                        Repeater {
                            model: entry.modelData.items
                            KetMenuItem {
                                required property var modelData
                                visible: !modelData.separator
                                height: modelData.separator ? 0 : implicitHeight
                                text: modelData.text || ""
                                shortcut: modelData.shortcut || ""
                                onTriggered: {
                                    if (modelData.screen)
                                        chrome.window.showScreen(modelData.screen)
                                    else if (modelData.palette)
                                        chrome.window.spotlight.open()
                                    else if (modelData.terminal)
                                        chrome.window.editorTerminal = !chrome.window.editorTerminal
                                    else
                                        chrome.window.notify(modelData.text.replace("…", ""))
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: KetTheme.t.sidebarBorder }
    }
}
