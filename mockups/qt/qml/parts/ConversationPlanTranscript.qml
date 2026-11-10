// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// The toolbar opens the plan. Chat keeps the final report, not a second activity feed.
Column {
    id: transcript
    required property var window
    required property var run
    function px(n) { return KetTheme.px(n, KetTheme.zoom) }
    function reportMarkdown(report) {
        if (!report) return ""
        const evidence = ["✓ Commit 9f31c2a", "✓ 48/48 passed", "✓ No open findings", "✓ Package built"]
        let text = "**Turn report**\n\n**Plan: " + report.completed + "/" + run.tasks.length
            + " verified · " + run.statusLabel(report.status) + ".**\n\n"
            + "| Task | Result |\n| --- | --- |\n"
        for (let i = 0; i < run.tasks.length; ++i) {
            const result = i < report.completed ? evidence[i]
                : i === report.completed && report.status === "blocked" ? "🔴 Blocked" : "🟡 Pending"
            text += "| " + run.tasks[i].title + " | " + result + " |\n"
        }
        const next = report.status === "complete" ? "None. All four tasks are verified."
            : report.status === "blocked" ? "Repository owner: restore the regression fixture. Then the coding agent resumes tests."
            : report.status === "paused" ? "You: resume when ready. Automatic follow-up is stopped."
            : report.status === "limit" ? "You: review the stalled work before allowing another follow-up."
            : run.tasks[report.completed].title + " · " + run.tasks[report.completed].owner + ". Automatic follow-up queued."
        return text + "\n**Next:** " + next
    }
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
            markdown: run.planCreated
                ? "I created a four-task plan from the repository guidance and issue #241."
                : run.status === "ready" ? "Send the sample prompt to preview automatic planning."
                : run.contextRead ? "Repository context checked. I’m creating a plan for this request."
                : "This needs several steps. I’m checking the repository memory and relevant open issues before creating a plan."
        }
        MarkdownView {
            visible: !!run.lastReport
            width: parent.width
            window: transcript.window
            markdown: transcript.reportMarkdown(run.lastReport)
        }
    }
}
