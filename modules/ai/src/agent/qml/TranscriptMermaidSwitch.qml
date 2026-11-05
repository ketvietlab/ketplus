import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

// The Source | Mermaid switch of a Mermaid block. Both sides are always on show,
// so the reader sees what the block is now and what the other side is.
Rectangle {
    id: control

    required property var tokens
    // True while the block shows its diagram.
    property bool rendered: false
    signal modeRequested(bool rendered)

    implicitWidth: row.implicitWidth + 2 * Metrics.px(2, tokens)
    implicitHeight: Metrics.px(24, tokens)
    radius: Metrics.px(5, tokens)
    color: tokens.panelSubtle
    border.color: tokens.border
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
                implicitWidth: label.implicitWidth + 2 * Metrics.px(8, control.tokens)
                implicitHeight: control.height - 2 * Metrics.px(2, control.tokens)
                checkable: false
                contentItem: Text {
                    id: label
                    text: modelData.label
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: parent.current ? control.tokens.textMain : control.tokens.textSecondary
                    font.family: control.tokens.uiFont
                    font.pixelSize: control.tokens.caption
                    font.weight: parent.current ? Font.DemiBold : Font.Normal
                }
                background: Rectangle {
                    radius: Metrics.px(4, control.tokens)
                    color: parent.current ? control.tokens.codeBlockBackground : parent.hovered ? control.tokens.surfaceHover : "transparent"
                    border.color: parent.visualFocus ? control.tokens.focus : "transparent"
                }
                onClicked: if (!current)
                    control.modeRequested(modelData.rendered)
                Accessible.name: modelData.rendered ? "Show Mermaid diagram" : "Show Mermaid source"
                Accessible.checked: current
            }
        }
    }
}
