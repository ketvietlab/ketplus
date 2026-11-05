import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

Rectangle {
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
                return option.id
        }
        return ""
    }

    function submit(label) {
        if (!pending || answered) return
        const id = optionId(label)
        if (id.length === 0) return
        decisionSubmitted({ decision: { option_ids: [id], custom_answer: "" } })
    }

    implicitHeight: content.implicitHeight + Metrics.px(28, tokens)
    radius: Metrics.px(9, tokens)
    color: tokens.surfaceRaised
    border.width: 1
    border.color: answered ? stateColor : tokens.borderStrong

    required property var tokens

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Metrics.px(14, tokens)
        spacing: Metrics.px(10, tokens)

        // Title and status share one line; the status is pinned to the right
        // edge and the title gives way, whatever the font or window width.
        Item {
            width: parent.width
            height: Metrics.px(22, tokens)
            Rectangle {
                id: approvalIcon
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Metrics.px(22, tokens); height: Metrics.px(22, tokens); radius: Metrics.px(6, tokens)
                color: root.stateColor
                opacity: 0.22
            }
            Text {
                anchors.centerIn: approvalIcon
                text: root.declined ? "✕" : root.answered ? "✓" : "!"
                color: root.stateColor
                font.pixelSize: root.tokens.reading; font.weight: Font.Bold
            }
            Text {
                anchors.left: approvalIcon.right
                anchors.leftMargin: Metrics.px(8, tokens)
                anchors.right: approvalStatus.left
                anchors.rightMargin: Metrics.px(8, tokens)
                anchors.verticalCenter: parent.verticalCenter
                text: root.declined ? "Approval declined" : root.answered ? "Approval granted" : "Approval required"
                color: root.tokens.textMain
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.body; font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                id: approvalStatus
                objectName: "approvalStatus"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.declined ? "Declined" : root.answered ? "Allowed once" : root.pending ? "Agent is waiting" : "Closed"
                color: root.tokens.textMuted
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.micro
            }
        }

        Text {
            width: parent.width
            visible: text.length > 0
            text: root.prompt.length > 0 ? root.prompt : "Allow this agent action?"
            color: root.tokens.textMain
            font.family: root.tokens.uiFont
            font.pixelSize: root.tokens.reading
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }

        Basic.Button {
            id: detailsButton
            visible: root.command.length > 0 && !root.answered
            text: root.detailsVisible ? "Hide command" : "View command"
            padding: 0
            height: Metrics.px(24, tokens)
            onClicked: root.detailsVisible = !root.detailsVisible
            contentItem: Text {
                text: detailsButton.text
                color: root.tokens.textMuted
                font.family: root.tokens.uiFont
                font.pixelSize: root.tokens.caption
                verticalAlignment: Text.AlignVCenter
            }
            background: Item {}
        }

        Text {
            visible: root.detailsVisible && root.command.length > 0
            width: parent.width
            text: root.command
            color: root.tokens.textSecondary
            font.family: "Menlo"
            font.pixelSize: root.tokens.caption
            wrapMode: Text.WrapAnywhere
            maximumLineCount: 12
            elide: Text.ElideRight
        }

        Row {
            visible: root.pending && !root.answered
            width: parent.width
            spacing: Metrics.px(7, tokens)
            Basic.Button {
                objectName: "approvalDeclineButton"
                width: Metrics.px(88, tokens); height: Metrics.px(34, tokens); text: "Decline"
                onClicked: root.submit("Decline")
                contentItem: Text { text: parent.text; color: root.tokens.textSecondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: root.tokens.caption }
                background: Rectangle { radius: Metrics.px(6, tokens); color: parent.hovered ? root.tokens.surfaceHover : root.tokens.interactiveBackground; border.color: root.tokens.border }
            }
            Basic.Button {
                objectName: "approvalAcceptButton"
                width: Metrics.px(96, tokens); height: Metrics.px(34, tokens); text: "Allow once"
                onClicked: root.submit("Accept")
                contentItem: Text { text: parent.text; color: "#ffffff"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: root.tokens.caption; font.weight: Font.DemiBold }
                background: Rectangle { radius: Metrics.px(6, tokens); color: parent.pressed ? root.tokens.actionPrimaryActive : parent.hovered ? root.tokens.actionPrimaryHover : root.tokens.actionPrimary }
            }
        }
    }
}
