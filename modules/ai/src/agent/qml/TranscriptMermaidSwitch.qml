import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

// The Source | Mermaid switch of a Mermaid block. Both sides are always on show,
// so the reader sees what the block is now and what the other side is.
Rectangle {
    id: control

    // True while the block shows its diagram.
    property bool rendered: false
    signal modeRequested(bool rendered)

    implicitWidth: row.implicitWidth + 2 * KetTheme.px(2, KetTheme.zoom)
    implicitHeight: KetTheme.px(24, KetTheme.zoom)
    radius: KetTheme.px(5, KetTheme.zoom)
    color: KetTheme.t.panelBgSubtle
    border.color: KetTheme.t.panelBorder
    border.width: 1

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 0
        Repeater {
            model: [{
                    "label": "Source",
                    "rendered": false
                }, {
                    "label": "Mermaid",
                    "rendered": true
                }]
            delegate: Basic.AbstractButton {
                required property var modelData
                readonly property bool current: control.rendered === modelData.rendered
                objectName: modelData.rendered ? "mermaidSwitchDiagram" : "mermaidSwitchSource"
                focusPolicy: Qt.TabFocus
                implicitWidth: label.implicitWidth + 2 * KetTheme.px(8, KetTheme.zoom)
                implicitHeight: control.height - 2 * KetTheme.px(2, KetTheme.zoom)
                checkable: false
                contentItem: Text {
                    id: label
                    text: modelData.label
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: parent.current ? KetTheme.t.textMain : KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                    font.weight: parent.current ? Font.DemiBold : Font.Normal
                }
                background: Rectangle {
                    radius: KetTheme.px(4, KetTheme.zoom)
                    color: parent.current ? KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom).background : parent.hovered ? KetTheme.t.surfaceHover : "transparent"
                    border.color: parent.visualFocus ? KetTheme.t.colorFocus : "transparent"
                }
                onClicked: if (!current)
                    control.modeRequested(modelData.rendered)
                Accessible.name: modelData.rendered ? "Show Mermaid diagram" : "Show Mermaid source"
                Accessible.checked: current
            }
        }
    }
}
