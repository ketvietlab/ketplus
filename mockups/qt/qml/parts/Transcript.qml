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
    property int questionStep: 0
    readonly property var blocks: ({ claude: TranscriptFixtures.claude, codex: TranscriptFixtures.codex,
                                     cursor: TranscriptFixtures.cursor })[kind] || null

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
        spacing: transcript.px(18)

        // Welcome: a new conversation with starters.
        Column {
            visible: transcript.kind === "welcome"
            width: parent.width
            spacing: transcript.px(18)
            topPadding: transcript.px(90)

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: transcript.asset + "ketplus-mark.svg"
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
            text: transcript.kind === "running" ? "Why does the desktop take four seconds to show the first window? Profile the startup and propose a fix."
                : transcript.kind === "question" ? "Review the onboarding feature branch before we merge it."
                : transcript.kind === "atlas" ? "Design an onboarding flow for KetPlus Desktop, from opening a project to the first conversation."
                : transcript.kind === "error" ? "Profile the editor startup on a cold cache."
                : transcript.kind === "short" ? "Check this worktree and tell me if anything is uncommitted."
                : transcript.kind === "setup" ? (Fixtures.providerSetup[transcript.conversation.setup] || {}).prompt || ""
                : transcript.kind === "report" ? "Finish the repository picker, then tell me what is left on the new UI."
                : transcript.kind === "report-running" ? "Carry on with the dialog."
                : "Make transcript activity explicit and easier to scan."
        }

        // The work performed, collapsed into groups.
        KetActivityGroup {
            visible: ["activity", "running", "atlas", "error", "setup", "report", "report-running"].includes(transcript.kind)
            width: parent.width
            text: transcript.kind === "running" ? "Explored 6 files, ran 2 commands"
                : transcript.kind === "report" ? "Edited 4 files, ran 3 commands"
                : transcript.kind === "report-running" ? "Explored 3 files, editing 1"
                : "Used KetPlus editor and browser"
            expanded: transcript.kind === "running"
            KetActivityRow { text: "Read"; detail: "apps/desktop/src/main.cpp"; iconName: "file-code" }
            KetActivityRow { text: "Searched"; detail: "“indexAsync” in modules/"; iconName: "search" }
            KetActivityRow { text: "Ran"; detail: "ctest --test-dir build -R startup"; iconName: "terminal"; expandable: true }
            KetActivityRow { text: "Used browser"; detail: "visual compare · 2 captures"; iconName: "image" }
        }

        KetGallery {
            visible: transcript.kind === "activity"
            images: Fixtures.captures.map(capture => ({ source: capture.source,
                                                        caption: capture.caption.split(" · ")[1] }))
            onActivated: index => transcript.window.openImages(Fixtures.captures, index)
        }

        Note {
            visible: transcript.kind === "activity"
            iconName: "send"
            label: "Agent message"
            message: "Sent to @agent/9410 · Review the startup flow · started a turn · reply requested."
        }

        MarkdownView {
            visible: ["activity", "running", "short", "report"].includes(transcript.kind)
            width: parent.width
            window: transcript.window
            markdown: transcript.kind === "running" ? Fixtures.runningAnswer
                : transcript.kind === "report" ? "The daemon now lists another repository's worktrees without switching to it, and the dialog follows the mock flow. Its tests and the call site are next; two decisions are yours."
                : transcript.kind === "short" ? "The worktree is **clean**: no staged or unstaged changes, and `develop` is even with `origin/develop`."
                : Fixtures.activityAnswer
            onLinkActivated: link => transcript.window.notify("Opened " + link)
        }

        Row {
            visible: transcript.kind === "running"
            spacing: transcript.px(8)
            KetActivityIndicator { anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: "Working · profiling the provider handshake"
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: transcript.px(13)
            }
        }

        // The turn closes with the work report; while the next turn runs, it shows that turn's activity.
        TurnReportCard {
            visible: ["report", "report-running"].includes(transcript.kind)
            width: parent.width
            report: Fixtures.turnReport
            running: transcript.kind === "report-running"
            activity: Fixtures.turnReport.activity
            openItem: "picker-dialog"
            onOpenRequested: transcript.openDetails("report")
        }

        KetIconButton {
            visible: ["activity", "short", "report"].includes(transcript.kind)
            iconName: "copy"
            text: "Copy answer"
            onClicked: transcript.window.notify("Copied the answer")
        }

        Card {
            visible: transcript.kind === "activity"
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
            visible: transcript.kind === "activity"
            width: parent.width
            files: Fixtures.changedFiles
            note: "Not committed"
            onFileActivated: transcript.openDetails("changes")
            footer: KetButton {
                text: "Review changes"
                controlSize: KetTheme.ControlSize.Small
                onClicked: transcript.openDetails("changes")
            }
        }

        // A pending question pauses the turn.
        KetQuestionCard {
            visible: transcript.kind === "question"
            width: parent.width
            question: transcript.questionStep === 0
                      ? "The branch changes the first-run copy. Which tone should the review check against?"
                      : "Should I open a pull request when the review passes?"
            step: (transcript.questionStep + 1) + " of 2"
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; checked: true; text: transcript.questionStep === 0 ? "Friendly and short" : "Yes, into integration"; description: transcript.questionStep === 0 ? "Matches the mobile onboarding" : "Promoted to develop later" }
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; text: transcript.questionStep === 0 ? "Formal" : "No, only report"; description: transcript.questionStep === 0 ? "Matches the Settings copy" : "I will open it myself" }
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
            title: "Validated preview available"
            caption: "onboarding.ketatlas · 4 screens · 2 flows · Két Design System"
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
