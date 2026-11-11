// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket

// A conversation in the sidebar (concept): one line, always. A state mark, the title, the
// number. Nothing else is drawn in the list; the state in words, what started the turn, the
// tags and the worktree are in the hint of a resting pointer and in the accessible
// description. Colour is on the mark alone, and only for a conversation that runs or waits
// for the user; a finished one that was read has a grey mark and recedes.
Basic.AbstractButton {
    id: control

    property string number
    property var tags: []
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
    // History: nothing runs and nothing is asked.
    readonly property bool quiet: [KetTheme.ConversationStatus.Completed,
                                   KetTheme.ConversationStatus.Stopped,
                                   KetTheme.ConversationStatus.NotStarted].includes(status)
    readonly property string statusLabel: (finished && unread ? "Completed, unread"
                                           : KetTheme.conversationStatusLabel(status))
                                          + (detail ? " · " + detail : "")
    // Everything the row does not draw, one fact a line.
    readonly property string hint: [role, statusLabel,
                                    tags.map(t => t.label).join(", "),
                                    worktree].filter(line => line !== "").join("\n")
    // Where the title starts, for the sidebar's alignment check.
    readonly property real textLeft: px(30)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    checkable: true
    hoverEnabled: true
    implicitHeight: px(30)
    implicitWidth: px(240)
    padding: 0
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
            x: control.px(8)
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
                color: control.quiet ? KetTheme.t.textMuted : control.statusRecipe.iconColor
                opacity: control.quiet ? 0.7 : 1
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
            text: control.number
            color: control.recipe.number
            font.family: KetTheme.monoFontFamily
            font.pixelSize: control.px(11)
        }

        Text {
            x: control.textLeft
            anchors.verticalCenter: parent.verticalCenter
            width: numberText.x - control.px(10) - x
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
