// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket

// The label over a repository's own conversations (concept), shown where the repository also
// has teams: it tells the conversations that belong to no team from the teams above them.
// Its icon stands in the folder's column and its text starts with the repository's name.
Item {
    id: section

    property string text: "Conversations"
    property int count: 0
    property string actionText: "New conversation"
    readonly property real iconLeft: px(24)
    readonly property real textLeft: px(44)
    signal actionTriggered()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitWidth: px(240)
    implicitHeight: px(28)
    Accessible.role: Accessible.Heading
    Accessible.name: text + ", " + count

    KetIcon {
        x: section.iconLeft
        anchors.verticalCenter: parent.verticalCenter
        name: "chats"
        box: section.px(14)
        color: KetTheme.t.textMuted
    }
    Text {
        x: section.textLeft
        anchors.verticalCenter: parent.verticalCenter
        width: action.x - section.px(8) - x
        text: section.text + " (" + section.count + ")"
        elide: Text.ElideRight
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: section.px(12)
        font.weight: Font.Medium
    }
    KetIconButton {
        id: action
        anchors.right: parent.right
        anchors.rightMargin: section.px(4)
        anchors.verticalCenter: parent.verticalCenter
        controlSize: KetTheme.ControlSize.Small
        iconName: "plus"
        text: section.actionText
        onClicked: section.actionTriggered()
    }
}
