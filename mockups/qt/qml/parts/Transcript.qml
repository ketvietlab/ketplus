// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// A conversation's transcript at the 760 px reading width. `kind` picks the fixture:
// activity, running, question, atlas, error, setup, short, welcome, report or report-running;
// claude, codex and cursor replay every content type of that provider's journal (TranscriptFixtures).
Item {
    id: transcript

    required property var window
    required property string kind
    required property var conversation
    readonly property real column: Math.min(width - px(48), px(760))
    readonly property string asset: "qrc:/qt/qml/KetPlusMock/assets/"
    property bool changesOpen: false
    property bool reportOpen: false
    property bool datasetAnswered: false
    onConversationChanged: datasetAnswered = false
    property int questionStep: 0
    readonly property var blocks: ({ claude: TranscriptFixtures.claude, codex: TranscriptFixtures.codex,
                                     cursor: TranscriptFixtures.cursor,
                                     "turn-report": TranscriptFixtures.turnReport,
                                     "team-assignment": TranscriptFixtures.teamAssignment,
                                     "team-digest": TranscriptFixtures.teamDigest,
                                     "team-review": TranscriptFixtures.teamReview,
                                     "team-verdict": TranscriptFixtures.teamVerdict,
                                     "team-decision": TranscriptFixtures.teamDecision })[kind]
                                   || (Fixtures.jobStates[kind] ? TranscriptFixtures.jobTurn(kind) : null)

    signal openDetails(string tab)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: (blocks ? journal.implicitHeight : body.implicitHeight) + px(48)

    component Note: Rectangle {
        property string iconName
        property string label
        property string message
        width: parent.width
        implicitHeight: noteRow.implicitHeight + transcript.px(20)
        radius: transcript.px(6)
        color: KetTheme.t.panelBgSubtle
        border.color: KetTheme.t.panelBorder
        Row {
            id: noteRow
            x: transcript.px(14)
            width: parent.width - 2 * x
            anchors.verticalCenter: parent.verticalCenter
            spacing: transcript.px(8)
            KetIcon { name: iconName; box: transcript.px(14); color: KetTheme.t.accentText; anchors.verticalCenter: parent.verticalCenter }
            Text {
                width: parent.width - transcript.px(22)
                wrapMode: Text.Wrap
                text: "<b>" + label + "</b>&nbsp;&nbsp;" + message
                textFormat: Text.StyledText
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(13)
            }
        }
    }

    component Card: Rectangle {
        default property alias content: cardColumn.data
        property string title
        property string caption
        width: parent.width
        implicitHeight: cardColumn.implicitHeight + transcript.px(30)
        radius: transcript.px(7)
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.panelBorder
        Column {
            id: cardColumn
            x: transcript.px(15)
            y: transcript.px(15)
            width: parent.width - 2 * x
            spacing: transcript.px(8)
            Text {
                text: title
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(15)
                font.weight: Font.DemiBold
            }
            Text {
                visible: caption !== ""
                width: parent.width
                wrapMode: Text.Wrap
                text: caption
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(13)
            }
        }
    }

    Column {
        id: journal
        visible: !!transcript.blocks
        x: (transcript.width - width) / 2
        y: transcript.px(24)
        width: transcript.column
        spacing: transcript.px(18)
        Repeater {
            model: transcript.blocks || []
            TranscriptBlock {
                required property var modelData
                block: modelData
                window: transcript.window
                provider: transcript.conversation.provider || "the agent"
            }
        }
    }

    Column {
        id: body
        visible: !transcript.blocks
        x: (transcript.width - width) / 2
        y: transcript.px(24)
        width: transcript.column
        spacing: transcript.px(transcript.reportOpen ? 12 : 18)

        // Welcome: a new conversation with starters.
        Column {
            visible: transcript.kind === "welcome"
            width: parent.width
            spacing: transcript.px(18)
            topPadding: transcript.px(90)

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: transcript.asset + "ketplus-mark-v2.png"
                width: transcript.px(44)
                height: width
                sourceSize: Qt.size(width * 2, height * 2)
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "What will you create today?"
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(26)
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Your Agent works in " + transcript.window.repository + " on "
                      + transcript.conversation.worktree + ". Ask anything or start from an idea."
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(14)
            }
            Grid {
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 2
                spacing: transcript.px(10)
                topPadding: transcript.px(12)
                Repeater {
                    model: Fixtures.suggestions
                    KetSuggestion {
                        required property var modelData
                        width: (body.width - transcript.px(10)) / 2
                        text: modelData.title
                        caption: modelData.caption
                        iconName: modelData.icon
                        onClicked: transcript.window.showScreen("workspace/running")
                    }
                }
            }
        }

        KetUserMessage {
            visible: transcript.kind !== "welcome"
            width: parent.width
            text: transcript.kind === "running" ? "Cinder Trail takes four seconds to open the trail picker. Find what blocks the first frame."
                : transcript.kind === "question" ? "Review the first offline field note before we merge the onboarding branch."
                : transcript.kind === "atlas" ? Fixtures.atlasPrompt
                : transcript.kind === "error" ? "Profile the editor startup on a cold cache."
                : transcript.kind === "short" ? "Check this worktree and tell me if anything is uncommitted."
                : transcript.kind === "setup" ? (Fixtures.providerSetup[transcript.conversation.setup] || {}).prompt || ""
                : transcript.kind === "report" ? "Finish the trail picker and verify it for the 1.4 release."
                : transcript.kind === "report-running" ? "Run the picker checks against the approved dataset."
                : "Keep keyboard focus in the trail picker after filtering. Show me what changed."
        }

        // The work performed, collapsed into groups.
        KetActivityGroup {
            visible: ["activity", "running", "atlas", "error", "setup", "report", "report-running"].includes(transcript.kind)
                     && !transcript.reportOpen
            width: parent.width
            text: transcript.kind === "running" ? "Explored 6 files, ran 2 commands"
                : transcript.kind === "report" ? "Edited 3 files, ran picker tests"
                : transcript.kind === "atlas" ? "Built and validated 2 flows"
                : transcript.kind === "report-running" ? "Explored 3 files, editing 1"
                : "Read the worktree and checked the picker"
            expanded: transcript.kind === "running"
            KetActivityRow { text: "Read"; detail: "src/trails/TrailList.qml"; iconName: "file-code" }
            KetActivityRow { text: "Searched"; detail: "“refreshIndex” in src/trails"; iconName: "search" }
            TranscriptActivityItem {
                window: transcript.window
                step: ({
                    kind: "command", title: "Ran", detail: command, command: command,
                    output: "2 picker tests passed"
                })
                readonly property string command: "ctest --test-dir build -R trail_picker"
            }
            KetActivityRow { text: "Compared"; detail: "first-frame timing · 2 runs"; iconName: "clock" }
        }

        KetGallery {
            visible: transcript.kind === "activity" && !transcript.changesOpen
            images: Fixtures.captures.map(capture => ({ source: capture.source,
                                                        caption: capture.caption.split(" · ")[1] }))
            onActivated: index => transcript.window.openImages(Fixtures.captures, index)
        }

        Note {
            visible: transcript.kind === "activity" && !transcript.changesOpen
            iconName: "send"
            label: "Agent message"
            message: "Sent the diff to @agent/4583 for keyboard review."
        }

        MarkdownView {
            visible: ["activity", "running", "short", "report"].includes(transcript.kind)
            width: parent.width
            window: transcript.window
            markdown: transcript.kind === "running" ? Fixtures.runningAnswer
                : transcript.kind === "report" ? "The picker changes are in place. **2 macOS picker tests passed.** The offline Linux check is blocked on dataset approval."
                : transcript.kind === "short" ? "The worktree is **clean**: no staged or unstaged changes, and `develop` is even with `origin/develop`."
                : Fixtures.activityAnswer
            onLinkActivated: link => transcript.window.notify("Opened " + link)
        }

        Row {
            visible: transcript.kind === "running"
            spacing: transcript.px(8)
            KetActivityIndicator { anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: "Working · measuring the first frame"
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(13)
            }
        }

        // The turn closes with the work report; while the next turn runs, it shows that turn's activity.
        TurnReportCard {
            visible: ["report", "report-running"].includes(transcript.kind) && !transcript.reportOpen
            width: parent.width
            report: Fixtures.turnReport
            running: transcript.kind === "report-running"
            activity: Fixtures.turnReport.activity
            openItem: "picker-dialog"
            onOpenRequested: transcript.openDetails("report")
        }

        KetIconButton {
            visible: ["activity", "short", "report"].includes(transcript.kind) && !transcript.reportOpen
            iconName: "copy"
            text: "Copy answer"
            onClicked: transcript.window.notify("Copied the answer")
        }

        Card {
            visible: transcript.kind === "activity" && !transcript.changesOpen
            title: "Referenced design"
            caption: "Code, diff, table, diagram and media detail are loaded on demand."
            KetButton {
                text: "Open preview"
                iconName: "panels-top-left"
                variant: KetTheme.ButtonVariant.Ghost
                onClicked: transcript.openDetails("atlas")
            }
        }

        KetChangedFiles {
            visible: (transcript.kind === "activity" && !transcript.changesOpen) || transcript.kind === "report"
            width: parent.width
            files: Fixtures.changedFiles
            note: "Not committed"
            onFileActivated: transcript.openDetails("changes")
            footer: KetButton {
                visible: !transcript.reportOpen
                text: "Review changes"
                controlSize: KetTheme.ControlSize.Small
                onClicked: transcript.openDetails("changes")
            }
        }

        KetQuestionCard {
            visible: transcript.kind === "report" && !transcript.datasetAnswered
            width: parent.width
            question: Fixtures.datasetQuestion
            KetChoice {
                width: parent.width
                kind: KetTheme.ToggleKind.Radio
                checked: true
                text: "Bundled trail dataset"
                description: "Three trails with synthetic field notes"
            }
            KetChoice {
                width: parent.width
                kind: KetTheme.ToggleKind.Radio
                text: "Full QA dataset"
                description: "Larger route set with sync history"
            }
            footer: KetButton {
                text: "Send answer"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    transcript.datasetAnswered = true
                    transcript.window.notify("Dataset selected · the turn can continue")
                }
            }
        }

        // A pending question pauses the turn.
        KetQuestionCard {
            visible: transcript.kind === "question"
            width: parent.width
            question: transcript.questionStep === 0
                      ? "What should the app show when no trails are saved for offline use?"
                      : "Should I open a pull request when the review passes?"
            step: (transcript.questionStep + 1) + " of 2"
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; checked: true; text: transcript.questionStep === 0 ? "Offer to save a trail" : "Yes, into integration"; description: transcript.questionStep === 0 ? "Show the trail picker with an offline download action" : "Promoted to develop later" }
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; text: transcript.questionStep === 0 ? "Show the empty journal" : "No, only report"; description: transcript.questionStep === 0 ? "Explain that recording needs a saved trail" : "I will open it myself" }
            footer: [
                KetButton {
                    text: transcript.questionStep === 0 ? "Next" : "Send answers"
                    variant: KetTheme.ButtonVariant.Primary
                    onClicked: {
                        if (transcript.questionStep === 0)
                            transcript.questionStep = 1
                        else {
                            transcript.questionStep = 0
                            transcript.window.notify("Answers sent · the turn continues")
                        }
                    }
                },
                KetButton { text: "Skip"; variant: KetTheme.ButtonVariant.Ghost; onClicked: transcript.window.notify("Skipped the question") }
            ]
        }

        // An Atlas the Agent produced.
        Card {
            visible: transcript.kind === "atlas"
            title: "First field entry"
            caption: "first-field-entry.ketatlas · " + Fixtures.atlasSummary + " · Két Design System"
            Row {
                spacing: transcript.px(8)
                KetButton { text: "Edit in KétPlus"; iconName: "edit"; variant: KetTheme.ButtonVariant.Primary; onClicked: transcript.window.showScreen("atlas") }
                KetButton { text: "Open in browser"; iconName: "open"; onClicked: transcript.window.notify("Opened the preview in the browser") }
            }
        }

        // A turn the provider ended (Fixtures.providerSetup): the guide closes the turn.
        Loader {
            active: transcript.kind === "setup"
            visible: active
            width: parent.width
            sourceComponent: ProviderSetupCard {
                setupId: transcript.conversation.setup || ""
                window: transcript.window
            }
        }

        KetBanner {
            visible: transcript.kind === "error"
            width: parent.width
            tone: KetTheme.BannerTone.Danger
            title: "The provider stopped responding"
            message: "Codex did not answer for 60 seconds. Your message and the work so far are kept."
            actionText: "Retry"
            actionIcon: "refresh"
            onActionTriggered: transcript.window.notify("Retrying the turn")
        }
    }
}
