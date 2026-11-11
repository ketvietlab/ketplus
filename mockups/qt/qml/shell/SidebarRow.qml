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
    // What waits for the user is semibold; a turn that runs has its spinner and is not.
    readonly property bool calls: unread || [KetTheme.ConversationStatus.NeedsInput,
                                             KetTheme.ConversationStatus.AwaitingReply,
                                             KetTheme.ConversationStatus.Error].includes(status)
    readonly property bool twoLines: live || tags.length > 0 || detail !== ""
    readonly property string statusLabel: KetTheme.conversationStatusLabel(status)
    readonly property string lead: live ? statusLabel + (detail ? " · " + detail : "") : detail
    // Where the title and the second line start, for the sidebar's alignment check.
    readonly property real textLeft: px(30)
    readonly property real secondRight: textLeft + second.width
    // Whole tags only: the state and what started the turn come first, then as many tags
    // as fit, keeping room for +N while any is left out. A tag is never cut.
    readonly property int shownTags: {
        const gap = px(6)
        let room = width - textLeft - px(8)
        if (lead !== "")
            room -= leadMetrics.advanceWidth(lead) + gap
        const chip = text => chipProbe.widthOf(tagMetrics.advanceWidth(text))
        const more = count => count ? gap + tagMetrics.advanceWidth("+" + count) : 0
        let x = 0
        for (let i = 0; i < tags.length; ++i) {
            if (x + chip(tags[i].label) + more(tags.length - i - 1) > room)
                return i
            x += chip(tags[i].label) + gap
        }
        return tags.length
    }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    checkable: true
    hoverEnabled: true
    implicitHeight: px(twoLines ? 46 : 30)
    implicitWidth: px(240)
    padding: 0
    Accessible.name: text
    Accessible.description: (unread ? "Unread, " : "") + statusLabel + (detail ? ", " + detail : "")
                            + (tags.length ? ", tags " + tags.map(t => t.label).join(", ") : "")

    FontMetrics {
        id: tagMetrics
        font.family: KetTheme.fontFamily
        font.pixelSize: control.px(11)
        font.weight: Font.Medium
    }
    FontMetrics {
        id: leadMetrics
        font: leadText.font
    }
    SidebarTag { id: chipProbe; visible: false }

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
            x: control.textLeft
            y: control.px(6)
            width: numberText.x - control.px(8) - x
            height: control.px(18)
            verticalAlignment: Text.AlignVCenter
            text: control.text
            elide: Text.ElideRight
            color: control.recipe.title
            font.family: KetTheme.fontFamily
            font.pixelSize: control.px(13)
            font.weight: control.calls ? Font.DemiBold : Font.Normal
        }

        Row {
            id: second
            objectName: "second"
            visible: control.twoLines
            x: control.textLeft
            y: control.px(25)
            height: control.px(16)
            spacing: control.px(6)

            Text {
                id: leadText
                visible: control.lead !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: control.lead
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
                }
            }
            // Not a chip: it counts tags, it is not one.
            Text {
                visible: control.tags.length > control.shownTags
                anchors.verticalCenter: parent.verticalCenter
                text: "+" + (control.tags.length - control.shownTags)
                color: control.recipe.meta
                font.family: KetTheme.fontFamily
                font.pixelSize: control.px(11)
                font.weight: Font.Medium
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
