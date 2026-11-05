import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic

Rectangle {
    id: root
    objectName: "agentQuestionCard"

    required property var questions
    required property var answerData
    required property bool pending
    required property string messageKey
    required property var tokens

    property int currentIndex: 0
    property var draftAnswers: ({})
    property var otherQuestions: ({})
    property var initialDraftAnswers: ({})
    property var initialOtherQuestions: ({})
    property int initialQuestionIndex: 0
    property bool submitted: false
    onMessageKeyChanged: {
        submitted = false
        Qt.callLater(restoreInitialDraft)
    }
    readonly property bool answered: Object.keys(answerData || {}).length > 0
    readonly property var currentQuestion: questions.length > 0
                                                   ? questions[Math.min(currentIndex,
                                                                        questions.length - 1)]
                                                   : ({})
    readonly property bool confirmation: {
        const opts = currentQuestion.options || []
        if (questions.length !== 1 || currentQuestion.selection === "multiple" || currentQuestion.isSecret || opts.length !== 2) return false
        const labels = opts.map(o => o.label.trim().toLowerCase()).sort().join("|")
        return labels === "no|yes" || labels === "có|không"
    }
    readonly property string currentQuestionId: currentQuestion.id || ""
    readonly property var currentAnswer: draftAnswers[currentQuestionId] || ({})
    readonly property var currentAnswers: currentAnswer.option_ids || []
    readonly property string currentAnswerText: currentAnswer.custom_answer || ""
    readonly property bool currentUsesOther: otherQuestions[currentQuestionId] === true
    // A closed or already sent question is shown, not edited.
    readonly property bool editable: pending && !submitted && !answered
    readonly property bool canContinue: pending && !submitted
                                        && (currentAnswers.length > 0
                                            || currentAnswerText.trim().length > 0)

    signal answersSubmitted(var answers)
    signal draftChanged(var answers, int questionIndex, var otherAnswers)
    signal draftDiscarded()

    function restoreInitialDraft() {
        submitted = false
        draftAnswers = Object.assign({}, initialDraftAnswers || {})
        otherQuestions = Object.assign({}, initialOtherQuestions || {})
        currentIndex = Math.max(0, Math.min(initialQuestionIndex, questions.length - 1))
    }
    Component.onCompleted: restoreInitialDraft()
    onPendingChanged: {
        if (!pending && !answered) draftDiscarded()
    }
    onAnsweredChanged: {
        if (answered) draftDiscarded()
    }

    function setDraft(questionId, answer) {
        const updated = Object.assign({}, draftAnswers)
        updated[questionId] = answer
        draftAnswers = updated
        draftChanged(draftAnswers, currentIndex, otherQuestions)
    }

    function setOther(questionId, enabled) {
        const updated = Object.assign({}, otherQuestions)
        updated[questionId] = enabled
        otherQuestions = updated
        draftChanged(draftAnswers, currentIndex, otherQuestions)
    }

    function selectOption(optionId) {
        if (!editable) return
        setOther(currentQuestionId, false)
        if (currentQuestion.selection === "multiple") {
            const selected = currentAnswers.slice()
            const index = selected.indexOf(optionId)
            if (index >= 0)
                selected.splice(index, 1)
            else
                selected.push(optionId)
            setDraft(currentQuestionId, { option_ids: selected, custom_answer: "" })
        } else {
            setDraft(currentQuestionId, { option_ids: [optionId], custom_answer: "" })
        }
    }

    function selectOther() {
        if (!editable) return
        setOther(currentQuestionId, true)
        setDraft(currentQuestionId, { option_ids: [], custom_answer: "" })
        Qt.callLater(function() { otherInput.forceActiveFocus() })
    }

    function continueQuestion() {
        if (!canContinue) return
        if (currentIndex + 1 < questions.length) {
            currentIndex += 1
            draftChanged(draftAnswers, currentIndex, otherQuestions)
            return
        }
        submitted = true
        const completedAnswers = Object.assign({}, draftAnswers)
        answersSubmitted(completedAnswers)
        draftAnswers = ({})
    }

    implicitHeight: cardColumn.implicitHeight
    radius: Metrics.px(9, tokens)
    color: tokens.surfaceRaised
    border.color: answered ? tokens.positive : tokens.borderStrong
    border.width: 1
    clip: true

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Metrics.px(2, tokens)
        color: root.answered ? root.tokens.positive : root.tokens.accent
    }

    Column {
        id: cardColumn
        width: parent.width

        Rectangle {
            width: parent.width
            height: Metrics.px(44, tokens)
            color: root.tokens.interactiveBackground

            // Title and status share one line; the status is pinned to the
            // right edge whatever the font or window width.
            Item {
                anchors.left: parent.left
                anchors.leftMargin: Metrics.px(14, tokens)
                anchors.right: parent.right
                anchors.rightMargin: Metrics.px(14, tokens)
                anchors.verticalCenter: parent.verticalCenter
                height: Metrics.px(22, tokens)

                Rectangle {
                    id: questionIcon
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: Metrics.px(22, tokens)
                    height: Metrics.px(22, tokens)
                    radius: Metrics.px(6, tokens)
                    color: root.tokens.accentMuted
                    Text {
                        anchors.centerIn: parent
                        text: "?"
                        color: root.tokens.accentHover
                        font.family: root.tokens.uiFont
                        font.pixelSize: root.tokens.body
                        font.weight: Font.Bold
                    }
                }
                Text {
                    anchors.left: questionIcon.right
                    anchors.leftMargin: Metrics.px(8, tokens)
                    anchors.right: questionStatus.left
                    anchors.rightMargin: Metrics.px(8, tokens)
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Question"
                    color: root.tokens.textMain
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.body
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    id: questionStatus
                    objectName: "questionStatus"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.answered ? "Answered" : root.submitted ? "Answer sent"
                                        : !root.pending ? "Closed"
                                        : root.questions.length === 1 ? ""
                                        : "Question " + (root.currentIndex + 1) + "/" + root.questions.length
                    color: root.tokens.textMuted
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.micro
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: root.tokens.border
            }
        }

        Item {
            width: parent.width
            height: root.answered ? receiptColumn.implicitHeight + Metrics.px(28, tokens)
                                  : questionColumn.implicitHeight + Metrics.px(28, tokens)

            Column {
                id: questionColumn
                visible: !root.answered
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Metrics.px(14, tokens)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Metrics.px(8, tokens)

                Text {
                    width: parent.width
                    text: root.currentQuestion.header || "Question"
                    color: root.tokens.tabActiveText
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.micro
                    font.weight: Font.Bold
                    font.capitalization: Font.AllUppercase
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.currentQuestion.question || ""
                    color: root.tokens.textMain
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.tokens.reading
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    lineHeight: 1.35
                }

                Column {
                    width: parent.width
                    spacing: Metrics.px(6, tokens)

                    Row {
                        visible: root.confirmation
                        spacing: Metrics.px(8, tokens)
                        Repeater {
                            model: root.confirmation ? root.currentQuestion.options : []
                            delegate: Basic.Button {
                                required property var modelData
                                objectName: "questionConfirmOption"
                                text: modelData.label
                                enabled: root.editable
                                height: Metrics.px(36, tokens)
                                width: Metrics.px(100, tokens)
                                onClicked: {
                                    root.selectOption(modelData.id)
                                    root.continueQuestion()
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: root.tokens.textMain
                                    font.family: root.tokens.uiFont
                                    font.pixelSize: root.tokens.body
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: Metrics.px(7, tokens)
                                    color: root.tokens.interactiveBackground
                                    border.color: root.tokens.accent
                                }
                            }
                        }
                    }
                    Repeater {
                        model: root.confirmation ? [] : root.currentQuestion.options || []
                        delegate: Basic.Button {
                            id: optionButton
                            objectName: "questionOption"
                            required property var modelData
                            readonly property bool selected: !root.currentUsesOther
                                                             && root.currentAnswers.indexOf(
                                                                 modelData.id) >= 0
                            width: parent.width
                            height: Math.max(Metrics.px(48, tokens), optionCopy.implicitHeight + Metrics.px(16, tokens))
                            padding: 0
                            enabled: root.editable
                            Accessible.name: modelData.label
                            onClicked: root.selectOption(modelData.id)

                            contentItem: Row {
                                anchors.fill: parent
                                anchors.margins: Metrics.px(9, tokens)
                                spacing: Metrics.px(9, tokens)

                                Rectangle {
                                    anchors.top: parent.top
                                    anchors.topMargin: 1
                                    width: Metrics.px(15, tokens)
                                    height: Metrics.px(15, tokens)
                                    radius: Metrics.px(8, tokens)
                                    color: "transparent"
                                    border.color: optionButton.selected
                                                  ? root.tokens.accentHover
                                                  : root.tokens.textMuted
                                    Rectangle {
                                        anchors.centerIn: parent
                                        visible: optionButton.selected
                                        width: Metrics.px(7, tokens)
                                        height: Metrics.px(7, tokens)
                                        radius: Metrics.px(4, tokens)
                                        color: root.tokens.accentHover
                                    }
                                }
                                Column {
                                    id: optionCopy
                                    width: parent.width - Metrics.px(24, tokens)
                                    spacing: Metrics.px(3, tokens)
                                    Text {
                                        width: parent.width
                                        text: optionButton.modelData.label
                                        color: root.tokens.textMain
                                        font.family: root.tokens.uiFont
                                        font.pixelSize: root.tokens.body
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.Wrap
                                    }
                                    Text {
                                        width: parent.width
                                        visible: text.length > 0
                                        text: optionButton.modelData.description || ""
                                        color: root.tokens.textMuted
                                        font.family: root.tokens.uiFont
                                        font.pixelSize: root.tokens.caption
                                        wrapMode: Text.Wrap
                                        lineHeight: 1.25
                                    }
                                }
                            }
                            background: Rectangle {
                                radius: Metrics.px(7, tokens)
                                color: optionButton.selected ? root.tokens.accentSubtle
                                                             : optionButton.hovered
                                                               ? root.tokens.surfaceHover
                                                               : root.tokens.interactiveBackground
                                border.color: optionButton.selected ? root.tokens.accent
                                                                    : root.tokens.border
                            }
                        }
                    }

                    Basic.Button {
                        id: otherButton
                        objectName: "questionOtherOption"
                        visible: !root.confirmation && root.currentQuestion.isOther === true
                                 && (root.currentQuestion.options || []).length > 0
                        width: parent.width
                        height: Metrics.px(44, tokens)
                        padding: 0
                        enabled: root.editable
                        Accessible.name: "Other"
                        onClicked: root.selectOther()
                        contentItem: Row {
                            anchors.fill: parent
                            anchors.margins: Metrics.px(9, tokens)
                            spacing: Metrics.px(9, tokens)
                            Rectangle {
                                width: Metrics.px(15, tokens)
                                height: Metrics.px(15, tokens)
                                radius: Metrics.px(8, tokens)
                                color: "transparent"
                                border.color: root.currentUsesOther ? root.tokens.accentHover
                                                                  : root.tokens.textMuted
                                Rectangle {
                                    anchors.centerIn: parent
                                    visible: root.currentUsesOther
                                    width: Metrics.px(7, tokens)
                                    height: Metrics.px(7, tokens)
                                    radius: Metrics.px(4, tokens)
                                    color: root.tokens.accentHover
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Other…"
                                color: root.tokens.textMain
                                font.family: root.tokens.uiFont
                                font.pixelSize: root.tokens.body
                                font.weight: Font.DemiBold
                            }
                        }
                        background: Rectangle {
                            radius: Metrics.px(7, tokens)
                            color: root.currentUsesOther ? root.tokens.accentSubtle
                                                         : otherButton.hovered
                                                           ? root.tokens.surfaceHover
                                                           : root.tokens.interactiveBackground
                            border.color: root.currentUsesOther ? root.tokens.accent
                                                                : root.tokens.border
                        }
                    }

                    Rectangle {
                        visible: root.currentUsesOther
                                 || (root.currentQuestion.options || []).length === 0
                        width: parent.width
                        height: Metrics.px(58, tokens)
                        radius: Metrics.px(7, tokens)
                        color: root.tokens.pageBackground
                        border.color: otherInput.activeFocus ? root.tokens.focus
                                                             : root.tokens.borderStrong
                        Basic.TextField {
                            id: otherInput
                            objectName: "questionOtherInput"
                            anchors.fill: parent
                            readOnly: !root.editable
                            anchors.margins: Metrics.px(6, tokens)
                            text: root.currentUsesOther || (root.currentQuestion.options || []).length === 0
                                  ? root.currentAnswerText : ""
                            placeholderText: "Type your answer…"
                            color: root.tokens.textMain
                            placeholderTextColor: root.tokens.textMuted
                            font.family: root.tokens.uiFont
                            font.pixelSize: root.tokens.body
                            selectByMouse: true
                            echoMode: root.currentQuestion.isSecret === true
                                      ? TextInput.Password : TextInput.Normal
                            background: null
                            onTextChanged: {
                                if (visible && text !== root.currentAnswerText)
                                    root.setDraft(root.currentQuestionId,
                                                  { option_ids: [], custom_answer: text })
                            }
                        }
                    }
                }

                Row {
                    visible: !root.confirmation
                    width: parent.width
                    height: Metrics.px(44, tokens)
                    spacing: Metrics.px(6, tokens)

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Metrics.px(5, tokens)
                        Repeater {
                            model: root.questions.length > 1 ? root.questions.length : 0
                            delegate: Rectangle {
                                required property int index
                                width: Metrics.px(18, tokens)
                                height: Metrics.px(3, tokens)
                                radius: Metrics.px(2, tokens)
                                color: index < root.currentIndex ? root.tokens.positive
                                      : index === root.currentIndex ? root.tokens.accentHover
                                                                    : root.tokens.borderStrong
                            }
                        }
                    }
                    Item { width: Math.max(0, parent.width - stepButtons.width - 50); height: 1 }
                    Row {
                        id: stepButtons
                        height: parent.height
                        spacing: Metrics.px(6, tokens)
                        Basic.Button {
                            id: backButton
                            objectName: "questionBackButton"
                            visible: root.currentIndex > 0
                            height: parent.height
                            width: Metrics.px(68, tokens)
                            text: "Back"
                            onClicked: {
                                root.currentIndex -= 1
                                root.draftChanged(root.draftAnswers, root.currentIndex,
                                                  root.otherQuestions)
                            }
                            contentItem: Text {
                                text: backButton.text
                                color: root.tokens.textMuted
                                font.family: root.tokens.uiFont
                                font.pixelSize: root.tokens.caption
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: Metrics.px(6, tokens)
                                color: backButton.hovered ? root.tokens.surfaceHover : "transparent"
                            }
                        }
                        Basic.Button {
                            id: continueButton
                            objectName: "questionContinueButton"
                            enabled: root.canContinue
                            height: parent.height
                            width: root.currentIndex + 1 === root.questions.length ? 104 : 72
                            text: root.currentIndex + 1 === root.questions.length ? "Submit" : "Continue"
                            onClicked: root.continueQuestion()
                            contentItem: Text {
                                text: continueButton.text
                                color: continueButton.enabled ? "#ffffff" : root.tokens.textDisabled
                                font.family: root.tokens.uiFont
                                font.pixelSize: root.tokens.caption
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: Metrics.px(6, tokens)
                                color: !continueButton.enabled ? root.tokens.interactiveBackground
                                       : continueButton.pressed ? root.tokens.actionPrimaryActive
                                       : continueButton.hovered ? root.tokens.actionPrimaryHover
                                       : root.tokens.actionPrimary
                            }
                        }
                    }
                }
            }

            Column {
                id: receiptColumn
                objectName: "questionReceipt"
                visible: root.answered
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Metrics.px(14, tokens)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Metrics.px(9, tokens)

                Repeater {
                    model: root.questions
                    delegate: Row {
                        required property var modelData
                        width: parent.width
                        spacing: Metrics.px(8, tokens)
                        Rectangle {
                            width: Metrics.px(17, tokens)
                            height: Metrics.px(17, tokens)
                            radius: Metrics.px(9, tokens)
                            color: root.tokens.positive
                            opacity: 0.22
                            Text {
                                anchors.centerIn: parent
                                text: "✓"
                                color: root.tokens.positive
                                font.pixelSize: root.tokens.micro
                                font.weight: Font.Bold
                            }
                        }
                        Column {
                            width: parent.width - Metrics.px(25, tokens)
                            spacing: Metrics.px(2, tokens)
                            Text {
                                width: parent.width
                                text: modelData.header || "Answer"
                                color: root.tokens.textMuted
                                font.family: root.tokens.uiFont
                                font.pixelSize: root.tokens.micro
                                font.weight: Font.DemiBold
                                font.capitalization: Font.AllUppercase
                            }
                            Text {
                                width: parent.width
                                text: root.answerData[modelData.id] || ""
                                color: root.tokens.textSecondary
                                font.family: root.tokens.uiFont
                                font.pixelSize: root.tokens.body
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
}
