// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A repository's label in the sidebar (concept). It is set apart by its form, not by more
// decoration: small capitals with letter spacing, in the secondary colour, where every row
// below is a sentence in the main colour. Space above it separates the repositories; there
// is no rule and no icon. Folded, it shows how many conversations it holds and one dot when
// any of them waits for the user. The action shows while it is hovered or focused.
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
    readonly property real lead: divider ? px(10) : 0
    // Where the name starts, for the sidebar's alignment check.
    readonly property real textLeft: px(30)
    signal actionTriggered()
    // The user folds or unfolds it; the owner keeps the state.
    signal toggled(bool expanded)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitWidth: px(240)
    implicitHeight: lead + px(28)

    HoverHandler { id: hover }

    Basic.AbstractButton {
        id: fold
        y: header.lead
        width: parent.width
        height: header.px(28)
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
                x: header.px(10)
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"
                box: header.px(10)
                rotation: header.expanded ? 0 : -90
                color: KetTheme.t.textMuted
            }
            Text {
                x: header.textLeft
                anchors.verticalCenter: parent.verticalCenter
                width: folded.x - header.px(8) - x
                text: header.name
                elide: Text.ElideMiddle
                color: header.selected ? KetTheme.t.textMain : KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: header.px(11)
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: header.px(0.6)
            }

            // Folded only: what is inside.
            Row {
                id: folded
                anchors.right: parent.right
                anchors.rightMargin: header.px(8)
                anchors.verticalCenter: parent.verticalCenter
                spacing: header.px(6)
                visible: !header.expanded && !header.hot

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: header.waiting > 0
                    width: header.px(6)
                    height: width
                    radius: width / 2
                    color: KetTheme.t.accentText
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: header.count
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.monoFontFamily
                    font.pixelSize: header.px(11)
                }
            }
        }
    }

    KetIconButton {
        id: action
        anchors.right: parent.right
        anchors.rightMargin: header.px(4)
        y: header.lead + (header.px(28) - height) / 2
        visible: header.hot
        controlSize: KetTheme.ControlSize.Small
        iconName: "plus"
        text: header.actionText
        onClicked: header.actionTriggered()
    }
}
