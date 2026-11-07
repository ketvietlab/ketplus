// SPDX-License-Identifier: MPL-2.0
import QtQuick

// Deterministic concept only. No daemon, provider, tracker or filesystem calls.
QtObject {
    id: run
    property string conversationId: ""
    property string status: "ready"
    property int completed: 0
    property int followUps: 0
    property int followUpLimit: 3
    property bool autoplay: false
    property Timer clock: Timer {
        interval: 2200
        repeat: true
        running: run.autoplay && run.active
        onTriggered: run.advance()
    }
    property var events: [{ actor: "Plan", title: "Plan v1 is ready",
        detail: "Four tasks with explicit acceptance criteria. Deployment is outside this plan." }]
    readonly property bool active: status === "running" || status === "continuing"
    readonly property var tasks: [
        { title: "Implement the fix", owner: "Coding agent", criterion: "Patch matches the agreed scope.", evidence: "Commit 9f31c2a · 3 files changed" },
        { title: "Run regression tests", owner: "Coding agent", criterion: "All 48 regression tests pass.", evidence: "48 / 48 passed · test-results.txt" },
        { title: "Review the changes", owner: "Review agent", criterion: "Review the same commit; no open findings.", evidence: "Review for 9f31c2a · no open findings" },
        { title: "Build the release package", owner: "Coding agent", criterion: "Build succeeds and the artifact exists.", evidence: "Build passed · release-package.zip" }
    ]
    function record(actor, title, detail) {
        events = events.concat([{ actor: actor, title: title, detail: detail }])
    }
    function load(state) {
        autoplay = false
        status = "ready"; completed = 0; followUps = 0; followUpLimit = 3; events = []
        record("Plan", "Plan v1 is ready", "Four tasks with explicit acceptance criteria. Deployment is outside this plan.")
        if (state === "ready") return
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
    function start() {
        if (status !== "ready") return
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
        status = "paused"; autoplay = false
        record("You", "Run stopped", "Queued continuation cancelled. Completed tasks and evidence are preserved.")
    }
    function resume() {
        if (status !== "paused") return
        status = "running"; autoplay = true
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
