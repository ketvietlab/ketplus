import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

// A transcript copy button. After a click the icon turns into a check and the
// label into "Copied" for a moment, so the click visibly did something.
Basic.ToolButton {
    id: control

    // What the button copies, as its tool tip and accessible name.
    property string label: "Copy"
    property bool showToolTip: true
    property color idleColor: "transparent"
    property color hoverColor: KetTheme.t.surfaceHover
    property real cornerRadius: KetTheme.px(4, KetTheme.zoom)
    property bool focusBorder: false
    property int copiedInterval: 1500
    readonly property bool copied: copiedTimer.running

    focusPolicy: Qt.TabFocus
    contentItem: Item {
        implicitWidth: KetTheme.px(16, KetTheme.zoom)
        implicitHeight: implicitWidth
        KetIcon {
            anchors.centerIn: parent
            box: Math.min(parent.width, parent.height)
            name: KetTheme.iconForLucide(control.copied ? "check" : "copy")
            color: control.copied ? KetTheme.t.textMain : KetTheme.t.textMuted
        }
    }
    background: Rectangle {
        color: control.hovered ? control.hoverColor : control.idleColor
        border.color: control.focusBorder && control.visualFocus ? KetTheme.t.colorFocus : "transparent"
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
        objectName: "ketToolTip"
        visible: control.showToolTip && (control.hovered || control.copied)
        text: control.copied ? "Copied" : control.label
    }
}
