// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// The Source | Mermaid switch of a Mermaid block, as the app's TranscriptMermaidSwitch:
// both sides always show, so the reader sees what the block is and what the other side is.
Rectangle {
    id: control

    property bool rendered: false // true while the block shows its diagram

    signal modeRequested(bool rendered)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitWidth: row.implicitWidth + 2 * px(2)
    implicitHeight: px(24)
    radius: px(5)
    color: KetTheme.t.panelBgSubtle
    border.color: KetTheme.t.borderDefault

    Row {
        id: row
        anchors.centerIn: parent

        Repeater {
            model: [{ label: "Source", rendered: false }, { label: "Mermaid", rendered: true }]

            Basic.AbstractButton {
                id: side
                required property var modelData
                readonly property bool current: control.rendered === modelData.rendered
                focusPolicy: Qt.TabFocus
                hoverEnabled: true
                implicitWidth: label.implicitWidth + 2 * control.px(8)
                implicitHeight: control.height - 2 * control.px(2)
                Accessible.name: modelData.rendered ? "Show Mermaid diagram" : "Show Mermaid source"
                Accessible.checked: current
                onClicked: if (!current) control.modeRequested(modelData.rendered)

                contentItem: Text {
                    id: label
                    text: side.modelData.label
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: side.current ? KetTheme.t.textMain : KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: control.px(12)
                    font.weight: side.current ? Font.DemiBold : Font.Normal
                }
                background: Rectangle {
                    radius: control.px(4)
                    color: side.current ? KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom).background
                         : side.hovered ? KetTheme.t.colorSurfaceHover : "transparent"
                    border.color: side.visualFocus ? KetTheme.t.focusBorder : "transparent"
                }
            }
        }
    }
}
