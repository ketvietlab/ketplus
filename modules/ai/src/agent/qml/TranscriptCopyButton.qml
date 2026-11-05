import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

// A transcript copy button. After a click the icon turns into a check and the
// label into "Copied" for a moment, so the click visibly did something.
Basic.ToolButton {
    id: control

    required property var tokens
    // What the button copies, as its tool tip and accessible name.
    property string label: "Copy"
    property bool showToolTip: true
    property color idleColor: "transparent"
    property color hoverColor: tokens.surfaceHover
    property real cornerRadius: Metrics.px(4, tokens)
    property bool focusBorder: false
    property int copiedInterval: 1500
    readonly property bool copied: copiedTimer.running

    focusPolicy: Qt.TabFocus
    contentItem: TintedIcon {
        source: control.copied ? "qrc:/agent/icons/check.svg" : "qrc:/agent/icons/copy.svg"
        color: control.copied ? control.tokens.textMain : control.tokens.textMuted
    }
    background: Rectangle {
        color: control.hovered ? control.hoverColor : control.idleColor
        border.color: control.focusBorder && control.visualFocus ? control.tokens.focus : "transparent"
        radius: control.cornerRadius
    }
    // Runs alongside the instance's own onClicked, which does the copying.
    onClicked: copiedTimer.restart()
    Accessible.name: copied ? "Copied" : label

    Timer {
        id: copiedTimer
        interval: control.copiedInterval
    }
    KetToolTip {
        tokens: control.tokens
        visible: control.showToolTip && (control.hovered || control.copied)
        text: control.copied ? "Copied" : control.label
    }
}
