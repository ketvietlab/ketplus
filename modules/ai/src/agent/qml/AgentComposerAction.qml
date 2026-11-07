import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket as Kit

Basic.AbstractButton {
    id: action
    property bool stop: false
    readonly property var look: Kit.KetTheme.composerActionRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom, stop, !stop && enabled, hovered, down, visualFocus)
    Layout.alignment: Qt.AlignBottom
    Layout.preferredWidth: look.size
    Layout.preferredHeight: look.size
    implicitWidth: look.size
    implicitHeight: look.size
    hoverEnabled: true
    padding: 0
    Accessible.name: text
    contentItem: Item {
        Kit.KetIcon {
            anchors.centerIn: parent
            visible: !action.stop
            name: Kit.KetTheme.iconForLucide("arrow-up")
            box: action.look.iconSize
            color: action.look.icon
        }
    }
    Rectangle {
        anchors.centerIn: parent
        width: action.look.stopGlyph
        height: width
        radius: action.look.stopRadius
        color: action.look.icon
        visible: action.stop
    }
    background: Rectangle {
        radius: width / 2
        color: action.look.background
        border.width: action.visualFocus ? 2 : 0
        border.color: action.look.focusRing
    }
    Kit.KetToolTip {
        visible: action.hovered
        text: action.text
    }
}
