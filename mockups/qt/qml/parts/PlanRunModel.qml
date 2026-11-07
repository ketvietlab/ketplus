// SPDX-License-Identifier: MPL-2.0
import QtQuick

// Deterministic concept only. No daemon, provider, tracker or filesystem calls.
QtObject {
    id: run
    property string conversationId: ""
    property bool presented: false
    property string status: "ready"
    property int completed: 0
    property int followUps: 0
    property int followUpLimit: 3
    property bool autoplay: false
    property bool contextRead: false
    property bool planCreated: false
    property string resumeState: "running"
    readonly property string prompt: "Fix the release validation failure, verify the change and prepare a package."
    readonly property var sources: [
        { title: "AGENTS.md", detail: "Repo memory · run regression tests and review the same commit before packaging." },
        { title: "docs/release.md", detail: "Repo memory · package locally; publishing requires a separate request." },
        { title: "GitHub issue #241 · Open", detail: "Relevant issue · release validation fails when the fixture path changes. Use its reproduction as an acceptance check." }
    ]
    property Timer clock: Timer {
        interval: 2200
        repeat: true
        running: run.autoplay && run.active
        onTriggered: run.advance()
    }
    property var events: []
    readonly property bool active: status === "planning" || status === "running" || status === "continuing"
    readonly property var tasks: [
        { title: "Fix the validation failure", owner: "Coding agent", criterion: "Issue #241 reproduction passes after the patch.", source: "GitHub #241", evidence: "Commit 9f31c2a · 3 files changed" },
        { title: "Run regression tests", owner: "Coding agent", criterion: "All 48 regression tests pass.", source: "AGENTS.md · GitHub #241", evidence: "48 / 48 passed · test-results.txt" },
        { title: "Review the changes", owner: "Review agent", criterion: "Review the same commit; no open findings.", source: "AGENTS.md", evidence: "Review for 9f31c2a · no open findings" },
        { title: "Build the release package", owner: "Coding agent", criterion: "Build succeeds and the artifact exists.", source: "docs/release.md · user request", evidence: "Build passed · release-package.zip" }
    ]
    function record(actor, title, detail) {
        events = events.concat([{ actor: actor, title: title, detail: detail }])
    }
    function load(state) {
        autoplay = false
        status = "ready"; completed = 0; followUps = 0; followUpLimit = 3; events = []
        contextRead = false; planCreated = false; resumeState = "running"
        if (state === "ready") return
        record("You", "Request received", prompt)
        record("Agent", "Multi-step work detected", "Inspect repo memory and relevant open issues, then create a plan for this conversation.")
        if (state === "planning") { status = state; return }
        readContext()
        createPlan()
        completed = state === "complete" ? 4 : 1
        record("Agent", "Task 1 verified", tasks[0].evidence)
        status = state
        if (state === "continuing") {
            record("Agent", "Turn ended early", "The patch is ready. Tests, review and packaging are still open.")
            record("Daemon", "Continuation queued", "@agent/" + conversationId + ": Plan v1 is 1/4 complete. Resume task 2; retain unfinished tasks.")
        } else if (state === "blocked") {
            record("Daemon", "Waiting for a dependency", "The regression fixture is unavailable. The repository owner must restore it.")
        } else if (state === "paused") {
            record("You", "Run stopped", "No automatic follow-up will start. Resume explicitly when ready.")
        } else if (state === "limit") {
            followUps = 3
            record("Daemon", "Follow-up limit reached", "Three follow-ups produced no verified progress. The plan remains 1/4 complete.")
        } else if (state === "complete") {
            followUps = 1
            for (let i = 1; i < 4; ++i) record("Agent", "Task " + (i + 1) + " verified", tasks[i].evidence)
            record("Daemon", "Plan complete", "All four acceptance criteria have evidence. No further follow-up is scheduled.")
        }
    }
    function submitPrompt() {
        if (status !== "ready") return
        status = "planning"; autoplay = true
        record("You", "Request received", prompt)
        record("Agent", "Multi-step work detected", "A fix, verification, review and packaging need a conversation plan. Reading repo memory and open issues first.")
    }
    function readContext() {
        contextRead = true
        record("Agent", "Repo context read · issue #241 selected", "Read AGENTS.md, docs/release.md and open issues. #241 matches this request; unrelated backlog stays outside the plan.")
    }
    function createPlan() {
        planCreated = true
        record("Agent", "Plan v1 created from repo context", "Four tasks scoped to this request, with source references and acceptance criteria. Deployment is outside the plan.")
    }
    function start() {
        if (status !== "ready") return
        if (!contextRead) readContext()
        if (!planCreated) createPlan()
        status = "running"; autoplay = true
        record("Agent", "Working on task 1", "Implementing the agreed fix on release/1.4.")
    }
    function earlyFinish() {
        if (status !== "running" || completed === 4) return
        status = "continuing"
        record("Agent", "Turn ended early", completed + "/4 tasks verified. The remaining tasks are still open.")
        record("Daemon", "Continuation queued", "Resume @agent/" + conversationId + " at task " + (completed + 1) + " against Plan v1. Do not repeat completed work.")
    }
    function advance() {
        if (!active) return
        if (status === "planning") {
            if (!contextRead) readContext()
            else { createPlan(); status = "ready"; start() }
            return
        }
        if (status === "continuing") {
            if (followUps >= followUpLimit) {
                status = "limit"; autoplay = false
                record("Daemon", "Follow-up limit reached", "Work remains. Review the cause before extending the limit.")
                return
            }
            ++followUps; status = "running"
            record("Daemon", "Follow-up " + followUps + " started", "Resume @agent/" + conversationId + " at task " + (completed + 1) + ". Keep Plan v1 scope and evidence.")
            return
        }
        ++completed
        record("Agent", "Task " + completed + " verified", tasks[completed - 1].evidence)
        if (completed === 1) {
            earlyFinish()
        } else if (completed === 4) {
            status = "complete"; autoplay = false
            record("Daemon", "Plan complete", "4/4 acceptance criteria verified. No automatic follow-up remains.")
        }
    }
    function stop() {
        if (!active) return
        resumeState = status === "planning" ? "planning" : "running"
        status = "paused"; autoplay = false
        record("You", "Run stopped", "Queued continuation cancelled. Completed tasks and evidence are preserved.")
    }
    function resume() {
        if (status !== "paused") return
        status = resumeState; autoplay = true
        record("You", "Run resumed", "Continue at task " + (completed + 1) + ".")
    }
    function retryBlocker() {
        if (status !== "blocked") return
        // The fixture intentionally remains missing; a retry cannot pretend it is resolved.
        record("Daemon", "Dependency is still unavailable", "Repository owner: restore the regression fixture. No follow-up started.")
    }
    function extendLimit() {
        if (status !== "limit") return
        ++followUpLimit; status = "continuing"; autoplay = true
        record("You", "One extra follow-up allowed", "New limit: " + followUpLimit + ". Existing attempt count is retained.")
    }
}
