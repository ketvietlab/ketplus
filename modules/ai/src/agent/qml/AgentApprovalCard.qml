import Ket
import QtQuick
import Ket 1.0 as Kit
import QtQuick.Controls.Basic as Basic

Kit.KetQuestionCard {
    id: root
    objectName: "agentApprovalCard"

    required property string prompt
    required property string command
    required property bool pending
    required property var answerData
    required property var questions

    property bool detailsVisible: false
    readonly property bool answered: Object.keys(answerData || {}).length > 0
    readonly property string decision: answerData.decision || ""
    readonly property var decisionQuestion: questions.length > 0 ? questions[0] : ({})
    // The receipt holds the chosen option's label.
    readonly property bool declined: answered && decision === "Decline"
    readonly property color stateColor: declined ? tokens.danger : answered ? tokens.positive : tokens.warning

    signal decisionSubmitted(var answers)

    function optionId(label) {
        for (const option of decisionQuestion.options || []) {
            if (option.label === label)
                return option.id;
        }
        return "";
    }

    function submit(label) {
        if (!pending || answered)
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

    required property var tokens
    label: declined ? "Approval declined" : answered ? "Approval granted" : "Approval required"
    question: prompt
    Column {
        width: parent.width
        spacing: root.recipe.controlGap
        Text {
            objectName: "approvalStatus"
            width: parent.width
            horizontalAlignment: Text.AlignRight
            text: root.declined ? "Declined" : root.answered ? "Allowed once" : root.pending ? "Agent is waiting" : "Closed"
            color: root.recipe.step
            font.family: root.tokens.uiFont
            font.pixelSize: root.recipe.labelPixels
        }
        Kit.KetButton {
            visible: root.command.length > 0
            text: root.detailsVisible ? "Hide command" : "View command"
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: root.detailsVisible = !root.detailsVisible
        }
        TextEdit {
            visible: root.detailsVisible && root.command.length > 0
            width: parent.width
            text: root.command
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            color: root.tokens.textSecondary
            font.family: root.tokens.monoFont
            font.pixelSize: root.tokens.body
        }
    }
    footer: [
        Kit.KetButton {
            objectName: "approvalDeclineButton"
            visible: root.pending && !root.answered
            text: "Decline"
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: root.submit("Decline")
        },
        Kit.KetButton {
            objectName: "approvalAcceptButton"
            visible: root.pending && !root.answered
            text: "Allow once"
            variant: Kit.KetTheme.ButtonVariant.Primary
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: root.submit("Accept")
        }
    ]
}
