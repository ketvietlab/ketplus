// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// #468: what a team sends into a member's conversation, drawn as what it is. The daemon writes
// these messages, not an agent, so each is a card with its kind, its task and the facts the
// member acts on. `block.kind`:
//   assignment  a developer's task: criteria, lane, branch, who assigned it
//   digest      the leader's changes since its last digest, grouped by task, "needs you" marked
//   review      the reviewer's request: task, criteria, the diff in short
//   verdict     the owner's review result: findings and the round
//   decision    the leader's (and the blocked owner's) answer from the user
// The card is one accessible item named by its kind and title; Open T-n shows the task on the
// team page. A client that does not know a kind shows the message body as text.
Rectangle {
    id: card

    required property var block
    required property var window

    readonly property var kinds: ({
        assignment: { label: "Assignment", icon: "branch" },
        digest: { label: "Digest", icon: "info" },
        review: { label: "Review request", icon: "pull-request" },
        verdict: { label: "Review", icon: block.passed ? "check" : "warning" },
        decision: { label: "Decision", icon: "check" }
    })
    readonly property var kind: kinds[block.kind] || kinds.assignment
    readonly property string heading: (block.task ? block.task + " " : "") + (block.title || "")

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight + px(28)
    radius: px(8)
    color: KetTheme.t.panelBg
    border.color: block.kind === "verdict" && !block.passed ? KetTheme.t.warningBorder : KetTheme.t.panelBorder

    Accessible.role: Accessible.Grouping
    Accessible.name: kind.label + ": " + heading
    Accessible.description: (block.facts || []).join(", ")

    component Small: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: card.px(13)
        lineHeight: 1.3
    }
    // A criterion, a finding or a digest line: a mark and its text.
    component Item_: Row {
        property string mark
        property color markColor: KetTheme.t.textMuted
        property string text
        property bool strong: false
        width: parent ? parent.width : 0
        spacing: card.px(8)
        Text {
            width: card.px(14)
            text: parent.mark
            color: parent.markColor
            font.family: KetTheme.fontFamily
            font.pixelSize: card.px(13)
        }
        Text {
            width: parent.width - card.px(22)
            text: parent.text
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: parent.strong ? KetTheme.t.textMain : KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: card.px(13)
            font.weight: parent.strong ? Font.Medium : Font.Normal
        }
    }

    Column {
        id: column
        x: card.px(16)
        y: card.px(14)
        width: parent.width - 2 * x
        spacing: card.px(8)

        // Kind and team, and the link to the task.
        Item {
            width: parent.width
            height: Math.max(eyebrow.implicitHeight, open.implicitHeight)
            Row {
                id: eyebrow
                anchors.verticalCenter: parent.verticalCenter
                spacing: card.px(8)
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: card.kind.icon
                    box: card.px(14)
                    color: card.block.kind === "verdict" && !card.block.passed ? KetTheme.t.warning : KetTheme.t.accentText
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.kind.label.toUpperCase() + "  ·  TEAM " + card.block.team.toUpperCase()
                          + (card.block.round ? "  ·  ROUND " + card.block.round : "")
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: card.px(11)
                    font.weight: Font.DemiBold
                    font.letterSpacing: card.px(0.4)
                }
            }
            KetButton {
                id: open
                objectName: "teamBlockOpenTask"
                visible: !!card.block.task
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "Open " + card.block.task
                controlSize: KetTheme.ControlSize.Small
                variant: KetTheme.ButtonVariant.Ghost
                onClicked: card.window.openTeamTask(card.block.task)
            }
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: card.heading
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: card.px(15)
            font.weight: Font.Medium
        }
        Small { visible: !!card.block.facts; text: (card.block.facts || []).join("  ·  ") }
        Small { visible: !!card.block.text; text: card.block.text || ""; color: KetTheme.t.textMain }

        // Assignment and review request: the acceptance criteria.
        Column {
            visible: !!card.block.criteria
            width: parent.width
            spacing: card.px(4)
            Small { text: "Acceptance criteria"; font.weight: Font.Medium; color: KetTheme.t.textMain }
            Repeater {
                model: card.block.criteria || []
                Item_ {
                    required property var modelData
                    mark: modelData.done ? "✓" : "○"
                    markColor: modelData.done ? KetTheme.t.success : KetTheme.t.textMuted
                    text: modelData.text
                }
            }
        }

        // Review request: the change in short.
        Column {
            visible: !!card.block.diff
            width: parent.width
            spacing: card.px(4)
            Small { text: "The change"; font.weight: Font.Medium; color: KetTheme.t.textMain }
            Repeater {
                model: card.block.diff || []
                Item_ {
                    required property var modelData
                    mark: "·"
                    text: modelData
                }
            }
        }

        // Verdict: the findings, as the reviewer wrote them.
        Column {
            visible: !!card.block.findings
            width: parent.width
            spacing: card.px(4)
            Repeater {
                model: card.block.findings || []
                Item_ {
                    required property var modelData
                    required property int index
                    mark: (index + 1) + "."
                    text: modelData
                }
            }
        }

        // Digest: one group per task; what needs the leader is marked. Ten changes, one card.
        Repeater {
            model: card.block.groups || []
            Column {
                id: group
                required property var modelData
                width: column.width
                spacing: card.px(4)
                topPadding: card.px(4)
                Item {
                    width: parent.width
                    height: groupTitle.implicitHeight
                    // A group's title opens its task, as the card's Open T-n does.
                    HoverHandler { enabled: !!group.modelData.task; cursorShape: Qt.PointingHandCursor }
                    TapHandler { enabled: !!group.modelData.task; onTapped: card.window.openTeamTask(group.modelData.task) }
                    Text {
                        id: groupTitle
                        width: parent.width - groupOpen.width - card.px(8)
                        text: group.modelData.task ? group.modelData.task + " " + group.modelData.title : "Team"
                        elide: Text.ElideRight
                        color: KetTheme.t.textMain
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(13)
                        font.weight: Font.Medium
                    }
                    Text {
                        id: groupOpen
                        visible: !!group.modelData.task
                        anchors.right: parent.right
                        text: group.modelData.state || ""
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(12)
                    }
                }
                Repeater {
                    model: group.modelData.lines
                    Item_ {
                        required property var modelData
                        mark: modelData.needsYou ? "●" : "·"
                        markColor: modelData.needsYou ? KetTheme.t.warning : KetTheme.t.textMuted
                        strong: !!modelData.needsYou
                        text: modelData.text + (modelData.needsYou ? "  — needs you" : "")
                    }
                }
            }
        }

        // Decision: the question and what the user chose.
        Column {
            visible: !!card.block.answer
            width: parent.width
            spacing: card.px(4)
            Small { text: card.block.question || "" }
            Item_ {
                mark: "→"
                markColor: KetTheme.t.accentText
                strong: true
                text: card.block.answer || ""
            }
        }
    }
}
