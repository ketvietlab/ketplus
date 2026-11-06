// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// One block of a conversation, in the order the journal replays it. `host.type`:
//   day, user, steer, queued, commentary, answer, activity, changes, question, approval,
//   agent, event, reconnect, error, working, turn, trimmed
// See TranscriptFixtures for the fields each type reads and the journal events behind it.
Loader {
    id: host

    required property var block
    required property var window
    required property string provider

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    width: parent ? parent.width : 0
    sourceComponent: ({
        day: day, user: user, steer: user, queued: queued, commentary: commentary,
        answer: answer, activity: activity, changes: changes, question: question,
        approval: approval, agent: agent, event: event, reconnect: reconnect, error: error,
        working: working, turn: turn, trimmed: note
    })[host.block.type] || null

    // A quiet line of the transcript: an icon, a bold label and the rest.
    component Line: Row {
        property string iconName
        property color iconColor: KetTheme.t.textMuted
        property string label
        property string message
        width: parent ? parent.width : 0
        spacing: host.px(8)
        KetIcon { name: iconName; box: host.px(14); color: iconColor; anchors.verticalCenter: parent.verticalCenter }
        Text {
            width: parent.width - host.px(22)
            wrapMode: Text.Wrap
            text: (label ? "<b>" + label + "</b>&nbsp;&nbsp;" : "") + message
            textFormat: Text.StyledText
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: host.px(13)
        }
    }

    // A bordered note in the flow: agent messages, resolved questions, trimmed detail.
    component Note: Rectangle {
        property alias iconName: noteLine.iconName
        property alias iconColor: noteLine.iconColor
        property alias label: noteLine.label
        property alias message: noteLine.message
        default property alias extra: noteExtra.data
        width: parent ? parent.width : 0
        implicitHeight: noteColumn.implicitHeight + host.px(20)
        radius: host.px(6)
        color: KetTheme.t.panelBgSubtle
        border.color: KetTheme.t.panelBorder
        Column {
            id: noteColumn
            x: host.px(14)
            y: host.px(10)
            width: parent.width - 2 * x
            spacing: host.px(8)
            Line { id: noteLine; iconColor: KetTheme.t.accentText }
            Row { id: noteExtra; x: host.px(22); visible: children.length > 0; spacing: host.px(8) }
        }
    }

    Component { id: day; KetDayDivider { text: host.block.text } }

    // user.message (text, images, a message from another conversation) and turn.steered.
    Component {
        id: user
        Column {
            spacing: host.px(6)
            Text {
                visible: !!host.block.from
                anchors.right: parent.right
                text: host.block.from ? "From @agent/" + host.block.from.number + " · " + host.block.from.title
                                         + " · " + host.block.from.provider : ""
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: host.px(12)
            }
            Row {
                visible: !!(host.block.images && host.block.images.length)
                anchors.right: parent.right
                spacing: host.px(8)
                Repeater {
                    model: host.block.images || []
                    Rectangle {
                        required property var modelData
                        required property int index
                        width: host.px(120)
                        height: host.px(78)
                        radius: host.px(8)
                        clip: true
                        color: KetTheme.t.panelBgSubtle
                        border.color: KetTheme.t.panelBorder
                        Image { anchors.fill: parent; anchors.margins: 1; source: modelData.source; fillMode: Image.PreserveAspectCrop }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: host.window.openImages(host.block.images, index) }
                    }
                }
            }
            KetUserMessage { width: parent.width; text: host.block.text }
            Text {
                visible: host.block.type === "steer"
                anchors.right: parent.right
                text: "Sent while " + host.provider + " was working · joined the running turn"
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: host.px(12)
            }
        }
    }

    // turn.queued: a message waiting for the running turn to end.
    Component {
        id: queued
        Note {
            iconName: "clock"
            label: "Queued"
            message: host.block.text + "  ·  sends when this turn ends"
            KetButton { text: "Send now"; controlSize: KetTheme.ControlSize.Small; onClicked: host.window.notify("Steered the running turn") }
            KetButton { text: "Remove"; controlSize: KetTheme.ControlSize.Small; variant: KetTheme.ButtonVariant.Ghost; onClicked: host.window.notify("Removed the queued message") }
        }
    }

    // agentMessage with phase "commentary": what the agent says between steps.
    Component {
        id: commentary
        MarkdownView {
            window: host.window
            markdown: host.block.text
            onLinkActivated: link => host.window.notify("Opened " + link)
        }
    }

    // agentMessage with phase "final_answer", then the copy action.
    Component {
        id: answer
        Column {
            spacing: host.px(10)
            MarkdownView {
                width: parent.width
                window: host.window
                markdown: host.block.markdown
                onLinkActivated: link => host.window.notify("Opened " + link)
            }
            Text {
                visible: host.block.status === "interrupted"
                text: "Stopped before the answer finished"
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: host.px(12)
            }
            KetIconButton { iconName: "copy"; text: "Copy answer"; onClicked: host.window.notify("Copied the answer") }
        }
    }

    // A run of item.started/item.completed steps, grouped by the daemon's activity index.
    Component {
        id: activity
        KetActivityGroup {
            text: host.block.summary
            expanded: host.block.expanded !== false
            Repeater {
                model: host.block.steps
                TranscriptActivityItem {
                    required property var modelData
                    step: modelData
                    window: host.window
                }
            }
        }
    }

    Component {
        id: changes
        KetChangedFiles {
            files: host.block.files
            note: host.block.note || "Not committed"
            onFileActivated: host.window.notify("Opened the diff")
            footer: KetButton { text: "Review changes"; controlSize: KetTheme.ControlSize.Small; onClicked: host.window.notify("Opened Changes") }
        }
    }

    // approval.requested kind "input" (AskUserQuestion and Codex questions); answered,
    // deferred and expired questions fold into a note.
    Component {
        id: question
        Loader {
            sourceComponent: (host.block.state || "pending") === "pending" ? pendingQuestion : closedQuestion
            Component {
                id: pendingQuestion
                KetQuestionCard {
                    width: host.width
                    label: host.block.header ? "Your input is needed · " + host.block.header : "Your input is needed"
                    step: host.block.step || ""
                    question: host.block.question
                    Repeater {
                        model: host.block.options
                        KetChoice {
                            required property var modelData
                            required property int index
                            width: parent.width
                            kind: host.block.multi ? KetTheme.ToggleKind.Checkbox : KetTheme.ToggleKind.Radio
                            checked: index === 0
                            text: modelData.label
                            description: modelData.description || ""
                        }
                    }
                    KetTextField {
                        visible: !!host.block.allowCustom
                        width: parent.width
                        placeholderText: "Other: type your own answer"
                    }
                    footer: [
                        KetButton { text: "Answer"; variant: KetTheme.ButtonVariant.Primary; onClicked: host.window.notify("Answered") },
                        KetButton { text: "Later"; variant: KetTheme.ButtonVariant.Ghost; onClicked: host.window.notify("Deferred the question") }
                    ]
                }
            }
            Component {
                id: closedQuestion
                Note {
                    width: host.width
                    iconName: host.block.state === "answered" ? "check" : "clock"
                    iconColor: host.block.state === "answered" ? KetTheme.t.success : KetTheme.t.textMuted
                    label: host.block.state === "answered" ? "Answered" : host.block.state === "deferred" ? "Deferred" : "Question expired"
                    message: host.block.question + (host.block.answer ? "  ·  " + host.block.answer : "")
                }
            }
        }
    }

    // approval.requested kinds command, mcp_tool_call, claude_tool and cursor_tool.
    Component {
        id: approval
        Loader {
            sourceComponent: (host.block.state || "pending") === "pending" ? pendingApproval : closedApproval
            Component {
                id: pendingApproval
                KetQuestionCard {
                    width: host.width
                    label: "Approval needed · " + host.block.tool
                    question: host.block.reason
                    KetCodeBlock { width: parent.width; code: host.block.command; language: "sh" }
                    footer: [
                        KetButton { text: "Allow once"; variant: KetTheme.ButtonVariant.Primary; onClicked: host.window.notify("Allowed once") },
                        KetButton { text: "Allow in this conversation"; onClicked: host.window.notify("Allowed for this conversation") },
                        KetButton { text: "Deny"; variant: KetTheme.ButtonVariant.GhostDanger; onClicked: host.window.notify("Denied") }
                    ]
                }
            }
            Component {
                id: closedApproval
                Note {
                    width: host.width
                    iconName: host.block.state === "denied" ? "close" : host.block.state === "expired" ? "clock" : "shield"
                    iconColor: host.block.state === "denied" ? KetTheme.t.danger : KetTheme.t.success
                    label: host.block.state === "denied" ? "Denied" : host.block.state === "expired" ? "Approval expired" : "Approved"
                    message: host.block.tool + " · " + host.block.command
                }
            }
        }
    }

    // agent_message.state of an outgoing request, reply or notice.
    Component {
        id: agent
        Note {
            readonly property var states: ({
                sent: "Sent", replied: "Replied", failed: "Not delivered", expired: "Expired",
                held: "Held until the turn ends"
            })
            iconName: "send"
            iconColor: host.block.state === "failed" ? KetTheme.t.danger : KetTheme.t.accentText
            label: (host.block.kind === "reply" ? "Reply to" : host.block.kind === "notice" ? "Notice to" : "Message to")
                   + " @agent/" + host.block.peer.number
            message: host.block.peer.title + " · " + host.block.peer.provider + " · " + states[host.block.state]
                     + (host.block.expectReply && host.block.state === "sent" ? " · reply requested" : "")
                     + (host.block.text ? "<br>" + host.block.text : "")
        }
    }

    // Session events between turns: compaction, provider and model changes, forks, a new
    // worktree, a rename.
    Component {
        id: event
        Column {
            spacing: host.px(4)
            KetDayDivider { width: parent.width; text: host.block.text }
            Text {
                visible: !!host.block.detail
                anchors.horizontalCenter: parent.horizontalCenter
                text: host.block.detail || ""
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: host.px(12)
            }
        }
    }

    // runtime.error with will_retry: the provider stream reconnecting.
    Component {
        id: reconnect
        Row {
            spacing: host.px(8)
            KetActivityIndicator { anchors.verticalCenter: parent.verticalCenter; color: KetTheme.t.warning }
            Text {
                text: host.block.text
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: host.px(13)
            }
        }
    }

    // runtime.error that ends the turn.
    Component {
        id: error
        KetBanner {
            tone: KetTheme.BannerTone.Danger
            title: host.block.title
            message: host.block.message
            actionText: host.block.action || "Retry"
            onActionTriggered: host.window.notify(host.block.action || "Retrying")
        }
    }

    Component {
        id: working
        Row {
            spacing: host.px(8)
            KetActivityIndicator { anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: "Working · " + host.block.text
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: host.px(13)
            }
        }
    }

    // turn.completed: how the turn ended, its length, context and (Claude) cost.
    Component {
        id: turn
        Line {
            readonly property string status: host.block.status || "completed"
            iconName: status === "failed" ? "error" : status === "interrupted" ? "stop-circle" : "success"
            iconColor: status === "failed" ? KetTheme.t.danger : status === "interrupted" ? KetTheme.t.textMuted : KetTheme.t.success
            label: status === "failed" ? "Failed" : status === "interrupted" ? "Stopped" : "Completed"
            message: host.block.text
        }
    }

    // Detail the journal compacted (ketvietlab/ketplus#128).
    Component {
        id: note
        Note {
            iconName: "archive"
            iconColor: KetTheme.t.textMuted
            label: "Detail trimmed"
            message: host.block.text
            KetButton { text: "Storage settings"; controlSize: KetTheme.ControlSize.Small; variant: KetTheme.ButtonVariant.Ghost; onClicked: host.window.showScreen("settings/storage") }
        }
    }
}
