// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// The plan is conversation content. Its checklist opens in the existing detail pane.
Column {
    id: transcript
    required property var window
    required property var run
    signal openPlan()
    function px(n) { return KetTheme.px(n, KetTheme.zoom) }
    topPadding: px(24)
    bottomPadding: px(24)
    spacing: px(20)

    Column {
        x: transcript.px(24)
        width: parent.width - 2 * x
        spacing: transcript.px(20)
        KetUserMessage { width: parent.width; text: run.prompt }
        MarkdownView {
            width: parent.width
            window: transcript.window
            markdown: run.contextRead
                ? "I used the repository guidance and issue #241 to create a plan for this request. Each task has a source and an acceptance check."
                : run.status === "ready" ? "Send the sample prompt to preview automatic planning."
                : "This needs several steps. I’m checking the repository memory and relevant open issues before creating a plan."
        }
        KetButton {
            text: run.planCreated ? "Plan · " + run.completed + "/4 verified" : "View planning"
            iconName: "check-all"
            variant: KetTheme.ButtonVariant.Secondary
            onClicked: transcript.openPlan()
        }
        Repeater {
            model: run.events.filter(e => e.actor !== "You").slice(-3)
            Column {
                required property var modelData
                width: parent.width
                spacing: transcript.px(5)
                Text {
                    width: parent.width
                    text: modelData.actor + " · " + modelData.title
                    color: modelData.actor === "Daemon" ? KetTheme.t.accentText : KetTheme.t.textMain
                    font.family: KetTheme.fontFamily
                    font.pixelSize: transcript.px(13)
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: modelData.detail
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: transcript.px(13)
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
