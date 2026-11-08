import Ket
import QtQuick
import Ket 1.0 as Kit
import QtQuick.Layouts
import QtQuick.Controls.Basic as Basic

Kit.KetQuestionCard {
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
        submitted = false;
        Qt.callLater(restoreInitialDraft);
    }
    readonly property bool answered: Object.keys(answerData || {}).length > 0
    readonly property var currentQuestion: questions.length > 0 ? questions[Math.min(currentIndex, questions.length - 1)] : ({})
    readonly property bool confirmation: {
        const opts = currentQuestion.options || [];
        if (questions.length !== 1 || currentQuestion.selection === "multiple" || currentQuestion.isSecret || opts.length !== 2)
            return false;
        const labels = opts.map(o => o.label.trim().toLowerCase()).sort().join("|");
        return labels === "no|yes" || labels === "có|không";
    }
    readonly property string currentQuestionId: currentQuestion.id || ""
    readonly property var currentAnswer: draftAnswers[currentQuestionId] || ({})
    readonly property var currentAnswers: currentAnswer.option_ids || []
    readonly property string currentAnswerText: currentAnswer.custom_answer || ""
    readonly property bool currentUsesOther: otherQuestions[currentQuestionId] === true
    // A closed or already sent question is shown, not edited.
    readonly property bool editable: pending && !submitted && !answered
    readonly property bool canContinue: pending && !submitted && (currentAnswers.length > 0 || currentAnswerText.trim().length > 0)

    signal answersSubmitted(var answers)
    signal draftChanged(var answers, int questionIndex, var otherAnswers)
    signal draftDiscarded

    function restoreInitialDraft() {
        submitted = false;
        draftAnswers = Object.assign({}, initialDraftAnswers || {});
        otherQuestions = Object.assign({}, initialOtherQuestions || {});
        currentIndex = Math.max(0, Math.min(initialQuestionIndex, questions.length - 1));
    }
    Component.onCompleted: restoreInitialDraft()
    onPendingChanged: {
        if (!pending && !answered)
            draftDiscarded();
    }
    onAnsweredChanged: {
        if (answered)
            draftDiscarded();
    }

    function setDraft(questionId, answer) {
        const updated = Object.assign({}, draftAnswers);
        updated[questionId] = answer;
        draftAnswers = updated;
        draftChanged(draftAnswers, currentIndex, otherQuestions);
    }

    function setOther(questionId, enabled) {
        const updated = Object.assign({}, otherQuestions);
        updated[questionId] = enabled;
        otherQuestions = updated;
        draftChanged(draftAnswers, currentIndex, otherQuestions);
    }

    function selectOption(optionId) {
        if (!editable)
            return;
        setOther(currentQuestionId, false);
        if (currentQuestion.selection === "multiple") {
            const selected = currentAnswers.slice();
            const index = selected.indexOf(optionId);
            if (index >= 0)
                selected.splice(index, 1);
            else
                selected.push(optionId);
            setDraft(currentQuestionId, {
                option_ids: selected,
                custom_answer: ""
            });
        } else {
            setDraft(currentQuestionId, {
                option_ids: [optionId],
                custom_answer: ""
            });
        }
    }

    function selectOther() {
        if (!editable)
            return;
        setOther(currentQuestionId, true);
        setDraft(currentQuestionId, {
            option_ids: [],
            custom_answer: ""
        });
        Qt.callLater(function () {
            otherInput.forceActiveFocus();
        });
    }

    function continueQuestion() {
        if (!canContinue)
            return;
        if (currentIndex + 1 < questions.length) {
            currentIndex += 1;
            draftChanged(draftAnswers, currentIndex, otherQuestions);
            return;
        }
        submitted = true;
        const completedAnswers = Object.assign({}, draftAnswers);
        answersSubmitted(completedAnswers);
        draftAnswers = ({});
    }

    property string lifecycleStatus: "pending"
    label: answered ? "Answered" : submitted ? "Answer sent" : !pending ? (lifecycleStatus === "deferred" ? "Question deferred" : "Question closed") : "Your input is needed" + (currentQuestion.header ? " · " + currentQuestion.header : "")
    question: answered ? "" : currentQuestion.question || ""
    step: !answered && questions.length > 1 ? (currentIndex + 1) + "/" + questions.length : ""

    Column {
        width: parent.width
        visible: !root.answered
        spacing: root.recipe.headingSpacing
        Flow {
            visible: root.confirmation
            width: parent.width
            spacing: KetTheme.px(8, KetTheme.zoom)
            Repeater {
                model: root.confirmation ? root.currentQuestion.options : []
                Kit.KetButton {
                    required property var modelData
                    objectName: "questionConfirmOption"
                    text: modelData.label
                    enabled: root.editable
                    onClicked: {
                        root.selectOption(modelData.id);
                        root.continueQuestion();
                    }
                }
            }
        }
        Repeater {
            model: root.confirmation ? [] : root.currentQuestion.options || []
            Kit.KetChoice {
                required property var modelData
                objectName: "questionOption"
                readonly property bool selected: !root.currentUsesOther && root.currentAnswers.indexOf(modelData.id) >= 0
                width: parent.width
                text: modelData.label
                description: modelData.description || ""
                kind: root.currentQuestion.selection === "multiple" ? Kit.KetTheme.ToggleKind.Checkbox : Kit.KetTheme.ToggleKind.Radio
                autoExclusive: false
                checked: selected
                enabled: root.editable
                onClicked: root.selectOption(modelData.id)
            }
        }
        Kit.KetChoice {
            objectName: "questionOtherOption"
            width: parent.width
            visible: !root.confirmation && root.currentQuestion.isOther === true && (root.currentQuestion.options || []).length > 0
            text: "Other…"
            kind: Kit.KetTheme.ToggleKind.Radio
            autoExclusive: false
            checked: root.currentUsesOther
            enabled: root.editable
            onClicked: root.selectOther()
        }
        Kit.KetTextField {
            id: otherInput
            objectName: "questionOtherInput"
            width: parent.width
            visible: !root.confirmation && (root.currentUsesOther || (root.currentQuestion.options || []).length === 0)
            readOnly: !root.editable
            text: visible ? root.currentAnswerText : ""
            placeholderText: "Type your answer…"
            selectByMouse: true
            echoMode: root.currentQuestion.isSecret === true ? TextInput.Password : TextInput.Normal
            onTextChanged: {
                if (visible && root.editable && text !== root.currentAnswerText)
                    root.setDraft(root.currentQuestionId, {
                        option_ids: [],
                        custom_answer: text
                    });
            }
        }
    }
    Column {
        objectName: "questionReceipt"
        visible: root.answered
        width: parent.width
        spacing: root.recipe.headingSpacing
        Repeater {
            model: root.questions
            Column {
                required property var modelData
                width: parent.width
                Text {
                    width: parent.width
                    text: modelData.header || "Answer"
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.recipe.labelPixels
                    color: root.recipe.step
                }
                Text {
                    width: parent.width
                    text: root.answerData[modelData.id] || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: root.tokens.uiFont
                    font.pixelSize: root.recipe.headingPixels
                    color: root.recipe.heading
                }
            }
        }
    }
    footer: [
        Kit.KetButton {
            objectName: "questionBackButton"
            visible: !root.answered && !root.confirmation && root.currentIndex > 0
            enabled: root.editable
            text: "Back"
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: {
                root.currentIndex -= 1;
                root.draftChanged(root.draftAnswers, root.currentIndex, root.otherQuestions);
            }
        },
        Kit.KetButton {
            objectName: "questionContinueButton"
            visible: !root.answered && !root.confirmation
            enabled: root.canContinue
            text: root.submitted ? "Answer sent" : root.currentIndex + 1 < root.questions.length ? "Next" : "Submit answer"
            variant: Kit.KetTheme.ButtonVariant.Primary
            controlSize: Kit.KetTheme.ControlSize.Small
            onClicked: root.continueQuestion()
        }
    ]
}
