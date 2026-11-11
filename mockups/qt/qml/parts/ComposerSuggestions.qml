// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket

// The composer's suggestion popover, as the app draws it (AgentComposer skillMentionPalette and
// slashPalette): above the composer, raised surface, one row per suggestion. Files show their
// worktree path and File or Folder; conversations and skills show the mention, then what it is.
Rectangle {
    id: popover

    // Each row: {kind: "file"|"conversation"|"skill"|"command", ...}.
    property var suggestions: []
    property int currentIndex: 0

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    readonly property int bodyPixels: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
    readonly property int smallPixels: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
    readonly property int labelPixels: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels

    visible: suggestions.length > 0
    height: list.implicitHeight + px(8)
    radius: px(12)
    color: KetTheme.t.surfaceRaised
    border.width: 1
    border.color: KetTheme.t.borderStrong

    Column {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: popover.px(4)
        spacing: popover.px(2)
        Repeater {
            model: popover.suggestions
            Basic.ItemDelegate {
                id: row
                required property var modelData
                required property int index
                readonly property bool fileEntry: modelData.kind === "file"
                readonly property bool command: modelData.kind === "command"
                width: list.width
                height: popover.px(command ? 42 : 46)
                padding: popover.px(8)
                highlighted: index === popover.currentIndex
                contentItem: Item {
                    ColumnLayout {
                        anchors.fill: parent
                        visible: row.fileEntry
                        spacing: 1
                        Text { Layout.fillWidth: true; text: row.modelData.path || ""; color: KetTheme.t.accentHover; font.family: KetTheme.monoFontFamily; font.pixelSize: popover.bodyPixels; elide: Text.ElideMiddle }
                        Text { text: row.modelData.directory ? "Folder" : "File"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: popover.smallPixels }
                    }
                    RowLayout {
                        anchors.fill: parent
                        visible: !row.fileEntry
                        spacing: popover.px(10)
                        Text { text: row.modelData.mention || ""; color: row.command ? KetTheme.t.textMain : KetTheme.t.accentHover; font.family: KetTheme.monoFontFamily; font.pixelSize: popover.bodyPixels; Layout.preferredWidth: popover.px(row.command ? 92 : 150); elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text { Layout.fillWidth: true; text: row.modelData.description || ""; color: row.command ? KetTheme.t.textMuted : KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: popover.labelPixels; elide: Text.ElideRight }
                            Text { visible: !row.command; text: row.modelData.detail || ""; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: popover.smallPixels }
                        }
                    }
                }
                background: Rectangle {
                    radius: popover.px(8)
                    color: row.highlighted || row.hovered ? KetTheme.t.interactiveActive : "transparent"
                }
            }
        }
    }
}
