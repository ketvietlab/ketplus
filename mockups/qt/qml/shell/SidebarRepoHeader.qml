// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A repository in the sidebar tree (concept): the root of its teams and conversations. A
// chevron that folds it, a folder, the name in small capitals, and how many conversations it
// holds. Folded, one dot says that something inside waits for the user. The action shows
// while it is hovered or focused, left of the count, which stays.
Item {
    id: header

    property string name
    property bool expanded: true
    property bool selected: false
    // Space above every repository but the first.
    property bool divider: true
    property int count: 0
    // Conversations that wait for the user: a question, a failure, or finished and unread.
    property int waiting: 0
    property string actionText: "New conversation"
    readonly property bool hot: hover.hovered || fold.visualFocus || action.visualFocus
    readonly property real lead: divider ? px(6) : 0
    // The tree's columns: the chevron, the folder, then the name. The guide line of what the
    // repository holds runs under the chevron's centre.
    readonly property real guideLeft: px(13)
    readonly property real iconLeft: px(24)
    readonly property real textLeft: px(44)
    signal actionTriggered()
    // The user folds or unfolds it; the owner keeps the state.
    signal toggled(bool expanded)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitWidth: px(240)
    implicitHeight: lead + px(30)

    HoverHandler { id: hover }

    Basic.AbstractButton {
        id: fold
        y: header.lead
        width: parent.width
        height: header.px(30)
        hoverEnabled: true
        Accessible.name: header.name
        Accessible.description: (header.expanded ? "Expanded" : "Collapsed") + ", " + header.count
                                + (header.count === 1 ? " conversation" : " conversations")
                                + (header.waiting ? ", " + header.waiting + " waiting for you" : "")
        onClicked: header.toggled(!header.expanded)

        background: Rectangle {
            radius: header.px(6)
            color: fold.hovered ? KetTheme.t.surfaceHover : "transparent"
            border.width: fold.visualFocus ? 2 : 0
            border.color: KetTheme.t.focusBorder
        }

        contentItem: Item {
            KetIcon {
                x: header.px(8)
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"
                box: header.px(10)
                rotation: header.expanded ? 0 : -90
                color: KetTheme.t.textMuted
            }
            KetIcon {
                x: header.iconLeft
                anchors.verticalCenter: parent.verticalCenter
                name: header.expanded ? "folder-open" : "folder"
                box: header.px(14)
                color: KetTheme.t.textSecondary
            }
            Text {
                x: header.textLeft
                anchors.verticalCenter: parent.verticalCenter
                width: (header.hot ? trailing.x - header.px(26) : trailing.x) - header.px(8) - x
                text: header.name
                elide: Text.ElideMiddle
                // A label over the tree, quieter than the titles it holds.
                color: header.selected ? KetTheme.t.textMain : KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: header.px(12)
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: header.px(0.4)
            }

            Row {
                id: trailing
                anchors.right: parent.right
                anchors.rightMargin: header.px(6)
                anchors.verticalCenter: parent.verticalCenter
                spacing: header.px(6)

                // Folded only: something inside waits for the user.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: header.waiting > 0 && !header.expanded
                    width: header.px(6)
                    height: width
                    radius: width / 2
                    color: KetTheme.t.accentText
                }
                KetBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    text: header.count
                }
            }
        }
    }

    KetIconButton {
        id: action
        x: trailing.x - width - header.px(2)
        y: header.lead + (header.px(30) - height) / 2
        visible: header.hot
        controlSize: KetTheme.ControlSize.Small
        iconName: "plus"
        text: header.actionText
        onClicked: header.actionTriggered()
    }
}
