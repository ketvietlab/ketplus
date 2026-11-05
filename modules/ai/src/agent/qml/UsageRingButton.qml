import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

Basic.ToolButton {
    id: root
    objectName: "usageLimitsButton"
    property var tokens: ({})
    property real progress: 0
    property color trackColor: "#3d4248"
    property color fillColor: "#c5c9ce"
    property color hoverColor: "transparent"
    implicitWidth: Metrics.px(20, root.tokens)
    implicitHeight: Metrics.px(20, root.tokens)
    padding: 0
    Accessible.name: "Context and usage limits"

    background: Rectangle {
        radius: Metrics.px(4, root.tokens)
        color: root.hovered || root.down ? root.hoverColor : "transparent"
    }

    contentItem: Item {
        implicitWidth: Metrics.px(12, root.tokens)
        implicitHeight: Metrics.px(12, root.tokens)
        Canvas {
            id: canvas
            anchors.centerIn: parent
            width: Metrics.px(12, root.tokens)
            height: Metrics.px(12, root.tokens)
            antialiasing: true
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const cx = width / 2
                const cy = height / 2
                const line = 1.5
                const r = (Math.min(width, height) - line) / 2
                ctx.lineWidth = line
                ctx.lineCap = "round"
                ctx.strokeStyle = root.trackColor
                ctx.beginPath()
                ctx.arc(cx, cy, r, 0, Math.PI * 2)
                ctx.stroke()
                const amount = Math.max(0, Math.min(1, root.progress))
                if (amount <= 0)
                    return
                ctx.strokeStyle = root.fillColor
                ctx.beginPath()
                const start = -Math.PI / 2
                ctx.arc(cx, cy, r, start, start + Math.PI * 2 * amount)
                ctx.stroke()
            }
        }
    }

    onProgressChanged: canvas.requestPaint()
    onTrackColorChanged: canvas.requestPaint()
    onFillColorChanged: canvas.requestPaint()
    Component.onCompleted: canvas.requestPaint()
}
