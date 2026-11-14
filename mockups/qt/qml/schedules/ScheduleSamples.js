// SPDX-License-Identifier: MPL-2.0
.pragma library

function waitingTeam(base) {
    return Object.assign({}, base, {
        id: "website", name: "Website", goal: "Write a new post for the website", branch: "develop",
        automaticTurns: 0, tasks: [], decisions: [], updates: [], paused: false,
        storage: { free: 64.2, worktrees: [] },
        members: base.members.map((m, i) => Object.assign({}, m, {
            number: String(6100 + i), state: "idle", now: "Not started", reviewing: "", queue: []
        }))
    })
}
