import Ket
import QtQuick
import Ket as Kit

// #468: what a team sends into a member's conversation, drawn as what it is (the mock's
// TeamTranscriptBlock): an assignment, the leader's digest, a review request or its verdict,
// an answered decision. `block` is what TeamTranscriptBlocks read from the daemon's message.
// The card is one item for a screen reader, named by its kind and title; Open T-n and a
// digest group's title open the task on the team page.
Rectangle {
    id: card

    required property var block
    readonly property bool findings: block.kind === "verdict"
    readonly property var kinds: ({
            assignment: {
                label: "Assignment",
                icon: "branch"
            },
            digest: {
                label: "Digest",
                icon: "info"
            },
            review: {
                label: "Review request",
                icon: "pull-request"
            },
            verdict: {
                label: "Review",
                icon: "warning"
            },
            decision: {
                label: "Decision",
                icon: "check"
            }
        })
    readonly property var kind: kinds[block.kind] || kinds.assignment
    readonly property string heading: (block.taskId ? block.taskId + " " : "") + (block.title || "")

    signal taskRequested(string taskId)

    function px(value) {
        return KetTheme.px(value, KetTheme.zoom);
    }

    objectName: "transcriptTeamBlock"
    implicitHeight: column.implicitHeight + px(28)
    radius: px(8)
    color: KetTheme.t.panelBg
    border.color: findings ? KetTheme.t.warningBorder : KetTheme.t.panelBorder
    Accessible.role: Accessible.Grouping
    readonly property string accessibleName: block.accessibleName || (kind.label + ": " + heading)
    Accessible.name: accessibleName
    Accessible.description: (block.facts || []).join(", ")

    component Small: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: card.px(13)
        lineHeight: 1.3
        Accessible.ignored: true
    }
    // A criterion, a changed file or a digest line: a mark and its text.
    component Entry: Row {
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
            Accessible.ignored: true
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
            Accessible.ignored: true
        }
    }

    Column {
        id: column
        x: card.px(16)
        y: card.px(14)
        width: parent.width - 2 * x
        spacing: card.px(8)

        Item {
            width: parent.width
            height: Math.max(eyebrow.implicitHeight, openTask.implicitHeight)
            Row {
                id: eyebrow
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - (openTask.visible ? openTask.width + card.px(8) : 0)
                spacing: card.px(8)
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: card.kind.icon
                    box: card.px(14)
                    color: card.findings ? KetTheme.t.warning : KetTheme.t.accentText
                }
                Text {
                    objectName: "transcriptTeamBlockKind"
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - card.px(22)
                    elide: Text.ElideRight
                    text: card.kind.label.toUpperCase() + "  ·  TEAM " + String(card.block.team || "").toUpperCase()
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: card.px(11)
                    font.weight: Font.DemiBold
                    font.letterSpacing: card.px(0.4)
                    Accessible.ignored: true
                }
            }
            Kit.KetButton {
                id: openTask
                objectName: "transcriptTeamBlockOpenTask"
                visible: !!card.block.taskId
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "Open " + (card.block.taskId || "")
                controlSize: Kit.KetTheme.ControlSize.Small
                variant: Kit.KetTheme.ButtonVariant.Ghost
                onClicked: card.taskRequested(card.block.taskId)
            }
        }
        Text {
            objectName: "transcriptTeamBlockTitle"
            width: parent.width
            visible: text.length > 0
            text: card.heading
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: card.px(15)
            font.weight: Font.Medium
            Accessible.ignored: true
        }
        Small {
            visible: (card.block.facts || []).length > 0
            text: (card.block.facts || []).join("  ·  ")
        }
        Small {
            objectName: "transcriptTeamBlockText"
            visible: !!card.block.text
            text: card.block.text || ""
            color: KetTheme.t.textMain
        }

        Column {
            visible: (card.block.criteria || []).length > 0
            width: parent.width
            spacing: card.px(4)
            Small {
                text: "Acceptance criteria"
                color: KetTheme.t.textMain
                font.weight: Font.Medium
            }
            Repeater {
                model: card.block.criteria || []
                Entry {
                    required property var modelData
                    required property int index
                    mark: !modelData.checked ? (index + 1) + "." : modelData.done ? "✓" : "○"
                    markColor: modelData.checked && modelData.done ? KetTheme.t.success : KetTheme.t.textMuted
                    text: modelData.text
                }
            }
        }

        Column {
            visible: (card.block.files || []).length > 0
            width: parent.width
            spacing: card.px(4)
            Small {
                text: "Changed files"
                color: KetTheme.t.textMain
                font.weight: Font.Medium
            }
            Repeater {
                model: card.block.files || []
                Entry {
                    required property var modelData
                    mark: "·"
                    text: modelData
                }
            }
        }

        // The digest: one group per task, what needs the leader marked. Every change, one card.
        Repeater {
            model: card.block.groups || []
            Column {
                id: group
                required property var modelData
                objectName: "transcriptTeamDigestGroup"
                width: column.width
                spacing: card.px(4)
                topPadding: card.px(4)
                Item {
                    width: parent.width
                    height: groupTitle.implicitHeight
                    Text {
                        id: groupTitle
                        width: parent.width - groupState.width - card.px(8)
                        text: (group.modelData.taskId ? group.modelData.taskId + " " : "") + group.modelData.title
                        elide: Text.ElideRight
                        color: groupArea.containsMouse ? KetTheme.t.accentText : KetTheme.t.textMain
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(13)
                        font.weight: Font.Medium
                        Accessible.ignored: true
                    }
                    Text {
                        id: groupState
                        anchors.right: parent.right
                        text: group.modelData.state || ""
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: card.px(12)
                        Accessible.ignored: true
                    }
                    MouseArea {
                        id: groupArea
                        anchors.fill: groupTitle
                        enabled: !!group.modelData.taskId
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.taskRequested(group.modelData.taskId)
                    }
                }
                Repeater {
                    model: group.modelData.lines || []
                    Entry {
                        required property var modelData
                        mark: modelData.needsYou ? "●" : "·"
                        markColor: modelData.needsYou ? KetTheme.t.warning : KetTheme.t.textMuted
                        strong: !!modelData.needsYou
                        text: modelData.text + (modelData.needsYou ? "  — needs you" : "")
                    }
                }
            }
        }

        Column {
            visible: !!card.block.answer
            width: parent.width
            spacing: card.px(4)
            Small {
                text: card.block.question || ""
            }
            Entry {
                objectName: "transcriptTeamDecisionAnswer"
                mark: "→"
                markColor: KetTheme.t.accentText
                strong: true
                text: card.block.answer || ""
            }
        }
    }
}
