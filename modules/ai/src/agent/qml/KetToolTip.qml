import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ToolTip {
    id: control
    objectName: "ketToolTip"

    required property var tokens
    readonly property color backgroundColor: tokens.accent
    readonly property color labelColor: "#ffffff"
    readonly property real cornerRadius: 5

    opacity: 1
    leftPadding: Metrics.px(8, tokens)
    rightPadding: Metrics.px(8, tokens)
    topPadding: Metrics.px(6, tokens)
    bottomPadding: Metrics.px(6, tokens)
    implicitWidth: Math.min(Metrics.px(360, tokens), contentItem.implicitWidth + leftPadding + rightPadding)

    contentItem: Text {
        text: control.text
        color: control.labelColor
        font.family: control.tokens.uiFont
        font.pixelSize: control.tokens.body
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        color: control.backgroundColor
        radius: control.cornerRadius
    }
}
