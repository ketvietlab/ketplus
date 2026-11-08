import QtQuick
import QtTest
import "../../modules/ai/src/agent/qml" as Agent
TestCase {
    name: "QuestionCard"
    visible: true
    width: 700; height: 600
    when: windowShown
    Component {
        id: factory
        Agent.AgentQuestionCard {
            width: 650
            questions: []
            answerData: ({})
            pending: true
            messageKey: "fixture"
            tokens: ({zoomFactor: 1, uiFont: "Arial", body: 13, reading: 13, caption: 12, micro: 11,
                tabActiveText: "blue", textSecondary: "gray", surface: "white", positive: "green", borderStrong: "gray", surfaceRaised: "white", interactiveBackground: "white",
                accentMuted: "gray", accentHover: "blue", accent: "blue", textMain: "black", textMuted: "gray",
                border: "gray", surfaceHover: "gray", accentSubtle: "white", pageBackground: "white", focus: "blue", textDisabled: "gray"})
        }
    }
    SignalSpy { id: sent; signalName: "answersSubmitted" }
    function question(options) { return [{id: "q", question: "Ready?", selection: "single", isOther: true, options: options}] }
    function test_confirmation() {
        const card = createTemporaryObject(factory, this, {questions: question([{id:"original-yes", label:"Có"}, {id:"original-no", label:"Không"}])})
        verify(card !== null)
        wait(20)
        verify(card.confirmation)
        compare(findChild(card, "questionOtherOption").visible, false)
        compare(findChild(card, "questionContinueButton").visible, false)
        sent.target = card; sent.clear()
        const button = findChild(card, "questionConfirmOption")
        mouseClick(button)
        compare(sent.count, 1)
        compare(sent.signalArguments[0][0].q.option_ids[0], "original-yes")
        mouseClick(button)
        compare(sent.count, 1)
    }
    function test_free_text() {
        const card = createTemporaryObject(factory, this, {questions: question([])})
        wait(20)
        verify(!card.confirmation)
        compare(findChild(card, "questionOtherOption").visible, false)
        verify(findChild(card, "questionOtherInput").visible)
        compare(card.step, "")
    }
    function test_choices_not_confirmation() {
        const card = createTemporaryObject(factory, this, {questions: question([{id:"a", label:"Workspace"}, {id:"b", label:"Full access"}])})
        wait(20)
        verify(!card.confirmation)
        verify(findChild(card, "questionOtherOption").visible)
        verify(findChild(card, "questionContinueButton").visible)
    }
}
