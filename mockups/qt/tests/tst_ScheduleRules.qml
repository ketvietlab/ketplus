// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtTest
import "../qml/schedules"
import "../qml/schedules/ScheduleRules.js" as Rules

TestCase {
    name: "ScheduleRules"
    ScheduleStore { id: store }
    function draft(changes) {
        return Object.assign({ name: "Write article", prompt: "Prepare draft", target: 0, conversation: "8116", rule: 1,
            date: "2026-11-14", time: "08:00", zone: "Asia/Ho_Chi_Minh", hours: "4", days: [1,2,3,4,5] }, changes || {})
    }
    function init() { store.reset() }
    function test_start_boundaries() {
        compare(Rules.startError("2026-11-13", "15:01", "Asia/Ho_Chi_Minh"), "")
        verify(Rules.startError("2026-11-13", "15:00", "Asia/Ho_Chi_Minh") !== "")
        compare(Rules.startError("2026-12-13", "15:00", "Asia/Ho_Chi_Minh"), "")
        verify(Rules.startError("2026-12-13", "15:01", "Asia/Ho_Chi_Minh") !== "")
        verify(Rules.startError("2026-02-30", "22:00", "Asia/Ho_Chi_Minh") !== "")
        verify(Rules.startError("2026-11-14", "25:00", "Asia/Ho_Chi_Minh") !== "")
        compare(Rules.startError("2026-11-13", "08:01", "UTC"), "")
    }
    function test_required_prompt_and_weekly_days() {
        verify(Rules.error(draft({ prompt: "   " })).includes("Prompt"))
        verify(Rules.error(draft({ name: " " })).includes("Name"))
        verify(Rules.error(draft({ rule: 3, days: [] })).includes("weekday"))
        verify(Rules.error(draft({ rule: 4, hours: "0" })).includes("hours"))
        verify(Rules.error(draft({ rule: 4, hours: "1.5" })).includes("hours"))
        compare(Rules.error(draft({ rule: 4, hours: "168" })), "")
    }
    function test_live_summary_and_next_run() {
        compare(Rules.nextLabel(draft()), "tomorrow 08:00")
        compare(Rules.nextLabel(draft({ rule: 2 })), "16/11 08:00")
        compare(Rules.nextLabel(draft({ rule: 3, days: [5], time: "17:00" })), "today 17:00")
        compare(Rules.nextLabel(draft({ rule: 4, hours: "2" })), "today 17:00")
        compare(Rules.nextLabel(draft({ zone: "UTC", time: "09:00" })), "today 09:00")
        verify(Rules.liveSummary(draft()).includes("GMT+7"))
        verify(Rules.summary(draft({ rule: 3, days: [1,5] })).includes("Mon, Fri"))
    }
    function test_store_create_edit_toggle_delete_and_team_filter() {
        const count = store.entries.length
        const id = store.save(draft())
        verify(id !== ""); compare(store.entries.length, count + 1)
        compare(store.clockFor("8116").conversation, "8116")
        const entry = Object.assign({}, store.find(id), { name: "Renamed", target: 2, teamId: "docs" })
        compare(store.save(entry), id); compare(store.entries.length, count + 1)
        compare(store.forTeam("docs").length, 1)
        store.patch(id, { enabled: false }); verify(!store.find(id).enabled)
        store.remove(id); compare(store.find(id), null)
        compare(store.entries.length, count)
    }
    function test_invalid_save_preserves_store() {
        const count = store.entries.length
        compare(store.save(draft({ prompt: "" })), "")
        compare(store.entries.length, count)
    }
}
