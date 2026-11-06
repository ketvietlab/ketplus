// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// KétPlus Search, in the left column: a query across the open folder, results grouped by
// file with the matching lines, and the empty state when nothing matches. Choosing a line
// opens its file at that line in the editor (a Markdown file in Source).
Rectangle {
    id: pane

    required property var window
    // The files Search reads: name → text, the same sources the editor opens, so a result
    // opens its file at a line that holds the match. `paths` gives each file's folder.
    property var sources: ({})
    property var paths: ({})
    property string query: "theme"
    signal lineChosen(string file, int line)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    readonly property var results: {
        const needle = query.trim().toLowerCase()
        if (needle === "")
            return []
        const groups = []
        for (const file of Object.keys(sources)) {
            const lines = []
            sources[file].split("\n").forEach((text, index) => {
                if (text.toLowerCase().includes(needle))
                    lines.push({ line: index + 1, text: text })
            })
            if (lines.length > 0)
                groups.push({ file: file, path: paths[file] || "", lines: lines })
        }
        return groups
    }
    readonly property int matchCount: results.reduce((sum, group) => sum + group.lines.length, 0)

    // The line with the match in the accent, the rest escaped. A match far into the line
    // keeps 12 characters before it, so the end of a long line elides instead of the match.
    function marked(text) {
        const escape = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;")
        const trimmed = text.trim()
        const at = trimmed.toLowerCase().indexOf(query.trim().toLowerCase())
        if (at < 0)
            return escape(trimmed)
        const end = at + query.trim().length
        const from = at > 16 ? at - 12 : 0
        return (from > 0 ? "…" : "") + escape(trimmed.slice(from, at)) + "<b><font color='" + KetTheme.t.accentText + "'>"
               + escape(trimmed.slice(at, end)) + "</font></b>" + escape(trimmed.slice(end))
    }

    color: KetTheme.t.sidebarBg

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        KetPaneHeader {
            Layout.fillWidth: true
            title: "SEARCH"
            KetIconButton {
                controlSize: KetTheme.ControlSize.Small
                iconName: "refresh"
                text: "Search again"
                onClicked: pane.window.notify("Searched ketplus-desktop again")
            }
        }

        KetTextField {
            id: field
            Layout.fillWidth: true
            Layout.leftMargin: pane.px(KetTheme.t.space3)
            Layout.rightMargin: pane.px(KetTheme.t.space3)
            leadingIcon: "search"
            placeholderText: "Search in ketplus-desktop"
            text: pane.query
            onTextEdited: pane.query = text
        }

        Text {
            Layout.fillWidth: true
            Layout.margins: pane.px(KetTheme.t.space3)
            visible: pane.results.length > 0
            text: pane.matchCount + (pane.matchCount === 1 ? " result in " : " results in ")
                  + pane.results.length + (pane.results.length === 1 ? " file" : " files")
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: pane.px(KetTheme.t.textSm)
        }

        Basic.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: pane.results.length > 0
            clip: true
            contentWidth: availableWidth

            Column {
                width: parent.width

                Repeater {
                    model: pane.results
                    Column {
                        id: group
                        required property var modelData
                        width: parent.width

                        KetFileRow {
                            width: parent.width
                            text: group.modelData.file
                            status: String(group.modelData.lines.length)
                            onClicked: pane.lineChosen(group.modelData.file, group.modelData.lines[0].line)
                        }
                        Repeater {
                            model: group.modelData.lines
                            Rectangle {
                                id: match
                                required property var modelData
                                width: parent.width
                                height: pane.px(24)
                                color: tap.hovered ? KetTheme.t.surfaceHover : "transparent"
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: pane.px(KetTheme.t.space8)
                                    anchors.rightMargin: pane.px(KetTheme.t.space3)
                                    anchors.verticalCenter: parent.verticalCenter
                                    elide: Text.ElideRight
                                    textFormat: Text.StyledText
                                    text: pane.marked(match.modelData.text)
                                    color: KetTheme.t.textSecondary
                                    font.family: KetTheme.monoFontFamily
                                    font.pixelSize: pane.px(KetTheme.t.textSm)
                                }
                                HoverHandler { id: tap }
                                TapHandler {
                                    onTapped: pane.lineChosen(group.modelData.file, match.modelData.line)
                                }
                                Basic.ToolTip.visible: tap.hovered
                                Basic.ToolTip.delay: 600
                                Basic.ToolTip.text: group.modelData.path + "/" + group.modelData.file
                                                    + ":" + match.modelData.line
                            }
                        }
                    }
                }
            }
        }

        KetEmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: pane.px(KetTheme.t.space4)
            visible: pane.results.length === 0
            iconName: "search"
            title: pane.query.trim() === "" ? "Search the folder" : "No results"
            message: pane.query.trim() === "" ? "Type a word or symbol to find it in every file."
                     : "Nothing in " + pane.window.repository + " matches “" + pane.query.trim()
                       + "”. Check the spelling, or search for a shorter word."
            actionText: pane.query.trim() === "" ? "" : "Clear search"
            actionIcon: "close"
            onActionTriggered: pane.query = ""
        }
    }

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: KetTheme.t.panelBorder }
}
