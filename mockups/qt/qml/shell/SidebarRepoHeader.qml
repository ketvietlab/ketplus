// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A repository's heading in the sidebar (concept). It reads as a heading, not as a
// row: a rule above it, the name in the main colour and semibold, and on the right what
// its conversations are doing, so a folded repository still says that something runs or
// waits for the user. The action replaces the summary while the heading is hovered.
Item {
    id: header

    property string name
    property bool expanded: true
    property bool selected: false
    // Above every repository but the first.
    property bool divider: true
    property int count: 0
    property int working: 0
    property int waiting: 0
    property int unread: 0
    property string actionText: "New conversation"
    // Where the name starts, for the sidebar's alignment check.
    readonly property real textLeft: px(30)
    // The summary in words, for a resting pointer and a screen reader.
    readonly property string summaryText: {
        const parts = []
        if (waiting) parts.push(waiting + " waiting for you")
        if (working) parts.push(working + " working")
        if (unread) parts.push(unread + " unread")
        parts.push(count + (count === 1 ? " conversation" : " conversations"))
        return parts.join(" · ")
    }
    readonly property bool hot: hover.hovered || fold.visualFocus || action.visualFocus
    readonly property real lead: divider ? px(9) : 0
    signal actionTriggered()
    // The user folds or unfolds it; the owner keeps the state.
    signal toggled(bool expanded)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitWidth: px(240)
    implicitHeight: lead + px(30)

    HoverHandler { id: hover }

    Rectangle {
        visible: header.divider
        x: header.px(8)
        y: header.px(4)
        width: parent.width - 2 * x
        height: 1
        color: KetTheme.t.sidebarBorder
    }

    Basic.AbstractButton {
        id: fold
        y: header.lead
        width: parent.width
        height: header.px(30)
        hoverEnabled: true
        Accessible.name: header.name
        Accessible.description: (header.expanded ? "Expanded" : "Collapsed") + ", " + header.summaryText
        KetToolTip {
            visible: fold.hovered && !fold.pressed
            delay: 900
            text: header.summaryText
        }
        onClicked: header.toggled(!header.expanded)

        background: Rectangle {
            radius: header.px(6)
            color: fold.hovered ? KetTheme.t.surfaceHover : "transparent"
            border.width: fold.visualFocus ? 2 : 0
            border.color: KetTheme.t.focusBorder
        }

        contentItem: Item {
            KetIcon {
                id: chevron
                x: header.px(9)
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-down"
                box: header.px(12)
                rotation: header.expanded ? 0 : -90
                color: KetTheme.t.textSecondary
            }
            Text {
                x: header.textLeft
                anchors.verticalCenter: parent.verticalCenter
                width: summary.x - header.px(8) - x
                text: header.name
                elide: Text.ElideMiddle
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: header.px(12)
                font.weight: Font.DemiBold
                font.letterSpacing: header.px(0.2)
            }

            // What the conversations inside are doing; the total last.
            Row {
                id: summary
                anchors.right: parent.right
                anchors.rightMargin: header.px(8)
                anchors.verticalCenter: parent.verticalCenter
                spacing: header.px(8)
                opacity: header.hot ? 0 : 1

                // The marks are the rows' own, so the summary needs no legend.
                component Count: Row {
                    property int status: -1 // a conversation status, or -1 for unread
                    property int value
                    readonly property var mark: status < 0 ? null
                        : KetTheme.conversationStatusRecipe(KetTheme.dark, status)
                    visible: value > 0
                    spacing: header.px(3)
                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: header.px(12)
                        height: width
                        KetIcon {
                            anchors.centerIn: parent
                            visible: !!parent.parent.mark
                            name: parent.parent.mark ? parent.parent.mark.icon : ""
                            box: header.px(12)
                            color: parent.parent.mark ? parent.parent.mark.iconColor : "transparent"
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            visible: !parent.parent.mark
                            width: header.px(7)
                            height: width
                            radius: width / 2
                            color: KetTheme.t.accentText
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: parent.value
                        color: KetTheme.t.textSecondary
                        font.family: KetTheme.monoFontFamily
                        font.pixelSize: header.px(11)
                    }
                }

                Count {
                    anchors.verticalCenter: parent.verticalCenter
                    status: KetTheme.ConversationStatus.NeedsInput
                    value: header.waiting
                }
                Count {
                    anchors.verticalCenter: parent.verticalCenter
                    status: KetTheme.ConversationStatus.Working
                    value: header.working
                }
                Count {
                    anchors.verticalCenter: parent.verticalCenter
                    value: header.unread
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: header.waiting + header.working + header.unread === 0
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
        y: header.lead + (header.px(30) - height) / 2
        visible: header.hot
        controlSize: KetTheme.ControlSize.Small
        iconName: "plus"
        text: header.actionText
        onClicked: header.actionTriggered()
    }
}
