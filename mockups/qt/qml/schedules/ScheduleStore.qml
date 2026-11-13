// SPDX-License-Identifier: MPL-2.0
import QtQuick
import "ScheduleRules.js" as Rules

QtObject {
    id: store
    property var entries: []
    property int serial: 5
    readonly property var initial: [
        { id: "web", name: "Daily website article", target: 2, teamId: "release", teamName: "Release 1.4", conversation: "4821", rule: 1, time: "08:00", date: "2026-11-14", zone: "Asia/Ho_Chi_Minh", days: [1,2,3,4,5], hours: "4", prompt: "Write a new website article: choose a useful topic, prepare an English draft and send it to the leader for approval before publishing.", enabled: true, creator: "You", busy: 0, missed: 0, provider: "claude", model: "Opus 5.5", effort: "High", repository: "ketplus", worktree: true, last: "✓ 13/11 08:00" },
        { id: "ci", name: "Summarize failing CI", target: 1, teamId: "", conversation: "", rule: 2, time: "08:30", date: "2026-11-14", zone: "Asia/Ho_Chi_Minh", days: [1,2,3,4,5], hours: "4", prompt: "Review failing CI workflows in the repository, group failures by cause and suggest fixes. Do not edit or push code.", enabled: true, creator: "You", busy: 0, missed: 0, provider: "codex", model: "GPT-5.6 Sol", effort: "Medium", repository: "ketplus", worktree: true, last: "✗ Provider is not signed in" },
        { id: "retry", name: "Recheck CI", target: 0, teamId: "", conversation: "8116", rule: 4, time: "16:00", date: "2026-11-14", zone: "Asia/Ho_Chi_Minh", days: [1], hours: "2", prompt: "Recheck the CI workflow and report its status in this conversation.", enabled: true, creator: "Agent #8116", busy: 1, missed: 1, provider: "codex", model: "GPT-5.6 Sol", effort: "Medium", repository: "ketplus", worktree: false, last: "↷ Skipped: busy" },
        { id: "start", name: "Start Release 1.4", target: 2, teamId: "release", teamName: "Release 1.4", conversation: "4821", kind: "team-start", rule: 0, time: "22:00", date: "2026-11-13", zone: "Asia/Ho_Chi_Minh", days: [], hours: "4", prompt: "Ask the leader to plan the website work and assign tasks to team members.", enabled: true, creator: "You", busy: 0, missed: 0, last: "Not run yet" },
        { id: "weekly", name: "Weekly report", target: 2, teamId: "release", teamName: "Release 1.4", conversation: "4821", rule: 3, time: "17:00", date: "2026-11-14", zone: "Asia/Ho_Chi_Minh", days: [5], hours: "4", prompt: "Summarize progress and pending decisions, then send a weekly report to the leader.", enabled: false, creator: "You", busy: 0, missed: 0, last: "✓ 06/11 17:00" }
    ]
    Component.onCompleted: reset()
    function reset() { entries = initial.map(e => Object.assign({}, e)); serial = 5 }
    function find(id) { return entries.find(e => e.id === id) || null }
    function patch(id, changes) { entries = entries.map(e => e.id === id ? Object.assign({}, e, changes) : e) }
    function remove(id) { entries = entries.filter(e => e.id !== id) }
    function save(draft) {
        if (Rules.error(draft)) return ""
        const id = draft.id || "schedule-" + (++serial)
        const item = Object.assign({}, draft, { id: id, enabled: draft.enabled !== false, creator: draft.creator || "You", last: draft.last || "Not run yet" })
        if (find(id)) patch(id, item)
        else entries = entries.concat([item])
        return id
    }
    function target(e) {
        if (e.kind === "team-start") return "Team start · once"
        if (e.target === 2) return "Team " + (e.teamName || "Website") + " → leader"
        if (e.target === 1) return "New conversation each run · " + (({ claude: "Claude", codex: "Codex", cursor: "Cursor" })[e.provider] || "Codex")
        return "#" + e.conversation + (e.creator.startsWith("Agent") ? " · agent-created" : "")
    }
    function forTeam(id) { return entries.filter(e => e.target === 2 && e.teamId === id) }
    function clockFor(number) { return entries.find(e => e.enabled && e.target === 0 && e.conversation === number) }
}
