// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A borderless control in the row below the composer card, as KetPlus desktop draws its
// mode and model buttons: an optional leading icon, the label and a chevron. `warning`
// colours an elevated choice such as Full access; `open` keeps the hover fill while its
// menu or dialog shows.
Basic.AbstractButton {
    id: control

    property string iconName
    property bool warning: false
    property bool open: false
    property bool showLabel: true
    readonly property color tint: warning ? KetTheme.t.warning : KetTheme.t.textSecondary

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: px(28)
    implicitWidth: row.implicitWidth + 2 * px(7)
    hoverEnabled: true
    Accessible.name: text

    KetToolTip {
        visible: control.hovered && !control.open && control.Accessible.description !== ""
        text: control.Accessible.description
    }

    background: Rectangle {
        radius: control.px(6)
        color: control.hovered || control.open || control.down ? KetTheme.t.surfaceHover : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: KetTheme.t.accent
    }

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: control.px(6)

            KetIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.iconName !== ""
                name: control.iconName
                weight: KetTheme.IconWeight.Regular
                box: control.px(14)
                color: control.tint
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.showLabel
                text: control.text
                color: control.tint
                font.family: KetTheme.fontFamily
                font.pixelSize: control.px(13)
            }
            KetIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"
                weight: KetTheme.IconWeight.Regular
                box: control.px(14)
                rotation: control.open ? 180 : 0
                color: control.warning ? KetTheme.t.warning : KetTheme.t.textMuted
            }
        }
    }
}
