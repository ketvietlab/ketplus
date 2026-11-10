// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// The terminal below the editor, drawn like the KetPlus Terminal mock (mockups/terminal): tabs
// with New tab, Find, Clear scrollback, Restart and Hide beside them, then the grid in KÉT
// Sans Mono 14 px on 22 px lines over page-bg. Output is fixture data; nothing runs.
Rectangle {
    id: panel

    required property var window
    property var tabs: ["ketplus — demo", "~"]
    property int current: 0
    property bool searching: false
    property string query: "Passed"
    property bool cleared: false
    property alias tabButtons: tabRepeater
    signal hideRequested()

    // Here, not in the tab: replacing `tabs` destroys the tab whose handler is running, and the
    // rest of that handler never runs. Closing the last tab hides the terminal.
    function closeTab(index) {
        tabs = tabs.filter((t, i) => i !== index)
        current = Math.max(0, Math.min(current, tabs.length - 1))
        if (tabs.length === 0)
            hideRequested()
    }

    // Shown again after its last tab closed, it starts a new shell, as the app does.
    onVisibleChanged: if (visible && tabs.length === 0) tabs = ["~"]

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    readonly property var lines: cleared ? [Fixtures.terminalLines[Fixtures.terminalLines.length - 1]]
                                         : Fixtures.terminalLines
    readonly property int matches: {
        if (!searching || query === "")
            return 0
        let count = 0
        for (const line of lines)
            count += line.map(s => s[0]).join("").split(query).length - 1
        return count
    }

    function roleColor(role) {
        switch (role) {
        case "muted": return KetTheme.t.textMuted
        case "success": return KetTheme.t.success
        case "danger": return KetTheme.t.danger
        case "warning": return KetTheme.t.warning
        case "info": return KetTheme.t.info
        case "accent": return KetTheme.t.accentText
        default: return KetTheme.t.textMain
        }
    }
    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/ /g, "&nbsp;")
    }
    // Spans as styled text; Find marks every match in the warning colour.
    function render(line) {
        let html = ""
        for (const span of line) {
            let body = escapeHtml(span[0])
            if (searching && query !== "")
                body = body.split(escapeHtml(query)).join("<span style='background-color:"
                    + Qt.alpha(KetTheme.t.warning, 0.35) + "'>" + escapeHtml(query) + "</span>")
            html += "<font color='" + roleColor(span[1]) + "'>" + body + "</font>"
        }
        return html
    }

    color: KetTheme.t.pageBg

    // The editor above ends on this rule.
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: KetTheme.t.panelBorder; z: 1 }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 1
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            KetTabBar {
                Layout.fillWidth: true
                currentIndex: panel.current
                Repeater {
                    id: tabRepeater
                    model: panel.tabs
                    KetTabButton {
                        required property string modelData
                        required property int index
                        text: modelData
                        iconName: "terminal"
                        closable: true
                        width: implicitWidth
                        onClicked: panel.current = index
                        onCloseRequested: panel.closeTab(index)
                    }
                }
            }
            Rectangle {
                readonly property var strip: KetTheme.tabBarRecipe(KetTheme.dark, KetTheme.zoom)
                implicitHeight: strip.height
                implicitWidth: actions.implicitWidth + panel.px(12)
                color: strip.background
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: parent.strip.border }
                Row {
                    id: actions
                    anchors.centerIn: parent
                    spacing: panel.px(2)
                    KetIconButton {
                        iconName: "plus"; text: "New tab"
                        onClicked: { panel.tabs = panel.tabs.concat(["~"]); panel.current = panel.tabs.length - 1 }
                    }
                    KetIconButton { iconName: "search"; text: "Find in terminal"; onClicked: panel.searching = !panel.searching }
                    KetIconButton {
                        iconName: "trash"; text: "Clear terminal scrollback"
                        onClicked: panel.cleared = true
                    }
                    KetIconButton {
                        iconName: "refresh"; text: "Restart terminal session"
                        onClicked: { panel.cleared = false; panel.window.notify("Terminal session restarted") }
                    }
                    KetIconButton { iconName: "close"; text: "Hide terminal"; onClicked: panel.hideRequested() }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: grid
                anchors.fill: parent
                anchors.leftMargin: panel.px(10)
                clip: true
                topMargin: panel.px(8)
                model: panel.lines
                boundsBehavior: Flickable.StopAtBounds
                Basic.ScrollBar.vertical: Basic.ScrollBar {}
                delegate: Item {
                    required property var modelData
                    required property int index
                    width: grid.width
                    height: panel.px(22)
                    Text {
                        id: cells
                        anchors.verticalCenter: parent.verticalCenter
                        textFormat: Text.RichText
                        text: panel.render(modelData)
                        font.family: KetTheme.monoFontFamily
                        font.pixelSize: panel.px(14)
                    }
                    // The block cursor after the prompt on the last line.
                    Rectangle {
                        visible: index === panel.lines.length - 1
                        x: cells.implicitWidth
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.px(8.4)
                        height: panel.px(18)
                        color: KetTheme.t.accent
                    }
                }
            }

            // Find in terminal, top right over the grid, as TerminalView draws it.
            Rectangle {
                visible: panel.searching
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: panel.px(8)
                width: findRow.implicitWidth + panel.px(12)
                height: findRow.implicitHeight + panel.px(8)
                radius: panel.px(6)
                color: KetTheme.t.panelBg
                border.color: KetTheme.t.panelBorder
                Row {
                    id: findRow
                    anchors.centerIn: parent
                    spacing: panel.px(4)
                    KetTextField {
                        width: panel.px(200)
                        text: panel.query
                        placeholderText: "Find in terminal"
                        onTextChanged: panel.query = text
                    }
                    Text {
                        width: panel.px(48)
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: panel.matches ? "1 / " + panel.matches : "0 / 0"
                        color: KetTheme.t.textSecondary
                        font.family: KetTheme.fontFamily
                        font.pixelSize: panel.px(13)
                    }
                    KetIconButton { iconName: "arrow-up"; text: "Previous match" }
                    KetIconButton { iconName: "arrow-down"; text: "Next match" }
                    KetIconButton { iconName: "close"; text: "Close search"; onClicked: panel.searching = false }
                }
            }
        }
    }
}
