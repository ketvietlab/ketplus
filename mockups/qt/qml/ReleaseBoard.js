// SPDX-License-Identifier: MPL-2.0
.pragma library

function make(base) {
    return Object.assign({}, base, {
        id: "field-release",
        name: "Cinder Trail 1.4",
        goal: "Ship offline field notes. Keep the trail picker fast, prove notes survive a restart, and get the Linux release check signed off.",
        members: [
            { number: "4821", title: "Release handoff", role: "leader", provider: "Codex · GPT-5.6 Luna",
              lane: "", paths: [], state: "working", now: "Tracking the release checks" },
            { number: "2035", title: "Trail picker focus", role: "developer", provider: "Cursor · Composer 2",
              lane: "Trail picker", paths: ["src/trails/"], state: "idle", now: "T-1 · review passed" },
            { number: "4583", title: "Picker review", role: "reviewer", provider: "Claude · Opus 5.5",
              lane: "", reviews: ["Trail picker", "Offline QA"], paths: [], state: "idle", now: "T-1 · keyboard review passed" }
        ],
        storage: { free: 64.2, worktrees: [] },
        tasks: [
            { id: "T-1", title: "Keep keyboard focus after filtering", owner: "2035", lane: "Trail picker",
              state: "done", pr: "#412", branch: "fix/trail-picker", worktree: "cinder-trail-picker", dependsOn: "",
              criteria: [
                  { text: "Enter moves focus from the filter to the results", done: true },
                  { text: "Opening the selected trail works with the keyboard", done: true }
              ],
              history: [{ when: "Today, 10:42", text: "@agent/4583 passed review: both keyboard checks passed." }] },
            { id: "T-2", title: "Verify an offline note on Linux", owner: "4821", lane: "Offline QA",
              state: "blocked", pr: "", branch: "develop", worktree: "", dependsOn: "", decision: "D-1",
              criteria: [
                  { text: "A note saves with the network off", done: false },
                  { text: "The note survives a restart", done: false }
              ],
              history: [{ when: "Today, 11:05", text: "The smoke test is waiting for the dataset choice." }] }
        ],
        decisions: [{
            id: "D-1", question: "Which dataset should we use for the offline check?",
            detail: "The bundled set covers the release gate. The full QA set adds sync history.",
            askedBy: "4821", task: "T-2", when: "8 min ago",
            options: ["Bundled trail dataset", "Full QA dataset"], recommended: 0
        }],
        updates: [{
            icon: "check", text: "T-1 review passed · 2/2 criteria",
            detail: "@agent/4583 · Keyboard focus and trail selection verified", when: "24 min ago"
        }]
    })
}
