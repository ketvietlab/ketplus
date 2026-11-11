// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A conversation in the sidebar (concept). One line when there is nothing to say:
// a state mark, the title and the number. A second line only for what needs a look: a turn
// that runs, a question, a failure, or the conversation's tags. A finished conversation
// that was read carries no mark, so the ones that matter stand out.
Basic.AbstractButton {
    id: control

    property string number
    property var tags: []
    property int status: KetTheme.ConversationStatus.NotStarted
    // Finished since the user last opened it.
    property bool unread: false
    // What started the running turn, such as "from #8585".
    property string detail
    readonly property var recipe: KetTheme.conversationRowRecipe(
        KetTheme.dark, KetTheme.zoom, checked, hovered, enabled, visualFocus)
    readonly property var statusRecipe: KetTheme.conversationStatusRecipe(KetTheme.dark, status)
    readonly property bool finished: status === KetTheme.ConversationStatus.Completed
    // States the user acts on or watches; the others are history.
    readonly property bool live: [KetTheme.ConversationStatus.Working,
                                  KetTheme.ConversationStatus.NeedsInput,
                                  KetTheme.ConversationStatus.AwaitingReply,
                                  KetTheme.ConversationStatus.Error].includes(status)
    readonly property bool twoLines: live || tags.length > 0 || detail !== ""
    readonly property string statusLabel: KetTheme.conversationStatusLabel(status)
    // The state and what started the turn come first; tags take the room that is left.
    readonly property int shownTags: Math.min(tags.length, !live ? 2 : detail !== "" ? 0 : 1)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    checkable: true
    hoverEnabled: true
    implicitHeight: px(twoLines ? 46 : 30)
    implicitWidth: px(240)
    padding: 0
    Accessible.name: text
    Accessible.description: (unread ? "Unread, " : "") + statusLabel + (detail ? ", " + detail : "")

    // A working mark turns once per spin period on KetTheme's stepped clock.
    readonly property bool spins: statusRecipe.spinning && visible
    property bool holdsClock: false
    function syncClock() {
        if (spins !== holdsClock) {
            holdsClock = spins
            KetTheme.holdAnimationClock(spins)
        }
    }
    onSpinsChanged: syncClock()
    Component.onCompleted: syncClock()
    Component.onDestruction: if (holdsClock) KetTheme.holdAnimationClock(false)

    contentItem: Item {
        // The state column: every mark sits on the first line, in one place to scan.
        Item {
            id: mark
            x: control.px(8)
            y: control.px(8)
            width: control.px(14)
            height: control.px(14)

            KetIcon {
                anchors.centerIn: parent
                visible: !control.finished
                name: control.statusRecipe.icon
                box: control.px(14)
                rotation: control.holdsClock
                          ? 360 * (KetTheme.animationClock % control.statusRecipe.spinMs)
                            / control.statusRecipe.spinMs
                          : 0
                color: control.statusRecipe.iconColor
            }
            Rectangle {
                anchors.centerIn: parent
                visible: control.finished && control.unread
                width: control.px(8)
                height: width
                radius: width / 2
                color: KetTheme.t.accentText
            }
        }

        Text {
            id: numberText
            anchors.right: parent.right
            anchors.rightMargin: control.px(8)
            y: control.px(8)
            height: control.px(14)
            verticalAlignment: Text.AlignVCenter
            text: control.number
            color: control.recipe.number
            font.family: KetTheme.monoFontFamily
            font.pixelSize: control.px(11)
        }

        Text {
            id: title
            x: mark.x + mark.width + control.px(8)
            y: control.px(6)
            width: numberText.x - control.px(8) - x
            height: control.px(18)
            verticalAlignment: Text.AlignVCenter
            text: control.text
            elide: Text.ElideRight
            color: control.recipe.title
            font.family: KetTheme.fontFamily
            font.pixelSize: control.px(13)
            font.weight: control.unread || control.live ? Font.DemiBold : Font.Normal
        }

        Row {
            visible: control.twoLines
            x: title.x
            y: control.px(25)
            width: parent.width - x - control.px(8)
            height: control.px(16)
            spacing: control.px(6)
            clip: true

            Text {
                visible: control.live || control.detail !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: control.live ? control.statusLabel + (control.detail ? " · " + control.detail : "")
                                   : control.detail
                color: control.live ? control.statusRecipe.labelColor : control.recipe.meta
                font.family: KetTheme.fontFamily
                font.pixelSize: control.px(11)
                font.weight: Font.Medium
            }
            Repeater {
                model: control.tags.slice(0, control.shownTags)
                SidebarTag {
                    required property var modelData
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    tagColor: modelData.color
                }
            }
            SidebarTag {
                visible: control.tags.length > control.shownTags
                anchors.verticalCenter: parent.verticalCenter
                text: "+" + (control.tags.length - control.shownTags)
                overflow: true
            }
        }
    }

    background: Rectangle {
        radius: control.recipe.radius
        color: control.recipe.background
        border.width: control.recipe.focusRing.a > 0 ? 2 : 0
        border.color: control.recipe.focusRing
    }
}
