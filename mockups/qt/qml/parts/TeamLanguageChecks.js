// SPDX-License-Identifier: MPL-2.0
// Exercise actual QML text bindings, not only the translation helper's return values.
function hasText(item, expected, contains) {
    if (!item) return false
    if (item.text === expected || (contains && typeof item.text === "string" && item.text.indexOf(expected) >= 0))
        return true
    const children = item.children || []
    for (let i = 0; i < children.length; ++i)
        if (hasText(children[i], expected, contains))
            return true
    return false
}

function run(screen, completed) {
    const failures = []
    const check = (ok, message) => { if (!ok) failures.push(message) }
    const saved = { teams: screen.teams, index: screen.teamIndex, filter: screen.filter }
    let authoredTitles = ""
    const stages = [
        () => screen.showState(""),
        () => {
            authoredTitles = JSON.stringify(screen.tasks.map(task => task.title))
            for (const text of ["Waiting for you", "Members", "Updates", "Add member"])
                check(hasText(screen, text), "English team chrome: " + text)
            screen.change({ language: "vi" })
        },
        () => {
            for (const text of ["Đang chờ bạn", "Thành viên", "Cập nhật", "Thêm thành viên"])
                check(hasText(screen, text), "Vietnamese team chrome: " + text)
            check(hasText(screen, "Lịch") && !hasText(screen, "Schedules"), "Embedded team schedules follow Vietnamese")
            check(!hasText(screen, "Show more"), "Goal expansion follows Vietnamese")
            check(!hasText(screen, "Waiting for you"), "Vietnamese heading kept literal English")
            check(hasText(screen, "(Đề xuất)", true) || hasText(screen, "Đề xuất"),
                  "Rendered decision recommendation follows Vietnamese")
            check(screen.states.todo.label === "Cần làm" && screen.states.doing.label === "Đang làm"
                  && screen.states.review.label === "Đang review" && screen.states.done.label === "Xong",
                  "Task board state labels follow Vietnamese")
            check(screen.budgetDecision.question.indexOf("Team đã dùng hết") === 0,
                  "Generated budget decision follows Vietnamese")
            check(screen.budgetDecision.options[1] === "Dừng", "Budget decision action follows Vietnamese")
            check(JSON.stringify(screen.tasks.map(task => task.title)) === authoredTitles,
                  "Changing chrome does not translate authored task titles")
            screen.showState("vi-paused")
        },
        () => {
            check(hasText(screen, "Team đã dùng hết " + screen.turnBudget + " lượt tự động. Tiếp tục?"),
                  "Paused team's budget question renders in Vietnamese")
            check(hasText(screen, "Dừng"), "Paused team's Stop choice renders in Vietnamese")
            screen.change({ language: "en" })
        },
        () => {
            check(hasText(screen, "Waiting for you") && !hasText(screen, "Đang chờ bạn"),
                  "Switching back restores English bindings")
            check(screen.states.todo.label === "To do", "English board wording stays unchanged")
        }
    ]
    let stage = 0
    function restore() {
        screen.teams = saved.teams
        screen.teamIndex = saved.index
        screen.filter = saved.filter
        Qt.callLater(() => completed(failures))
    }
    function next() {
        try {
            if (stage === stages.length) { restore(); return }
            stages[stage++]()
            // A user's next action happens after bindings/model delegates have updated.
            Qt.callLater(next)
        } catch (error) {
            failures.push("Language check exception: " + error)
            restore()
        }
    }
    next()
}
