import Ket
import QtQuick
import Ket as Kit

// An approval as the mock draws it: while the agent waits, the kit question card with what it
// asks to run in full; once decided or expired, one quiet note of how it ended.
Item {
    id: root
    objectName: "agentApprovalCard"

    required property string prompt
    required property string command
    required property bool pending
    required property var answerData
    required property var questions
    // What it asks to use ("Command", "ketplus_browser.browser_open") and how it ended.
    property string tool: ""
    property string status: ""

    readonly property bool answered: Object.keys(answerData || {}).length > 0
    readonly property string decision: (answerData || {}).decision || ""
    readonly property var decisionQuestion: questions.length > 0 ? questions[0] : ({})
    readonly property bool open: pending && !answered
    // The receipt holds the chosen option's label; another client's decision arrives as status.
    readonly property string outcome: answered ? (decision === "Decline" ? "denied" : "approved") : status
    readonly property string toolLabel: tool.length > 0 ? tool : "Tool"

    signal decisionSubmitted(var answers)

    function optionId(label) {
        for (const option of decisionQuestion.options || []) {
            if (option.label === label)
                return option.id;
        }
        return "";
    }

    function submit(label) {
        if (!open)
            return;
        const id = optionId(label);
        if (id.length === 0)
            return;
        decisionSubmitted({
            decision: {
                option_ids: [id],
                custom_answer: ""
            }
        });
    }

    implicitHeight: open ? card.implicitHeight : note.implicitHeight

    Kit.KetQuestionCard {
        id: card
        objectName: "approvalPendingCard"
        width: parent.width
        visible: root.open
        label: "Approval needed · " + root.toolLabel
        // Without a reason, the command is the question; the code block below shows it.
        question: root.prompt.length > 0 ? root.prompt : "Allow the agent to continue?"
        Kit.KetCodeBlock {
            objectName: "approvalCommand"
            visible: root.command.length > 0
            width: parent.width
            code: root.command
            language: "sh"
        }
        footer: [
            Kit.KetButton {
                objectName: "approvalAcceptButton"
                text: "Allow once"
                variant: Kit.KetTheme.ButtonVariant.Primary
                controlSize: Kit.KetTheme.ControlSize.Small
                onClicked: root.submit("Accept")
            },
            Kit.KetButton {
                objectName: "approvalDeclineButton"
                text: "Deny"
                variant: Kit.KetTheme.ButtonVariant.GhostDanger
                controlSize: Kit.KetTheme.ControlSize.Small
                onClicked: root.submit("Decline")
            }
        ]
    }

    Rectangle {
        id: note
        objectName: "approvalOutcome"
        readonly property string label: root.outcome === "denied" ? "Denied" : root.outcome === "expired" ? "Approval expired" : root.outcome === "approved" ? "Approved" : "Approval closed"
        width: parent.width
        visible: !root.open
        implicitHeight: noteRow.implicitHeight + KetTheme.px(20, KetTheme.zoom)
        radius: KetTheme.px(6, KetTheme.zoom)
        color: KetTheme.t.panelBgSubtle
        border.color: KetTheme.t.panelBorder
        Accessible.role: Accessible.StaticText
        Accessible.name: label + ". " + noteText.message

        Row {
            id: noteRow
            x: KetTheme.px(14, KetTheme.zoom)
            y: KetTheme.px(10, KetTheme.zoom)
            width: parent.width - 2 * x
            spacing: KetTheme.px(8, KetTheme.zoom)
            Kit.KetIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: root.outcome === "denied" ? "close" : root.outcome === "expired" ? "clock" : "shield"
                box: KetTheme.px(14, KetTheme.zoom)
                color: root.outcome === "denied" ? KetTheme.t.danger : root.outcome === "approved" ? KetTheme.t.success : KetTheme.t.textMuted
            }
            Text {
                id: noteText
                objectName: "approvalOutcomeText"
                readonly property string message: root.toolLabel + (root.command.length > 0 ? " · " + root.command : "")
                width: parent.width - KetTheme.px(22, KetTheme.zoom)
                wrapMode: Text.Wrap
                // The command is the agent's text, so it is escaped for StyledText.
                text: "<b>" + note.label + "</b>&nbsp;&nbsp;" + message.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                textFormat: Text.StyledText
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            }
        }
    }
}
