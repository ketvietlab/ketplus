// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A conversation in the sidebar tree (concept): one line, always. A state mark, the title,
// the number. Nothing else is drawn in the list; the state in words, what started the turn
// and the worktree are in the hint of a resting pointer and in the accessible description.
// Conversations carry no user tags (ADR 0117). The mark carries the state's own icon and colour; a finished conversation
// that was not read yet has a dot and a semibold title.
// `indent` is the row's level: its highlight starts there, right of the tree's guide line.
Basic.AbstractButton {
    id: control

    property string scheduleHint
    property string number
    property int status: KetTheme.ConversationStatus.NotStarted
    // Finished since the user last opened it.
    property bool unread: false
    // What started the running turn, such as "from #8585".
    property string detail
    // A team member's role, and where the conversation works.
    property string role
    property string worktree
    readonly property var recipe: KetTheme.conversationRowRecipe(
        KetTheme.dark, KetTheme.zoom, checked, hovered, enabled, visualFocus)
    readonly property var statusRecipe: KetTheme.conversationStatusRecipe(KetTheme.dark, status)
    readonly property bool finished: status === KetTheme.ConversationStatus.Completed
    readonly property string statusLabel: (finished && unread ? "Completed, unread"
                                           : KetTheme.conversationStatusLabel(status))
                                          + (detail ? " · " + detail : "")
    // Everything the row does not draw, one fact a line.
    readonly property string hint: [role, statusLabel, worktree, scheduleHint]
                                   .filter(line => line !== "").join("\n")
    // The level: a repository's own conversations stand at 20, a team's members at 38.
    property real indent: px(20)
    // Where the mark and the title start; the sidebar checks them against the level's heading.
    readonly property real markLeft: indent + px(4)
    readonly property real textLeft: indent + px(24)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    checkable: true
    hoverEnabled: true
    implicitHeight: px(30)
    implicitWidth: px(240)
    padding: 0
    leftInset: indent
    Accessible.name: text
    Accessible.description: hint.replace(/\n/g, ", ")

    // The hint waits for a resting pointer, and a press hides it until the pointer leaves
    // the row: clicking never raises it.
    property bool tipDismissed: false
    onHoveredChanged: if (!hovered) tipDismissed = false
    onPressedChanged: if (pressed) tipDismissed = true
    KetToolTip {
        visible: control.hovered && !control.pressed && !control.tipDismissed
        delay: 900
        text: control.hint
    }

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
        // One column of marks down the list.
        Item {
            id: mark
            x: control.markLeft
            anchors.verticalCenter: parent.verticalCenter
            width: control.px(14)
            height: width

            KetIcon {
                anchors.centerIn: parent
                visible: !(control.finished && control.unread)
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
            anchors.verticalCenter: parent.verticalCenter
            text: "#" + control.number
            color: control.recipe.number
            font.family: KetTheme.monoFontFamily
            font.pixelSize: control.px(11)
        }

        KetIcon {
            visible: control.scheduleHint !== ""
            anchors.verticalCenter: parent.verticalCenter
            x: numberText.x - control.px(20)
            name: "clock"; box: control.px(12); color: KetTheme.t.textMuted
        }
        Text {
            x: control.textLeft
            anchors.verticalCenter: parent.verticalCenter
            width: numberText.x - control.px(control.scheduleHint ? 28 : 10) - x
            text: control.text
            elide: Text.ElideRight
            color: control.recipe.title
            font.family: KetTheme.fontFamily
            font.pixelSize: control.px(13)
            // The one emphasis in the list: finished and not read yet.
            font.weight: control.unread ? Font.DemiBold : Font.Normal
        }
    }

    background: Rectangle {
        radius: control.recipe.radius
        color: control.recipe.background
        border.width: control.recipe.focusRing.a > 0 ? 2 : 0
        border.color: control.recipe.focusRing
    }
}
