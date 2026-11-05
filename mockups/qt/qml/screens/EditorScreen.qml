// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Files & Editor: the explorer, editor tabs, breadcrumb, a read-only code view and the
// status bars. Folders open and close; files open in tabs.
Item {
    id: screen

    required property var window
    property var openFolders: Fixtures.explorer.filter(f => f.folder && f.open).map(f => f.name)
    property var tabs: ["NavigationRail.qml", "design-system.md"]
    property string current: "NavigationRail.qml"
    property int cursorLine: 8

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    // A row shows when every folder above it is open.
    readonly property var visibleRows: {
        const result = []
        const closedAt = []
        for (const row of Fixtures.explorer) {
            while (closedAt.length && closedAt[closedAt.length - 1] >= row.depth)
                closedAt.pop()
            if (closedAt.length === 0) {
                result.push(row)
                if (row.folder && !openFolders.includes(row.name))
                    closedAt.push(row.depth)
            }
        }
        return result
    }

    readonly property string source: current.endsWith(".md") ? Fixtures.designSystem
                                                            : Fixtures.navigationRail
    readonly property var lines: source.replace(/\n$/, "").split("\n")

    // A small QML highlighter for the sample; colours are semantic tokens.
    function highlight(line) {
        const escape = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/ /g, "&nbsp;")
        const comment = line.indexOf("//")
        let code = comment >= 0 ? line.slice(0, comment) : line
        let html = escape(code)
            .replace(/(&quot;|")([^"]*)"/g, "<font color='" + KetTheme.t.success + "'>\"$2\"</font>")
            .replace(/\b(import|property|string|var|id)\b/g, "<font color='" + KetTheme.t.accentText + "'>$1</font>")
            .replace(/^((?:&nbsp;)*)([a-z][A-Za-z.]*)(:)/, "$1<font color='" + KetTheme.t.info + "'>$2</font>$3")
        if (comment >= 0)
            html += "<font color='" + KetTheme.t.textMuted + "'>" + escape(line.slice(comment)) + "</font>"
        return html
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Explorer.
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: screen.px(248)
                color: KetTheme.t.sidebarBg

                Column {
                    anchors.fill: parent

                    KetPaneHeader {
                        width: parent.width
                        title: "EXPLORER"
                        KetIconButton {
                            controlSize: KetTheme.ControlSize.Small
                            iconName: "refresh"
                            text: "Refresh"
                            onClicked: screen.window.notify("Explorer refreshed")
                        }
                    }

                    Repeater {
                        model: screen.visibleRows
                        KetFileRow {
                            required property var modelData
                            width: parent.width
                            text: modelData.root ? modelData.name.toUpperCase() : modelData.name
                            folder: !!modelData.folder
                            depth: modelData.depth
                            status: modelData.status || ""
                            checked: screen.openFolders.includes(modelData.name)
                            active: !modelData.folder && screen.current === modelData.name
                            onClicked: {
                                if (modelData.folder) {
                                    screen.openFolders = checked
                                        ? screen.openFolders.concat([modelData.name])
                                        : screen.openFolders.filter(n => n !== modelData.name)
                                } else {
                                    if (!screen.tabs.includes(modelData.name))
                                        screen.tabs = screen.tabs.concat([modelData.name])
                                    screen.current = modelData.name
                                }
                            }
                        }
                    }
                }

                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: KetTheme.t.panelBorder }
            }

            // Editor.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    KetTabBar {
                        Layout.fillWidth: true
                        currentIndex: screen.tabs.indexOf(screen.current)
                        Repeater {
                            model: screen.tabs
                            KetTabButton {
                                required property string modelData
                                text: modelData
                                iconName: modelData.endsWith(".md") ? "file-text" : "file-code"
                                closable: true
                                width: implicitWidth
                                onClicked: screen.current = modelData
                                onCloseRequested: {
                                    screen.tabs = screen.tabs.filter(t => t !== modelData)
                                    if (screen.current === modelData)
                                        screen.current = screen.tabs[0] || ""
                                }
                            }
                        }
                    }
                    KetIconButton {
                        iconName: "terminal"
                        text: "Toggle terminal"
                        onClicked: screen.window.notify("Terminal opens below the editor")
                    }
                }

                KetBreadcrumb {
                    Layout.fillWidth: true
                    visible: screen.current !== ""
                    path: screen.current.endsWith(".md") ? ["docs", screen.current]
                          : ["modules", "ai", "src", "agent", "qml", screen.current]
                    KetButton {
                        visible: !screen.current.endsWith(".md")
                        text: "View diff"
                        iconName: "compare"
                        variant: KetTheme.ButtonVariant.Ghost
                        controlSize: KetTheme.ControlSize.Small
                        onClicked: screen.window.showScreen("changes")
                    }
                }

                KetEmptyState {
                    visible: screen.current === ""
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    iconName: "file"
                    title: "No file open"
                    message: "Pick a file in the explorer or press ⌘ K."
                    actionText: "Quick search"
                    actionIcon: "search"
                    onActionTriggered: screen.window.spotlight.open()
                }

                ListView {
                    id: code
                    visible: screen.current !== ""
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    topMargin: screen.px(14)
                    model: screen.lines
                    boundsBehavior: Flickable.StopAtBounds
                    Basic.ScrollBar.vertical: Basic.ScrollBar {}
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        width: code.width
                        height: screen.px(24.6)
                        color: index + 1 === screen.cursorLine ? KetTheme.t.accentSubtle : "transparent"
                        Text {
                            width: screen.px(48)
                            anchors.verticalCenter: parent.verticalCenter
                            horizontalAlignment: Text.AlignRight
                            text: index + 1
                            color: index + 1 === screen.cursorLine ? KetTheme.t.textSecondary : KetTheme.t.textMuted
                            font.family: KetTheme.monoFontFamily
                            font.pixelSize: screen.px(13)
                        }
                        Text {
                            x: screen.px(70)
                            anchors.verticalCenter: parent.verticalCenter
                            textFormat: Text.StyledText
                            text: screen.highlight(modelData)
                            color: KetTheme.t.textMain
                            font.family: KetTheme.monoFontFamily
                            font.pixelSize: screen.px(13)
                        }
                        TapHandler { onTapped: screen.cursorLine = index + 1 }
                    }
                }

                KetStatusBar {
                    Layout.fillWidth: true
                    visible: screen.current !== ""
                    trailing: [
                        KetStatusItem { text: "Ln " + screen.cursorLine + ", Col 34" },
                        KetStatusItem { text: "Spaces: 4" },
                        KetStatusItem { text: screen.current.endsWith(".md") ? "Markdown" : "QML" },
                        KetStatusItem { text: "UTF-8" }
                    ]
                }
            }
        }

        KetStatusBar {
            Layout.fillWidth: true
            KetStatusItem { text: "Local workspace"; iconName: "success" }
            trailing: [
                KetStatusItem { text: "UTF-8" },
                KetStatusItem { text: "LF" },
                KetStatusItem { text: "KétPlus Pro" }
            ]
        }
    }
}
