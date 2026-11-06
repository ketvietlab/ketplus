// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

// Files & Editor: the explorer, editor tabs, breadcrumb, a read-only code view and the
// status bars, and the terminal below the editor. Folders open and close; files open in tabs.
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

    readonly property var sources: ({
        "NavigationRail.qml": Fixtures.navigationRail,
        "design-system.md": Fixtures.designSystem,
        "licensing.md": Mock.resourceText(":/docs/licensing.md")
    })
    readonly property string source: sources[current] || ""
    // Markdown opens in Preview; each file keeps its own Preview or Source choice.
    property var markdownModes: ({})
    readonly property bool markdown: current.endsWith(".md")
    readonly property bool previewing: markdown && markdownModes[current] !== "source"

    function openFile(name) {
        if (!tabs.includes(name))
            tabs = tabs.concat([name])
        current = name
    }
    function setMarkdownMode(mode) {
        const next = Object.assign({}, markdownModes)
        next[current] = mode
        markdownModes = next
    }

    Component.onCompleted: {
        if (window.editorFile !== "") {
            openFile(window.editorFile)
            if (window.editorSource)
                setMarkdownMode("source")
        }
    }
    Connections {
        target: screen.window
        function onEditorFileChanged() {
            if (screen.window.editorFile !== "")
                screen.openFile(screen.window.editorFile)
        }
        function onEditorSourceChanged() {
            if (screen.markdown)
                screen.setMarkdownMode(screen.window.editorSource ? "source" : "preview")
        }
    }
    readonly property var lines: source.replace(/\n$/, "").split("\n")

    // Code lines coloured by the kit's syntax highlighter (Két tokens), as code blocks are.
    readonly property var highlighted: markdown ? [] : KetTheme.highlightedLines(
        source.replace(/\n$/, ""), current.split(".").pop(), KetTheme.dark)

    // Markdown source: headings in the accent, the rest plain (URLs are not comments).
    function highlight(line, index) {
        if (!markdown)
            return highlighted[index] || ""
        const escape = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/ /g, "&nbsp;")
        return /^#{1,6} /.test(line) ? "<font color='" + KetTheme.t.accentText + "'>" + escape(line) + "</font>"
                                     : escape(line)
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
                                    screen.openFile(modelData.name)
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
                    // The strip's own fill and rule, so overflowing tabs pass beneath the button.
                    Rectangle {
                        readonly property var strip: KetTheme.tabBarRecipe(KetTheme.dark, KetTheme.zoom)
                        implicitHeight: strip.height
                        implicitWidth: terminalButton.implicitWidth + screen.px(8)
                        color: strip.background
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: parent.strip.border }
                        Rectangle { width: 1; height: parent.height - 1; color: parent.strip.border }
                        KetIconButton {
                            id: terminalButton
                            anchors.centerIn: parent
                            iconName: "terminal"
                            text: "Toggle terminal"
                            onClicked: screen.window.editorTerminal = !screen.window.editorTerminal
                        }
                    }
                }

                KetBreadcrumb {
                    Layout.fillWidth: true
                    visible: screen.current !== ""
                    path: screen.markdown ? ["docs", screen.current]
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

                Item {
                    visible: screen.current !== ""
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        id: code
                        anchors.fill: parent
                        visible: !screen.previewing
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
                                font.pixelSize: screen.px(14)
                            }
                            Text {
                                x: screen.px(70)
                                anchors.verticalCenter: parent.verticalCenter
                                textFormat: Text.StyledText
                                text: screen.highlight(modelData, index)
                                color: KetTheme.t.textMain
                                font.family: KetTheme.monoFontFamily
                                font.pixelSize: screen.px(14)
                            }
                            TapHandler { onTapped: screen.cursorLine = index + 1 }
                        }
                    }

                    // Markdown Preview: the rendered document, at most 960 px wide.
                    Flickable {
                        id: preview
                        anchors.fill: parent
                        visible: screen.previewing
                        clip: true
                        contentHeight: document.height + screen.px(160)
                        boundsBehavior: Flickable.StopAtBounds
                        Basic.ScrollBar.vertical: Basic.ScrollBar {}
                        KetMarkdown {
                            id: document
                            x: Math.max(screen.px(32), (preview.width - width) / 2)
                            y: screen.px(28)
                            width: Math.min(preview.width - screen.px(64), screen.px(960))
                            markdown: screen.previewing ? screen.source : ""
                            onLinkActivated: link => screen.window.notify("Opens " + link)
                        }
                    }

                    // The floating Preview/Source switch: centred, 100 px above the document's
                    // bottom edge, on a raised surface.
                    Rectangle {
                        visible: screen.markdown
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: screen.px(100)
                        width: modeSwitch.implicitWidth + screen.px(8)
                        height: modeSwitch.implicitHeight + screen.px(8)
                        radius: screen.px(10)
                        color: KetTheme.t.panelBg
                        border.color: KetTheme.t.panelBorder
                        KetSegmentedControl {
                            id: modeSwitch
                            anchors.centerIn: parent
                            options: ["Preview", "Source"]
                            currentIndex: screen.previewing ? 0 : 1
                            onActivated: index => screen.setMarkdownMode(index === 0 ? "preview" : "source")
                        }
                    }
                }

                TerminalPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: screen.px(280)
                    visible: screen.window.editorTerminal
                    window: screen.window
                    onHideRequested: screen.window.editorTerminal = false
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
