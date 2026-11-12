// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

// Teams (concept): several conversations with one goal. The page answers, in order, what waits
// for the user, who is on the team and what each member holds, and where every task stands.
// A leader (who also writes the requirements) assigns tasks; a developer owns a lane of the
// code; a reviewer checks the lanes it is given. Every change here is local to the mock.
Item {
    id: screen

    required property var window
    property var teams: Fixtures.teams
    property int teamIndex: 0
    readonly property var team: teams[teamIndex] || null
    // The Tasks filter: a task state, or "" for every task.
    property string filter: ""
    property var openTask: null
    property var handover: null

    readonly property var states: ({
        todo: { label: "To do", tone: KetTheme.BadgeTone.Neutral },
        doing: { label: "In progress", tone: KetTheme.BadgeTone.Accent },
        review: { label: "In review", tone: KetTheme.BadgeTone.Info },
        // Passed review with every criterion met; waits for whoever merges (#463).
        ready: { label: "Ready to merge", tone: KetTheme.BadgeTone.Success },
        blocked: { label: "Blocked", tone: KetTheme.BadgeTone.Warning },
        done: { label: "Done", tone: KetTheme.BadgeTone.Success }
    })
    readonly property var filters: ["", "doing", "review", "ready", "blocked", "done"]
    readonly property var roles: ({
        leader: { label: "Leader · Requirements", tone: KetTheme.BadgeTone.Accent },
        developer: { label: "Developer", tone: KetTheme.BadgeTone.Neutral },
        reviewer: { label: "Reviewer", tone: KetTheme.BadgeTone.Info }
    })
    readonly property var memberStates: ({
        working: { label: "Working", color: KetTheme.t.accentText },
        waiting: { label: "Waiting for review", color: KetTheme.t.textSecondary },
        blocked: { label: "Blocked", color: KetTheme.t.warning },
        idle: { label: "Idle", color: KetTheme.t.textMuted }
    })

    readonly property var members: team ? team.members : []
    readonly property var tasks: team ? team.tasks : []
    // T-37 (ADR 0114 §10): the disk that holds the team's worktrees, as the daemon measures it.
    // Below Fixtures.diskWarnGb it warns, below Fixtures.diskStopGb builds should stop; nothing
    // is paused or removed for the user.
    readonly property var storage: team && team.storage ? team.storage : null
    readonly property string diskLevel: !storage ? ""
        : storage.free < Fixtures.diskStopGb ? "stop" : storage.free < Fixtures.diskWarnGb ? "low" : ""
    readonly property var doneWorktrees: storage ? storage.worktrees : []
    // T-62: "Remove all…" is running, and what its last run did: { removed: [ids], kept: {id: reason} }.
    property bool removingAll: false
    property var removalOutcome: null
    function gb(value) { return value.toFixed(1) + " GB" }
    readonly property var decisions: team ? team.decisions : []
    readonly property var shownTasks: filter === "" ? tasks : tasks.filter(t => t.state === filter)
    readonly property var leader: members.find(m => m.role === "leader") || null
    readonly property int developers: members.filter(m => m.role === "developer").length
    readonly property int reviewers: members.filter(m => m.role === "reviewer").length
    readonly property int doneTasks: tasks.filter(t => t.state === "done").length
    // Past three developers one reviewer's reviews queue; a second reviewer shortens it (T-23).
    readonly property bool reviewQueue: developers >= 4 && reviewers < Fixtures.mostReviewers
    // A team has one to Fixtures.mostDevelopers developers.
    readonly property bool full: developers >= Fixtures.mostDevelopers
    // New teams count on from here, so a deleted team's id is never given again.
    property int teamsCreated: 0
    // Hides the secondary columns when the page is narrow.
    readonly property bool wide: content.width >= px(860)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function reference(number) { return number ? "@agent/" + number : "Unassigned" }
    function member(number) { return members.find(m => m.number === number) || null }
    function count(n, one, many) { return n + " " + (n === 1 ? one : many) }
    // T-23: what a reviewer reviews now and what waits for it.
    function reviewingText(reviewer) {
        return reviewer.reviewing ? "Reviewing " + reviewer.reviewing
             : (reviewer.reviews || []).length > 0 ? "Reviews " + reviewer.reviews.join(", ") : "Reviews every lane"
    }
    function queueText(reviewer) {
        const queue = reviewer.queue || []
        return queue.length === 0 ? "Nothing queued" : "Queued: " + queue.join(", ")
    }

    // Replaces the current team with a changed copy, so every binding follows.
    function change(patch) {
        const next = teams.slice()
        next[teamIndex] = Object.assign({}, team, patch)
        teams = next
    }
    function changeTask(id, patch, event) {
        change({ tasks: tasks.map(t => t.id !== id ? t : Object.assign({}, t, patch, {
            history: event ? t.history.concat([{ when: "Just now", text: event }]) : t.history })) })
        if (openTask && openTask.id === id)
            openTask = tasks.find(t => t.id === id)
    }

    // `teams/empty`: no team yet. `teams/large`: a fourth developer, so reviews queue;
    // `teams/two-reviewers`: the same team with a second reviewer, and no queue notice.
    // `teams/ready`: the user merges, and T-1 passed review: "Merge T-1?" waits for the user.
    // `teams/paused`: the team used its automatic turns and waits for the user to go on.
    function showState(state) {
        teams = state === "empty" ? [] : Fixtures.teams
        // `teams/live`: content as a team writes it: long choices, descriptions, lanes, updates.
        if (state === "live")
            teams = [Fixtures.liveTeam]
        teamIndex = 0
        filter = ""
        if (state === "large" || state === "two-reviewers")
            change({ members: members.concat([{ number: "9079", title: "Check this worktree", role: "developer",
                provider: "Claude · Opus 5.5", lane: "Onboarding", paths: ["apps/desktop/src/onboarding/"],
                state: "idle", now: "No task yet" }]) })
        // `teams/two-reviewers` (T-23): the large team with a second reviewer; each reviews one
        // task of its own, so T-1 and T-6 are reviewed at the same time.
        if (state === "two-reviewers")
            change({ members: members.map(m => m.role !== "reviewer" ? m
                    : Object.assign({}, m, { reviewing: "T-1", queue: [] }))
                .concat([{ number: "4590", title: "Reviewer 2", role: "reviewer", provider: "Claude · Opus 5.5",
                    lane: "", reviews: [], paths: [], state: "working", now: "Reviewing T-6",
                    reviewing: "T-6", queue: [] }]) })
        if (state === "task")
            Qt.callLater(() => screen.showTask("T-3"))
        // `teams/goal-open`: the team's goal opened with Show more.
        if (state === "goal-open")
            Qt.callLater(() => { teamGoalText.expanded = true })
        if (state === "live-task")
            Qt.callLater(() => { screen.showState("live"); screen.showTask("T-1") })
        if (state === "ready")
            change({ merges: "user",
                tasks: tasks.map(t => t.id !== "T-1" ? t : Object.assign({}, t, { state: "ready",
                    history: t.history.concat([
                        { when: "Today, 12:20", text: "@agent/4583 passed round 1 of 3 with no findings" },
                        { when: "Today, 12:20", text: "Every criterion is met: Ready to merge" }]) })),
                decisions: [mergeDecision].concat(decisions) })
        // `teams/disk-low` and `teams/disk-stop` (T-37): the two levels of the disk warning.
        if (state === "disk-low" || state === "disk-stop")
            change({ storage: Object.assign({}, storage, { free: state === "disk-low" ? 18.4 : 9.6 }) })
        if (state === "paused")
            change({ paused: true, pausedReason: "budget", automaticTurns: turnBudget,
                decisions: [budgetDecision].concat(decisions),
                members: members.map(m => m.state === "working"
                    ? Object.assign({}, m, { state: "idle", now: "Waits: the team is paused" }) : m) })
    }
    // What the daemon raises itself (#463, #459): a merge under the user setting, and the budget.
    readonly property var mergeDecision: ({ id: "D-3", kind: "merge", question: "Merge T-1 into release/1.4?",
        detail: "The review passed with no findings and every criterion is met. You chose to merge each task yourself.",
        askedBy: "", task: "T-1", when: "Just now", options: ["Merge", "Not yet"], recommended: 0 })
    // The daemon words it with the team's own budget (ADR 0114, open point 2).
    readonly property int turnBudget: team && team.turnBudget ? team.turnBudget : Fixtures.turnBudget
    readonly property int automaticTurns: team && team.automaticTurns ? team.automaticTurns : 0
    readonly property var budgetDecision: ({ id: "D-4", kind: "budget",
        question: "The team used its " + turnBudget + " automatic turns. Continue?",
        detail: "No member starts a turn on its own until you answer. Messages wait and arrive in order.",
        askedBy: "", task: "", when: "Just now", options: ["Continue (" + turnBudget + " more turns)", "Stop"], recommended: 0 })
    // T-40: how the team reviews. The review passes per PR are one to five (two by default),
    // counting every reviewer's pass; the place is the board or the task's pull request. A
    // change applies from the next review request: a task already past a lowered limit is
    // blocked for the leader then, not at once.
    readonly property int reviewRounds: team && team.reviewRounds ? team.reviewRounds : 2
    readonly property string reviewPlace: team && team.reviewPlace ? team.reviewPlace : "board"
    function reviewText(rounds, place) {
        return (rounds === 1 ? "1 review pass" : rounds + " review passes")
            + (place === "pull_request" ? " on the pull request" : " on the board")
    }
    function changeReview(rounds, place) {
        if (rounds === reviewRounds && place === reviewPlace)
            return
        change({ reviewRounds: rounds, reviewPlace: place,
            updates: [{ icon: "eye", text: "Max review passes: " + rounds
                            + (place === "pull_request" ? ", on the pull request" : ", on the board")
                            + ", from the next review request",
                        detail: "You · the team", when: "Just now" }].concat(team.updates) })
        window.notify(team.name + ": " + reviewText(rounds, place).toLowerCase())
    }
    // A new budget applies from the next automatic turn; the turns used stay counted. One that
    // leaves turns withdraws the budget question and resumes a team it paused.
    function changeTurnBudget(budget) {
        const resumes = team.paused && team.pausedReason === "budget" && automaticTurns < budget
        change({ turnBudget: budget,
            paused: resumes ? false : team.paused,
            pausedReason: resumes ? "" : team.pausedReason,
            decisions: resumes ? decisions.filter(d => d.kind !== "budget") : decisions,
            updates: [{ icon: "clock", text: "Automatic turns: " + budget + ", " + automaticTurns + " used",
                        detail: "You · the team", when: "Just now" }].concat(team.updates) })
        window.notify(resumes ? "Resumed " + team.name + ": " + automaticTurns + " of " + budget + " turns used"
                              : team.name + " has " + budget + " automatic turns")
    }

    Timer {
        id: goalTyping
        interval: 400
        onTriggered: {
            teamGoal.input.insert(teamGoal.text.length, "\n\nAsk @agent/")
            teamGoal.cursorPosition = teamGoal.text.length
        }
    }

    // `--open=new-team|add-member|task|handover` opens a dialog for review captures.
    function openDialog(name) {
        if (name === "new-team")
            startNewTeam("")
        else if (name === "new-team-goal" || name === "new-team-unknown") {
            // A long goal with a mention drawn as a token, and the picker open on the next one;
            // `new-team-unknown` mentions a number no conversation has.
            startNewTeam("")
            teamName.text = "Onboarding rework"
            teamGoal.text = Fixtures.longTeamGoal + (name === "new-team-unknown" ? " Also @agent/9999." : "")
            teamGoal.cursorPosition = teamGoal.text.length
            teamGoal.focusInput()
            // Typed once the dialog has settled, as a user would: the picker opens below.
            if (name === "new-team-goal")
                goalTyping.start()
        }
        else if (name === "delete-team" && team)
            disbandDialog.open()
        else if (name === "turn-budget" && team)
            turnBudgetDialog.open()
        else if (name === "team-review" && team)
            reviewDialog.open()
        else if (name === "remove-member" && team)
            startRemove(team.id, "8215")
        else if (name === "add-member" && team)
            addMemberDialog.open()
        else if (name === "add-reviewer" && team) {
            addMemberDialog.chooseRole("reviewer")
            addMemberDialog.open()
        }
        else if (name === "member-agent" && team) {
            addMemberDialog.open()
            memberAgentDialog.target = ""
            memberAgentDialog.open()
        }
        else if (name === "team-created") {
            // What Create leaves: the team with its planned members.
            startNewTeam("")
            teamName.text = "Onboarding rework"
            teamGoal.text = "Make the first run explain itself."
            createButton.clicked()
        }
        else if (name === "team-agent") {
            startNewTeam("")
            memberAgentDialog.target = "developer"
            memberAgentDialog.open()
        }
        else if (name === "reviewer-agent") {
            // The reviewer's picker: Claude only.
            startNewTeam("")
            memberAgentDialog.target = "reviewer"
            memberAgentDialog.open()
        }
        else if (name === "task")
            showTask(tasks.some(t => t.id === "T-3") && team.id !== "pilot" ? "T-3" : tasks[0].id)
        else if (name === "handover")
            startHandover(tasks.find(t => t.id === "T-3"))
        // T-37: the confirmation of a done task's worktree removal, and the daemon's refusal of
        // one that has uncommitted changes, shown in the dialog after Remove.
        // `remove-worktree-note` is a removal after which a conversation could not be moved
        // out of the folder: the dialog stays and names it.
        else if ((name === "remove-worktree" || name === "remove-worktree-refused" || name === "remove-worktree-note") && doneWorktrees.length > 0) {
            startRemoveWorktree(doneWorktrees[0])
            if (name === "remove-worktree-refused")
                removeWorktreeDialog.refusal = Fixtures.dirtyWorktreeRefusal
            if (name === "remove-worktree-note")
                removeWorktreeDialog.note = Fixtures.unmovedConversationNote
        }
        // T-62: the confirmation of Remove all…, and the page after a run that kept one
        // worktree with git's reason.
        else if (name === "remove-all" && doneWorktrees.length > 0)
            removeAllDialog.open()
        else if (name === "remove-all-kept" && doneWorktrees.length > 0)
            removeAllWorktrees({ [doneWorktrees[0].id]: Fixtures.dirtyWorktreeRefusal })
    }
    function openEveryOverlay() {
        for (const dialog of [newTeamDialog, addMemberDialog, taskDialog, handoverDialog, disbandDialog, removeDialog, removeWorktreeDialog, removeAllDialog]) {
            if (dialog === taskDialog || dialog === handoverDialog)
                openTask = handover = Fixtures.teams[0].tasks[2]
            dialog.open()
            dialog.close()
        }
        openTask = handover = null
    }

    // The sidebar's entry points: a repository's New team, and a team folder's Add member,
    // Delete team and a member row's Remove from team.
    function startNewTeam(repository) {
        newTeamDialog.repository = repository || Fixtures.repositories[0].name
        newTeamDialog.open()
    }
    function startAddMember(id, number) {
        showTeam(id)
        if (!team)
            return
        if (!full)
            addMemberDialog.open()
    }
    function startDelete(id) {
        showTeam(id)
        if (team)
            disbandDialog.open()
    }
    function startRemove(id, number) {
        showTeam(id)
        const who = member(number)
        if (!who || keeps(who) !== "")
            return
        removeDialog.who = who
        removeDialog.open()
    }

    // T-37: the user removes a done task's worktree, only after confirming; the daemon runs
    // git worktree remove without force and prune, and the task keeps its branch.
    function startRemoveWorktree(entry) {
        removeWorktreeDialog.entry = entry
        removeWorktreeDialog.refusal = ""
        removeWorktreeDialog.note = ""
        removeWorktreeDialog.open()
    }
    function removeWorktree(id) {
        const entry = doneWorktrees.find(w => w.id === id)
        if (!entry)
            return false
        change({ storage: Object.assign({}, storage, { worktrees: doneWorktrees.filter(w => w.id !== id) }) })
        changeTask(id, { worktree: "" }, "You removed the worktree " + entry.path)
        // A summary that still says this one was kept is not true any more.
        if (!removingAll && removalOutcome && removalOutcome.kept[id] !== undefined)
            removalOutcome = null
        return true
    }

    // T-62: every listed worktree through the same removal, one after another. `refused` maps
    // a task id to the daemon's reason for keeping it, as a refusal would say it.
    function removeAllWorktrees(refused) {
        const outcome = { removed: [], kept: {} }
        for (const entry of doneWorktrees.slice()) {
            if (refused && refused[entry.id])
                outcome.kept[entry.id] = refused[entry.id]
            else if (removeWorktree(entry.id))
                outcome.removed.push(entry.id)
        }
        removalOutcome = outcome
        return outcome
    }
    function removalSummary(outcome) {
        if (!outcome)
            return ""
        const kept = Object.keys(outcome.kept).length
        const done = "Removed " + count(outcome.removed.length, "worktree", "worktrees") + "."
        return kept === 0 ? done : done + " Kept " + kept + (kept === 1 ? " with its reason." : " with their reasons.")
    }
    function totalSize(entries) {
        return entries.reduce((sum, e) => sum + parseFloat(e.size), 0).toFixed(1) + " GB"
    }

    // The sidebar's team folder opens its team.
    function showTeam(id) {
        const index = teams.findIndex(t => t.id === id)
        if (index < 0)
            return
        teamIndex = index
        filter = ""
    }

    function showTask(id) {
        const task = tasks.find(t => t.id === id)
        if (!task)
            return
        openTask = task
        taskDialog.open()
    }
    function startHandover(task) {
        if (!task)
            return
        handover = task
        handoverDialog.open()
    }

    // The user's answer goes to the leader and to the owner of the task it blocks.
    function decide(decision, option) {
        // The daemon's own decisions act on the team instead of going to a member.
        if (decision.kind === "merge" || decision.kind === "budget") {
            const yes = option === decision.options[0]
            change({
                decisions: decisions.filter(d => d.id !== decision.id),
                paused: decision.kind === "budget" ? !yes : team.paused,
                automaticTurns: decision.kind === "budget" && yes ? 0 : automaticTurns,
                pausedReason: decision.kind === "budget" && yes ? "" : team.pausedReason,
                tasks: tasks.map(t => t.id !== decision.task || !yes ? t : Object.assign({}, t, { state: "done",
                    history: t.history.concat([{ when: "Just now", text: "You merged " + t.id + " into " + team.branch }]) })),
                updates: [{ icon: "check", text: decision.id + " decided: " + option, detail: "You · the team", when: "Just now" }]
                    .concat(team.updates)
            })
            screen.window.notify(decision.kind === "merge" ? (yes ? "Merged " + decision.task : "Kept " + decision.task + " ready")
                                                           : (yes ? "Resumed " + team.name : "The team stays paused"))
            return
        }
        const task = tasks.find(t => t.id === decision.task)
        const blocked = task && task.state === "blocked"
        change({
            decisions: decisions.filter(d => d.id !== decision.id),
            tasks: tasks.map(t => t.id !== decision.task ? t : Object.assign({}, t, {
                state: blocked ? "doing" : t.state, decision: "",
                history: t.history.concat([{ when: "Just now", text: "You decided " + decision.id + ": " + option }]) })),
            members: members.map(m => blocked && m.number === task.owner
                ? Object.assign({}, m, { state: "working", now: task.id + " · resumed with your decision" }) : m),
            updates: [{ icon: "check", text: decision.id + " decided: " + option,
                        detail: "You · sent to " + reference(decision.askedBy)
                                + (task && task.owner && task.owner !== decision.askedBy ? " and " + reference(task.owner) : ""),
                        when: "Just now" }].concat(team.updates)
        })
        screen.window.notify("Sent to " + reference(decision.askedBy))
    }

    function openConversation(number) {
        const found = window.findConversation(number)
        if (found)
            window.openConversation(found.repository, found.conversation)
    }

    // Every change to a team goes through these, so the dialogs, the sidebar and --smoke's
    // checks (selfCheck) share the rules: one leader, one or two reviewers, one to
    // Fixtures.mostDevelopers developers, and a conversation on one team of its own repository.
    function developersOf(team) { return team.members.filter(m => m.role === "developer").length }
    function agentOf(planned) {
        const provider = Fixtures.providers.find(p => p.id === planned.provider)
        return { id: planned.provider, label: provider ? provider.label : planned.provider,
                 model: planned.model, effort: planned.effort }
    }
    // A member whose conversation starts now, in `repository`.
    function newMember(repository, role, name, agent, lane, paths) {
        const conversation = window.startConversation(repository, name, agent)
        return { number: conversation.number, title: name, role: role,
                 provider: agent.label + " · " + agent.model, lane: lane || "", paths: paths || [],
                 reviews: [], state: "idle",
                 now: role === "leader" ? "Waiting for the goal" : role === "reviewer" ? "No review yet"
                    : lane ? "No task yet" : "No lane yet" }
    }
    // `plan` is { leader, developer, reviewer }, each { count, provider, model, effort }.
    // `budget` is the team's own automatic turns; without one it takes Settings → Teams.
    // `review` is { rounds, place }; without one, two passes on the board.
    function createTeam(repository, name, goal, branch, leaderWrites, plan, merges, permission, budget, review) {
        const developers = Math.max(1, Math.min(Fixtures.mostDevelopers, plan.developer.count))
        const members = [newMember(repository, "leader", "Lead " + name, agentOf(plan.leader))]
        for (let i = 1; i <= developers; ++i)
            members.push(newMember(repository, "developer", "Developer " + i, agentOf(plan.developer)))
        const reviewers = Math.max(1, Math.min(Fixtures.mostReviewers, plan.reviewer.count))
        for (let i = 1; i <= reviewers; ++i)
            members.push(newMember(repository, "reviewer", reviewers === 1 ? "Reviewer" : "Reviewer " + i,
                                   agentOf(plan.reviewer)))
        let id = ""
        do {
            id = "team-" + (++teamsCreated)
        } while (teams.some(t => t.id === id))
        teams = teams.concat([{
            id: id, name: name, goal: goal || "No goal yet.", repository: repository, branch: branch,
            paused: false, merges: merges || "leader", permission: permission || "Workspace",
            turnBudget: budget || Fixtures.turnBudget, automaticTurns: 0,
            reviewRounds: review && review.rounds >= 1 && review.rounds <= 5 ? review.rounds : 2,
            reviewPlace: review && review.place === "pull_request" ? "pull_request" : "board",
            leaderWrites: leaderWrites, members: members,
            tasks: [], decisions: [], updates: [] }])
        teamIndex = teams.length - 1
        filter = ""
        window.notify("Created " + name + " and started " + members.length + " conversations")
        return id
    }
    // One more developer for the current team, as a new conversation.
    function addDeveloper(name, lane, paths, agent) {
        if (!team || full)
            return ""
        const member = newMember(team.repository, "developer", name, agent, lane, paths)
        change({ members: members.concat([member]) })
        window.notify("Added " + reference(member.number) + " as a developer")
        return member.number
    }
    // A second reviewer for the current team, as a new conversation (T-23): each task in
    // review then goes to one of the two.
    function addReviewer(agent, name) {
        if (!team || reviewers >= Fixtures.mostReviewers)
            return ""
        const member = newMember(team.repository, "reviewer", name || "Reviewer " + (reviewers + 1), agent)
        change({ members: members.concat([member]) })
        window.notify("Added " + reference(member.number) + " as a second reviewer")
        return member.number
    }
    // A conversation that exists joins from the sidebar (dropped on the folder, or from its
    // menu), as a developer with no lane yet.
    function join(id, number) {
        const index = teams.findIndex(t => t.id === id)
        if (index < 0 || teams.some(t => t.members.some(m => m.number === number)))
            return false
        const target = teams[index]
        const found = window.conversationsOf(target.repository).find(c => c.number === number)
        if (!found) {
            window.notify("Only a conversation of " + target.repository + " can join " + target.name)
            return false
        }
        if (developersOf(target) >= Fixtures.mostDevelopers) {
            window.notify(target.name + " already has " + Fixtures.mostDevelopers + " developers")
            return false
        }
        const next = teams.slice()
        next[index] = Object.assign({}, target, { members: target.members.concat([{
            number: found.number, title: found.title, role: "developer",
            provider: (found.provider || "Claude") + " · " + (found.model || "Opus 5.5"),
            lane: "", paths: [], reviews: [], state: "idle", now: "No lane yet" }]) })
        teams = next
        window.notify("Added @agent/" + number + " to " + target.name + " as a developer")
        return true
    }
    // Why a member of the current team cannot be removed; "" when it can.
    function keeps(who) {
        return !who ? "" : who.role === "leader" ? "A team keeps its leader"
             : who.role === "reviewer" ? (reviewers <= 1 ? "A team keeps at least one reviewer: add another before removing this one" : "")
             : developers <= 1 ? "A team keeps at least one developer" : ""
    }
    // The conversation stays, among its repository's own; its open tasks become unassigned.
    function removeMember(number) {
        const who = member(number)
        if (!who || keeps(who) !== "")
            return false
        change({
            members: members.filter(m => m.number !== who.number)
                .map(m => m.role !== "reviewer" ? m : Object.assign({}, m, { reviews: (m.reviews || []).filter(l => l !== who.lane) })),
            tasks: tasks.map(t => t.owner !== who.number || t.state === "done" ? t : Object.assign({}, t, {
                owner: "", state: t.state === "review" ? "review" : "todo",
                history: t.history.concat([{ when: "Just now", text: "You removed " + reference(who.number) + " from the team; the task is unassigned" }]) }))
        })
        window.notify("Removed " + reference(who.number) + " from " + team.name)
        return true
    }
    // Its conversations stay, among their repository's own.
    function deleteTeam() {
        const name = team.name
        const next = teams.filter((t, i) => i !== teamIndex)
        teamIndex = 0
        teams = next
        window.notify("Deleted " + name)
    }

    // --smoke runs these after its pointer drag; each entry of the result is a broken rule.
    function selfCheck() {
        const failures = []
        const check = (ok, text) => { if (!ok) failures.push(text) }
        const repository = Fixtures.repositories[0].name
        const other = Fixtures.repositories[1].name
        const plan = { leader: { count: 1, provider: "claude", model: "Opus 5.5", effort: "High" },
                       developer: { count: 2, provider: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                       reviewer: { count: 1, provider: "claude", model: "Opus 5.5", effort: "High" } }
        const loose = number => !teams.some(t => t.members.some(m => m.number === number))
                                && !!window.findConversation(number)

        // A removed member and a deleted team leave their conversations.
        const a = createTeam(repository, "Check A", "", "develop", true, plan)
        const started = team.members.map(m => m.number)
        check(started.length === 4, "a new team has a leader, two developers and a reviewer")
        check(team.merges === "leader", "the leader merges a new team's tasks unless the user chose to")
        change({ merges: "user" })
        check(team.merges === "user" && team.members.length === 4, "who merges is a setting of the team")
        const added = addDeveloper("Check lane", "Check", ["check/"], agentOf(plan.developer))
        check(removeMember(added) && loose(added), "a removed developer stays as a conversation")
        // Ids stay unique after a deletion.
        const b = createTeam(repository, "Check B", "", "develop", true, plan)
        showTeam(a)
        deleteTeam()
        check(started.every(loose), "a deleted team's conversations stay")
        const c = createTeam(repository, "Check C", "", "develop", true, plan)
        check(teams.filter(t => t.id === c).length === 1 && b !== c, "team ids are unique after a deletion")
        // One to five developers.
        showTeam(c)
        for (let i = 0; i < Fixtures.mostDevelopers + 2; ++i)
            addDeveloper("Check " + i, "Check " + i, ["check/"], agentOf(plan.developer))
        check(developers === Fixtures.mostDevelopers, "a team has at most five developers")
        check(!join(c, "4822"), "a full team takes no dropped conversation")
        for (const m of members.slice())
            removeMember(m.number)
        check(developers === 1 && !!leader && reviewers === 1, "a team keeps its leader, its reviewer and one developer")
        // T-23: one or two reviewers; the last one stays.
        const second = addReviewer(agentOf(plan.reviewer))
        check(second !== "" && reviewers === 2, "a team takes a second reviewer")
        check(addReviewer(agentOf(plan.reviewer)) === "" && reviewers === 2, "a team has at most two reviewers")
        check(removeMember(second) && loose(second) && reviewers === 1, "one of two reviewers can be removed")
        check(!removeMember(members.find(m => m.role === "reviewer").number), "a team keeps its last reviewer")
        const pair = createTeam(repository, "Check F", "", "develop", true,
                                Object.assign({}, plan, { reviewer: Object.assign({}, plan.reviewer, { count: 2 }) }))
        check(reviewers === 2 && members.length === 5, "New team starts two reviewers when asked")
        showTeam(pair)
        deleteTeam()
        // A conversation joins only a team of its own repository.
        const far = createTeam(other, "Check D", "", "main", true, plan)
        check(!join(far, "4822"), "a conversation of another repository cannot join")
        // ADR 0114, open point 2: the global budget, a team's own, and a change while it runs.
        const global = Fixtures.turnBudget
        check(["4", "201", "7.5", "", "forty"].every(v => Fixtures.turnBudgetError(v) !== "")
              && ["5", "40", "200"].every(v => Fixtures.turnBudgetError(v) === ""),
              "automatic turns are a whole number from 5 to 200")
        const own = createTeam(repository, "Check E", "", "develop", true, plan, "leader", "", 12)
        check(turnBudget === 12, "a team created with its own budget keeps it")
        check(reviewRounds === 2 && reviewPlace === "board", "a new team reviews in two passes on the board")
        changeReview(1, "pull_request")
        check(reviewRounds === 1 && reviewPlace === "pull_request"
              && team.updates[0].text.indexOf("Max review passes: 1") === 0, "the team's review is a setting it changes")
        changeReview(5, "board")
        check(reviewRounds === 5 && team.updates[0].text.indexOf("Max review passes: 5") === 0,
              "a team allows up to five review passes per PR")
        Fixtures.turnBudget = 70
        const taken = createTeam(repository, "Check F", "", "develop", true, plan)
        check(turnBudget === 70, "a team created without a budget takes Settings › Teams")
        showTeam(own)
        check(turnBudget === 12, "changing Settings › Teams changes no existing team")
        Fixtures.turnBudget = global
        change({ paused: true, pausedReason: "budget", automaticTurns: 12,
                 decisions: [budgetDecision].concat(decisions) })
        check(team.paused && decisions.some(d => d.kind === "budget")
              && budgetDecision.options[0] === "Continue (12 more turns)", "the budget question uses the team's budget")
        changeTurnBudget(20)
        check(!team.paused && !decisions.some(d => d.kind === "budget") && automaticTurns === 12
              && turnBudget === 20, "a budget that leaves turns resumes the team and keeps the turns used")
        const reviewed = createTeam(repository, "Check G", "", "develop", true, plan, "leader", "", 0,
                                    { rounds: 4, place: "pull_request" })
        check(reviewRounds === 4 && reviewPlace === "pull_request", "New team chooses the passes and the place")
        for (const id of [b, c, far, own, taken, reviewed]) {
            showTeam(id)
            deleteTeam()
        }
        showTeam("release")
        // T-37: the disk warning's two levels, and a confirmed removal of a done worktree.
        const before = { storage: storage, tasks: tasks }
        for (const [value, level] of [[Fixtures.diskWarnGb, ""], [Fixtures.diskWarnGb - 0.1, "low"],
                                      [Fixtures.diskStopGb, "low"], [Fixtures.diskStopGb - 0.1, "stop"]]) {
            change({ storage: Object.assign({}, storage, { free: value }) })
            check(diskLevel === level, "the disk warning is " + (level || "off") + " at " + value + " GB")
        }
        change(before)
        const kept = doneWorktrees
        check(removeWorktree("T-2") && doneWorktrees.length === kept.length - 1
              && tasks.find(t => t.id === "T-2").worktree === "", "a removed worktree leaves the offer and the task")
        check(!removeWorktree("T-2"), "a worktree is removed once")
        change(before)
        const all = doneWorktrees.length
        const outcome = removeAllWorktrees({})
        check(outcome.removed.length === all && doneWorktrees.length === 0, "Remove all removes every listed worktree")
        change(before)
        const mixed = removeAllWorktrees({ "T-2": Fixtures.dirtyWorktreeRefusal })
        check(mixed.kept["T-2"] === Fixtures.dirtyWorktreeRefusal && doneWorktrees.some(w => w.id === "T-2"),
              "a refused worktree stays listed with its reason")
        removeWorktree("T-2")
        check(removalOutcome === null, "removing a kept worktree by hand drops the old summary")
        change(before)
        removeAllWorktrees({})
        check(removalOutcome !== null && doneWorktrees.length === 0,
              "the summary stays after every worktree was removed")
        removalOutcome = null
        change(before)
        return failures
    }

    // A dialog's fields: they scroll when the window is too low for all of them, and the
    // dialog's actions stay in view.
    component DialogBody: Flickable {
        id: dialogBody
        default property alias fields: bodyFields.data
        readonly property bool scrolls: contentHeight > height + 1
        Layout.fillWidth: true
        Layout.fillHeight: true
        implicitHeight: bodyFields.height
        contentHeight: bodyFields.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Basic.ScrollBar.vertical: Basic.ScrollBar {
            policy: dialogBody.scrolls ? Basic.ScrollBar.AlwaysOn : Basic.ScrollBar.AlwaysOff
        }
        Column {
            id: bodyFields
            width: dialogBody.width - (dialogBody.scrolls ? screen.px(14) : 0)
            spacing: screen.px(14)
        }
    }
    component Label: Text {
        color: KetTheme.t.textMain
        font.family: KetTheme.fontFamily
        font.pixelSize: screen.px(13)
        elide: Text.ElideRight
    }
    component Mono: Label {
        font.family: KetTheme.monoFontFamily
        font.pixelSize: screen.px(12)
        font.weight: Font.Medium
        color: KetTheme.t.textSecondary
    }
    component SectionTitle: Item {
        property string title
        property string count
        default property alias trailing: trailingRow.data
        width: parent.width
        height: screen.px(28)
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: screen.px(10)
            Label { anchors.verticalCenter: parent.verticalCenter; text: title; font.pixelSize: screen.px(17); font.weight: Font.Medium }
            Label { anchors.verticalCenter: parent.verticalCenter; text: count; color: KetTheme.t.textSecondary }
        }
        Row { id: trailingRow; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: screen.px(8) }
    }
    component Panel: Rectangle {
        width: parent.width
        radius: screen.px(7)
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.panelBorder
    }
    // A fact of a task: a muted label over its value.
    component Fact: Column {
        property string label
        property string value
        property bool mono: false
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        spacing: screen.px(4)
        Label { width: parent.width; text: label; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
        Label {
            width: parent.width
            text: value || "–"
            font.family: mono && value ? KetTheme.monoFontFamily : KetTheme.fontFamily
            font.weight: mono && value ? Font.Medium : Font.Normal
        }
    }

    KetEmptyState {
        visible: !screen.team
        anchors.centerIn: parent
        width: Math.min(parent.width - screen.px(84), screen.px(520))
        iconName: "bot"
        title: "No team yet"
        message: "A team gives several conversations one goal. A leader assigns the tasks, each developer owns a part of the code, and a reviewer checks their pull requests."
        actionText: "Create a team"
        actionIcon: "plus"
        onActionTriggered: screen.startNewTeam("")
    }

    Basic.ScrollView {
        id: scroll
        visible: !!screen.team
        anchors.fill: parent
        contentWidth: availableWidth
        contentHeight: content.y + content.height
        // --scroll: start part of the way down, for review captures.
        Timer {
            running: Mock.initialScroll >= 0 && Mock.initialScreen === "teams"
            interval: 250
            onTriggered: scroll.contentItem.contentY = Mock.initialScroll
                         * Math.max(0, scroll.contentItem.contentHeight - scroll.height)
        }

        Column {
            id: content
            x: Math.max(screen.px(42), (scroll.availableWidth - width) / 2)
            y: screen.px(38)
            width: Math.min(scroll.availableWidth - screen.px(84), screen.px(1120))
            spacing: screen.px(18)
            bottomPadding: screen.px(40)

            PageHeading {
                width: parent.width
                eyebrow: screen.team && screen.team.paused ? "TEAM · PAUSED" : "TEAM"
                title: screen.team ? screen.team.name : ""
                KetButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Add member"
                    iconName: "plus"
                    enabled: !screen.full
                    onClicked: addMemberDialog.open()
                }
                KetIconButton {
                    id: moreButton
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "more"
                    text: "Team actions"
                    onClicked: teamMenu.popup(moreButton, moreButton.width - teamMenu.width, moreButton.height + screen.px(4))
                }
            }

            // The goal whole, folded after three lines (ADR 0114); a mention opens its
            // conversation. Everywhere else the goal keeps one line.
            KetExpandableText {
                id: teamGoalText
                objectName: "teamPageGoal"
                width: parent.width
                visible: text !== ""
                text: screen.team ? screen.team.goal : ""
                foldedLines: 3
                mentionTitles: {
                    const titles = {}
                    for (const repository of Fixtures.repositories)
                        for (const conversation of repository.conversations || [])
                            titles[conversation.number] = conversation.title
                    return titles
                }
                onMentionActivated: number => screen.openConversation(number)
            }

            // Where the team works, and how far it is.
            Row {
                spacing: screen.px(18)
                Row {
                    spacing: screen.px(6)
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "folder"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Label { text: screen.team ? screen.team.repository : ""; color: KetTheme.t.textSecondary }
                }
                Row {
                    spacing: screen.px(6)
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "branch"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Mono { anchors.verticalCenter: parent.verticalCenter; text: screen.team ? screen.team.branch : "" }
                }
                Row {
                    spacing: screen.px(6)
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "pull-request"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Label { text: screen.team && screen.team.merges === "user" ? "You merge tasks" : "The leader merges tasks"; color: KetTheme.t.textSecondary }
                }
                Row {
                    spacing: screen.px(6)
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "eye"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Label {
                        objectName: "teamReview"
                        text: screen.reviewText(screen.reviewRounds, screen.reviewPlace)
                        color: KetTheme.t.textSecondary
                    }
                }
                Row {
                    spacing: screen.px(6)
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "clock"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Label {
                        objectName: "teamTurns"
                        text: screen.automaticTurns + " of " + screen.turnBudget + " automatic turns used"
                        color: KetTheme.t.textSecondary
                    }
                }
                Label {
                    visible: screen.tasks.length > 0
                    text: screen.doneTasks + " of " + screen.count(screen.tasks.length, "task", "tasks") + " done"
                    color: KetTheme.t.textSecondary
                }
            }

            KetBanner {
                objectName: "teamPausedBanner"
                width: parent.width
                visible: !!screen.team && screen.team.paused
                tone: KetTheme.BannerTone.Warning
                title: screen.team && screen.team.pausedReason === "budget"
                       ? "Paused: the team used its automatic turns" : "Paused"
                message: "No member starts a turn on its own. Messages to members wait and arrive in order when you resume."
                actionText: "Resume"
                onActionTriggered: {
                    screen.change({ paused: false, pausedReason: "",
                        decisions: screen.decisions.filter(d => d.kind !== "budget") })
                    screen.window.notify("Resumed " + screen.team.name)
                }
            }

            KetBanner {
                width: parent.width
                visible: screen.reviewQueue
                tone: KetTheme.BannerTone.Info
                title: screen.count(screen.developers, "developer", "developers") + " share one reviewer"
                message: "Reviews will queue. A reviewer keeps up with two or three developers; a second reviewer shortens the queue."
                actionText: "Add a second reviewer"
                onActionTriggered: screen.openDialog("add-reviewer")
            }

            // T-37: the free space of the disk under the team's worktrees, at two levels.
            KetBanner {
                objectName: "teamDiskBanner"
                width: parent.width
                visible: screen.diskLevel !== ""
                tone: screen.diskLevel === "stop" ? KetTheme.BannerTone.Danger : KetTheme.BannerTone.Warning
                title: screen.diskLevel === "stop"
                       ? "Builds should stop: " + screen.gb(screen.storage.free) + " free"
                       : screen.diskLevel === "low" ? "Disk space is low: " + screen.gb(screen.storage.free) + " free" : ""
                message: screen.diskLevel === "stop"
                         ? "The disk that holds this team's worktrees has less than " + Fixtures.diskStopGb + " GB free, so a build may fill it. Free space before the next build; nothing is paused or removed for you."
                         : "The disk that holds this team's worktrees has less than " + Fixtures.diskWarnGb + " GB free. Remove the worktrees of done tasks below, or free space, before builds fill it."
            }

            Item { width: 1; height: screen.px(4) }

            // What only the user can answer. An answer goes to the leader and unblocks its task.
            SectionTitle {
                title: "Waiting for you"
                count: screen.decisions.length > 0 ? screen.count(screen.decisions.length, "decision", "decisions") : ""
            }
            Panel {
                visible: screen.decisions.length === 0
                height: screen.px(56)
                Row {
                    x: screen.px(18)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: screen.px(10)
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "check"; box: screen.px(14); color: KetTheme.t.success }
                    Label { text: "Nothing waits for you. The leader asks here when a choice changes the product."; color: KetTheme.t.textSecondary }
                }
            }
            Repeater {
                model: screen.decisions
                Panel {
                    id: card
                    required property var modelData
                    readonly property var task: screen.tasks.find(t => t.id === modelData.task) || null
                    // Past this a choice is a sentence, not a button's label.
                    readonly property bool sentences: modelData.options.some(o => o.length > 28)
                    height: cardBody.implicitHeight + screen.px(32)
                    border.color: KetTheme.t.accentBorder
                    Column {
                        id: cardBody
                        x: screen.px(18)
                        y: screen.px(16)
                        width: parent.width - 2 * x
                        spacing: screen.px(6)
                        Label { width: parent.width; text: card.modelData.question; font.pixelSize: screen.px(15); font.weight: Font.Medium; wrapMode: Text.Wrap; elide: Text.ElideNone }
                        Label { width: parent.width; text: card.modelData.detail; color: KetTheme.t.textSecondary; wrapMode: Text.Wrap; elide: Text.ElideNone; lineHeight: 1.35 }
                        Item { width: 1; height: screen.px(2) }
                        // A choice written as a sentence is a row of its own: the whole sentence
                        // reads, and the card keeps its width. Short choices stay buttons.
                        Column {
                            visible: card.sentences
                            width: parent.width
                            spacing: screen.px(6)
                            Repeater {
                                model: card.sentences ? card.modelData.options : []
                                Basic.AbstractButton {
                                    id: choiceRow
                                    required property string modelData
                                    required property int index
                                    readonly property bool best: index === card.modelData.recommended
                                    width: parent.width
                                    height: choiceLine.implicitHeight + screen.px(20)
                                    hoverEnabled: true
                                    Accessible.name: modelData + (best ? ", recommended" : "")
                                    onClicked: screen.decide(card.modelData, modelData)
                                    background: Rectangle {
                                        radius: screen.px(6)
                                        color: choiceRow.down ? KetTheme.t.surfaceInset
                                             : choiceRow.hovered ? KetTheme.t.surfaceHover
                                             : choiceRow.best ? KetTheme.t.accentSubtle : "transparent"
                                        border.color: choiceRow.visualFocus ? KetTheme.t.focusBorder
                                                    : choiceRow.best ? KetTheme.t.accentBorder : KetTheme.t.panelBorder
                                        border.width: choiceRow.visualFocus ? 2 : 1
                                    }
                                    contentItem: Item {
                                        RowLayout {
                                            id: choiceLine
                                            x: screen.px(14)
                                            width: parent.width - screen.px(26)
                                            anchors.verticalCenter: parent.verticalCenter
                                            spacing: screen.px(12)
                                            Label {
                                                Layout.fillWidth: true
                                                text: choiceRow.modelData
                                                wrapMode: Text.Wrap
                                                elide: Text.ElideNone
                                                lineHeight: 1.3
                                                font.weight: choiceRow.best ? Font.Medium : Font.Normal
                                            }
                                            KetBadge { visible: choiceRow.best; text: "Recommended"; tone: KetTheme.BadgeTone.Accent }
                                            KetIcon { name: "chevron-right"; box: screen.px(14); color: KetTheme.t.textMuted }
                                        }
                                    }
                                }
                            }
                        }
                        Item { visible: card.sentences; width: 1; height: screen.px(2) }
                        // The choices, and beside them who asked and what the answer unblocks.
                        RowLayout {
                            width: parent.width
                            spacing: screen.px(8)
                            Repeater {
                                model: card.sentences ? [] : card.modelData.options
                                KetButton {
                                    required property string modelData
                                    required property int index
                                    text: modelData + (index === card.modelData.recommended ? " (Recommended)" : "")
                                    variant: index === card.modelData.recommended ? KetTheme.ButtonVariant.Primary : KetTheme.ButtonVariant.Secondary
                                    onClicked: screen.decide(card.modelData, modelData)
                                }
                            }
                            KetButton {
                                text: "Answer in the conversation"
                                variant: KetTheme.ButtonVariant.Ghost
                                visible: !!card.modelData.askedBy
                                onClicked: screen.openConversation(card.modelData.askedBy)
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideLeft
                                text: card.modelData.id + " · " + (card.modelData.askedBy ? screen.reference(card.modelData.askedBy) : "The team") + " · " + card.modelData.when
                                      + (card.task ? " · " + (card.task.state === "blocked" ? "Blocks " : "For ") + card.task.id : "")
                                color: KetTheme.t.textMuted
                                font.pixelSize: screen.px(12)
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: screen.px(4) }

            SectionTitle {
                title: "Tasks"
                count: screen.tasks.length > 0 ? screen.doneTasks + " of " + screen.tasks.length + " done" : ""
                KetSegmentedControl {
                    visible: screen.tasks.length > 0
                    options: screen.filters.map(f => f === "" ? "All " + screen.tasks.length
                        : screen.states[f].label + " " + screen.tasks.filter(t => t.state === f).length)
                    currentIndex: screen.filters.indexOf(screen.filter)
                    onActivated: index => screen.filter = screen.filters[index]
                }
            }
            KetEmptyState {
                visible: screen.tasks.length === 0
                width: parent.width
                iconName: "view-list"
                title: "No task yet"
                message: "The leader breaks the goal into tasks, writes what done means for each, then assigns them. Open the leader to start."
                actionText: "Open the leader"
                onActionTriggered: screen.openConversation(screen.leader.number)
            }
            Panel {
                visible: screen.tasks.length > 0
                height: taskList.implicitHeight
                Column {
                    id: taskList
                    width: parent.width
                    RowLayout {
                        x: screen.px(18)
                        width: parent.width - screen.px(36)
                        height: screen.px(34)
                        spacing: screen.px(16)
                        Label { Layout.fillWidth: true; text: "Task"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                        Label { Layout.preferredWidth: screen.px(52); text: "Criteria"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                        Label { Layout.preferredWidth: screen.px(104); text: "Owner"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                        Label { visible: screen.wide; Layout.preferredWidth: screen.px(140); text: "Lane"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                        Label { Layout.preferredWidth: screen.px(96); text: "State"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                        Label { Layout.preferredWidth: screen.px(44); text: "PR"; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                    }
                    KetEmptyState {
                        visible: screen.shownTasks.length === 0
                        width: parent.width
                        iconName: "filter"
                        title: "No task is " + (screen.filter ? screen.states[screen.filter].label.toLowerCase() : "here")
                        actionText: "Show all tasks"
                        onActionTriggered: screen.filter = ""
                    }
                    Repeater {
                        model: screen.shownTasks
                        Basic.AbstractButton {
                            id: taskRow
                            required property var modelData
                            readonly property int met: modelData.criteria.filter(c => c.done).length
                            width: taskList.width
                            height: screen.px(48)
                            hoverEnabled: true
                            Accessible.name: modelData.id + " " + modelData.title + ", " + screen.states[modelData.state].label
                                             + ", " + screen.reference(modelData.owner)
                            onClicked: screen.showTask(modelData.id)
                            background: Rectangle {
                                color: taskRow.hovered || taskRow.visualFocus ? KetTheme.t.surfaceHover : "transparent"
                                Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                                Rectangle {
                                    visible: taskRow.visualFocus
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    color: "transparent"
                                    border.color: KetTheme.t.focusBorder
                                    border.width: 2
                                    radius: screen.px(4)
                                }
                            }
                            contentItem: RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: screen.px(18)
                                anchors.rightMargin: screen.px(18)
                                spacing: screen.px(16)
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: screen.px(10)
                                    Mono { text: taskRow.modelData.id; Layout.preferredWidth: screen.px(28) }
                                    Label { Layout.fillWidth: true; text: taskRow.modelData.title; font.pixelSize: screen.px(14) }
                                    // Not colour alone: the icon and its label say what happened.
                                    Row {
                                        visible: !!taskRow.modelData.outsideLane
                                        spacing: screen.px(5)
                                        KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "warning"; box: screen.px(13); color: KetTheme.t.warning }
                                        Label { text: "Outside its lane"; color: KetTheme.t.warning; font.pixelSize: screen.px(12); font.weight: Font.Medium }
                                    }
                                }
                                Label {
                                    Layout.preferredWidth: screen.px(52)
                                    text: taskRow.met + "/" + taskRow.modelData.criteria.length
                                    color: KetTheme.t.textSecondary
                                }
                                Mono {
                                    Layout.preferredWidth: screen.px(104)
                                    text: screen.reference(taskRow.modelData.owner)
                                    font.family: taskRow.modelData.owner ? KetTheme.monoFontFamily : KetTheme.fontFamily
                                    color: taskRow.modelData.owner ? KetTheme.t.textSecondary : KetTheme.t.textMuted
                                }
                                Label {
                                    visible: screen.wide
                                    Layout.preferredWidth: screen.px(140)
                                    text: taskRow.modelData.lane || "Whole epic"
                                    color: KetTheme.t.textSecondary
                                }
                                Item {
                                    Layout.preferredWidth: screen.px(96)
                                    Layout.fillHeight: true
                                    KetBadge {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: screen.states[taskRow.modelData.state].label
                                        tone: screen.states[taskRow.modelData.state].tone
                                    }
                                }
                                Mono { Layout.preferredWidth: screen.px(44); text: taskRow.modelData.pr || "–" }
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: screen.px(4); visible: screen.doneWorktrees.length > 0 || screen.removalOutcome !== null }

            // T-37: done tasks whose worktree is still on disk, each offered to the user.
            // T-62: it stays for the summary after "Remove all…" removed every one.
            SectionTitle {
                visible: screen.doneWorktrees.length > 0 || screen.removalOutcome !== null
                title: "Worktrees of done tasks"
                count: screen.doneWorktrees.length > 0
                       ? screen.count(screen.doneWorktrees.length, "worktree", "worktrees") + " on disk"
                       : "None left on disk"
                // T-62: one confirmation for every listed worktree.
                KetButton {
                    objectName: "teamDoneWorktreesRemoveAll"
                    visible: screen.doneWorktrees.length > 0
                    text: "Remove all…"
                    iconName: "trash"
                    variant: KetTheme.ButtonVariant.Ghost
                    enabled: !screen.removingAll
                    onClicked: removeAllDialog.open()
                }
            }
            KetBanner {
                objectName: "teamDoneWorktreesSummary"
                width: parent.width
                visible: screen.removalOutcome !== null
                tone: screen.removalOutcome && Object.keys(screen.removalOutcome.kept).length > 0
                      ? KetTheme.BannerTone.Warning : KetTheme.BannerTone.Info
                title: screen.removalSummary(screen.removalOutcome)
            }
            Panel {
                objectName: "teamDoneWorktrees"
                visible: screen.doneWorktrees.length > 0
                height: doneWorktreeRows.height
                Column {
                    id: doneWorktreeRows
                    width: parent.width
                    Repeater {
                        model: screen.doneWorktrees
                        delegate: Item {
                            id: worktreeRow
                            required property var modelData
                            required property int index
                            width: doneWorktreeRows.width
                            height: screen.px(56)
                            Rectangle {
                                visible: worktreeRow.index > 0
                                width: parent.width; height: 1
                                color: KetTheme.t.panelBorder
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: screen.px(18)
                                anchors.rightMargin: screen.px(12)
                                spacing: screen.px(14)
                                Mono { Layout.preferredWidth: screen.px(36); text: worktreeRow.modelData.id }
                                Column {
                                    Layout.fillWidth: true
                                    spacing: screen.px(3)
                                    Label {
                                        width: parent.width
                                        text: (screen.tasks.find(t => t.id === worktreeRow.modelData.id) || { title: "" }).title
                                    }
                                    Mono {
                                        width: parent.width
                                        text: worktreeRow.modelData.path
                                        elide: Text.ElideMiddle
                                        color: KetTheme.t.textMuted
                                    }
                                    // T-62: why the last Remove all… kept it.
                                    Label {
                                        width: parent.width
                                        visible: text !== ""
                                        text: screen.removalOutcome ? (screen.removalOutcome.kept[worktreeRow.modelData.id] || "") : ""
                                        color: KetTheme.t.danger
                                        font.pixelSize: screen.px(12)
                                        elide: Text.ElideRight
                                    }
                                }
                                Label { text: worktreeRow.modelData.size; color: KetTheme.t.textSecondary }
                                KetButton {
                                    text: "Remove…"
                                    iconName: "trash"
                                    variant: KetTheme.ButtonVariant.Ghost
                                    enabled: !screen.removingAll
                                    onClicked: screen.startRemoveWorktree(worktreeRow.modelData)
                                }
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: screen.px(4) }

            SectionTitle {
                title: "Members"
                count: "1 leader · " + screen.count(screen.developers, "developer", "developers")
                       + (screen.full ? " (the most a team has)" : "") + " · "
                       + screen.count(screen.reviewers, "reviewer", "reviewers")
            }
            Panel {
                height: memberList.implicitHeight
                Column {
                    id: memberList
                    width: parent.width
                    Repeater {
                        model: screen.members
                        Item {
                            id: memberRow
                            required property var modelData
                            required property int index
                            readonly property var status: screen.memberStates[modelData.state] || screen.memberStates.idle
                            width: memberList.width
                            height: screen.px(64)
                            Rectangle { visible: memberRow.index > 0; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: screen.px(18)
                                anchors.rightMargin: screen.px(12)
                                spacing: screen.px(16)
                                Column {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: screen.px(150)
                                    Layout.preferredWidth: screen.px(190)
                                    spacing: screen.px(5)
                                    // A narrow page has no role column: the role sits beside the name.
                                    Row {
                                        width: parent.width
                                        spacing: screen.px(8)
                                        Label {
                                            width: Math.min(implicitWidth, parent.width - (nameRole.visible ? nameRole.width + parent.spacing : 0))
                                            text: memberRow.modelData.title
                                            font.pixelSize: screen.px(14)
                                            font.weight: Font.Medium
                                        }
                                        KetBadge {
                                            id: nameRole
                                            visible: !screen.wide
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: screen.roles[memberRow.modelData.role].label.split(" · ")[0]
                                            tone: screen.roles[memberRow.modelData.role].tone
                                        }
                                    }
                                    Row {
                                        width: parent.width
                                        spacing: screen.px(8)
                                        Mono { id: memberNumber; text: screen.reference(memberRow.modelData.number) }
                                        Label {
                                            width: parent.width - memberNumber.width - parent.spacing
                                            text: memberRow.modelData.provider
                                            color: KetTheme.t.textMuted
                                            font.pixelSize: screen.px(12)
                                        }
                                    }
                                }
                                Item {
                                    visible: screen.wide
                                    Layout.preferredWidth: screen.px(158)
                                    Layout.fillHeight: true
                                    KetBadge {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: screen.roles[memberRow.modelData.role].label
                                        tone: screen.roles[memberRow.modelData.role].tone
                                    }
                                }
                                // A developer's lane and its paths; what a reviewer reviews now
                                // and its queue (T-23: each of two reviewers has its own).
                                Column {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: screen.px(340)
                                    spacing: screen.px(5)
                                    Label {
                                        width: parent.width
                                        text: memberRow.modelData.role === "developer" ? memberRow.modelData.lane || "No lane yet"
                                            : memberRow.modelData.role === "reviewer" ? screen.reviewingText(memberRow.modelData)
                                            : "Assigns tasks, orders the merges"
                                        color: memberRow.modelData.role === "developer" ? KetTheme.t.textMain : KetTheme.t.textSecondary
                                    }
                                    Mono {
                                        objectName: "teamReviewerQueue"
                                        visible: memberRow.modelData.role === "reviewer"
                                        width: parent.width
                                        text: screen.queueText(memberRow.modelData)
                                        color: KetTheme.t.textMuted
                                        elide: Text.ElideRight
                                    }
                                    Mono {
                                        visible: memberRow.modelData.paths.length > 0
                                        width: parent.width
                                        text: memberRow.modelData.paths[0] + (memberRow.modelData.paths.length > 1
                                              ? "  +" + (memberRow.modelData.paths.length - 1) : "")
                                        color: KetTheme.t.textMuted
                                        elide: Text.ElideMiddle
                                    }
                                }
                                Column {
                                    visible: screen.wide
                                    Layout.preferredWidth: screen.px(220)
                                    spacing: screen.px(5)
                                    Label { width: parent.width; text: memberRow.status.label; color: memberRow.status.color; font.weight: Font.Medium }
                                    Label { width: parent.width; text: memberRow.modelData.now; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                                }
                                KetButton {
                                    text: "Open"
                                    variant: KetTheme.ButtonVariant.Ghost
                                    Accessible.name: "Open " + screen.reference(memberRow.modelData.number)
                                    onClicked: screen.openConversation(memberRow.modelData.number)
                                }
                                KetIconButton {
                                    id: memberMore
                                    iconName: "more"
                                    text: "Member actions"
                                    onClicked: {
                                        memberMenu.who = memberRow.modelData
                                        memberMenu.popup(memberMore, memberMore.width - memberMenu.width, memberMore.height + screen.px(4))
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: screen.px(4); visible: updatesPanel.visible }

            // Task changes wake nobody. The leader reads them as one digest when its turn ends.
            SectionTitle {
                visible: updatesPanel.visible
                title: "Updates"
                count: "Since the leader's last turn"
            }
            Panel {
                id: updatesPanel
                visible: !!screen.team && screen.team.updates.length > 0
                height: updateList.implicitHeight
                Column {
                    id: updateList
                    width: parent.width
                    Repeater {
                        model: screen.team ? screen.team.updates : []
                        Item {
                            id: updateRow
                            required property var modelData
                            required property int index
                            // A report is a paragraph: three lines, and the rest on request.
                            property bool expanded: false
                            width: updateList.width
                            height: Math.max(screen.px(52), updateLine.implicitHeight + screen.px(22))
                            Rectangle { visible: updateRow.index > 0; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                            RowLayout {
                                id: updateLine
                                x: screen.px(18)
                                width: parent.width - screen.px(36)
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: screen.px(12)
                                KetIcon {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: screen.px(2)
                                    name: updateRow.modelData.icon
                                    box: screen.px(14)
                                    color: updateRow.modelData.warning ? KetTheme.t.warning : KetTheme.t.textSecondary
                                }
                                Column {
                                    Layout.fillWidth: true
                                    spacing: screen.px(4)
                                    Label {
                                        id: updateText
                                        width: parent.width
                                        text: updateRow.modelData.text
                                        wrapMode: Text.Wrap
                                        lineHeight: 1.3
                                        maximumLineCount: updateRow.expanded ? 1000 : 3
                                    }
                                    Row {
                                        width: parent.width
                                        spacing: screen.px(10)
                                        Label {
                                            width: Math.min(implicitWidth, parent.width - (updateMore.visible ? updateMore.width + parent.spacing : 0))
                                            text: updateRow.modelData.detail
                                            color: KetTheme.t.textMuted
                                            font.pixelSize: screen.px(12)
                                            elide: Text.ElideMiddle
                                        }
                                        Basic.AbstractButton {
                                            id: updateMore
                                            visible: updateText.truncated || updateRow.expanded
                                            text: updateRow.expanded ? "Show less" : "Show more"
                                            Accessible.name: text
                                            onClicked: updateRow.expanded = !updateRow.expanded
                                            contentItem: Label {
                                                text: updateMore.text
                                                color: KetTheme.t.accentText
                                                font.pixelSize: screen.px(12)
                                                font.weight: Font.Medium
                                                font.underline: updateMore.hovered || updateMore.visualFocus
                                            }
                                            hoverEnabled: true
                                            padding: 0
                                        }
                                    }
                                }
                                Label {
                                    Layout.alignment: Qt.AlignTop
                                    text: updateRow.modelData.when
                                    color: KetTheme.t.textMuted
                                    font.pixelSize: screen.px(12)
                                }
                            }
                        }
                    }
                }
            }
            Row {
                visible: updatesPanel.visible
                spacing: screen.px(8)
                KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "info"; box: screen.px(13); color: KetTheme.t.textMuted }
                Label { text: "These woke nobody. The leader gets them as one digest when its turn ends."; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
            }
        }
    }

    KetMenu {
        id: teamMenu
        KetMenuItem {
            text: "Add member…"
            description: screen.full ? "A team has at most " + Fixtures.mostDevelopers + " developers" : ""
            iconName: "plus"
            enabled: !screen.full
            onTriggered: addMemberDialog.open()
        }
        KetMenuItem { text: "New team…"; iconName: "bot"; onTriggered: screen.startNewTeam(screen.team.repository) }
        KetMenuItem {
            text: screen.team && screen.team.paused ? "Resume automatic turns" : "Pause automatic turns"
            description: "Members finish their turn, then wait for you"
            iconName: screen.team && screen.team.paused ? "resume" : "pause-circle"
            onTriggered: {
                const paused = !screen.team.paused
                screen.change({ paused: paused })
                screen.window.notify(paused ? "Paused " + screen.team.name : "Resumed " + screen.team.name)
            }
        }
        KetMenuItem {
            text: "Automatic turns…"
            description: screen.automaticTurns + " of " + screen.turnBudget + " used"
            iconName: "clock"
            onTriggered: turnBudgetDialog.open()
        }
        KetMenuItem {
            text: "Review…"
            description: screen.reviewText(screen.reviewRounds, screen.reviewPlace)
            iconName: "eye"
            onTriggered: reviewDialog.open()
        }
        KetMenuItem {
            readonly property bool mine: !!screen.team && screen.team.merges === "user"
            text: mine ? "Let the leader merge tasks" : "Merge tasks myself"
            description: mine ? "Once the reviewer passes a task" : "Each reviewed task waits for you"
            iconName: "pull-request"
            onTriggered: {
                screen.change({ merges: mine ? "leader" : "user" })
                screen.window.notify(mine ? "The leader merges reviewed tasks" : "Reviewed tasks wait for you to merge")
            }
        }
        KetMenuSeparator {}
        KetMenuItem { text: "Delete team…"; iconName: "trash"; danger: true; onTriggered: disbandDialog.open() }
    }

    KetMenu {
        id: memberMenu
        property var who: null
        KetMenuItem { text: "Open conversation"; iconName: "chat"; onTriggered: screen.openConversation(memberMenu.who.number) }
        KetMenuSeparator {}
        KetMenuItem {
            text: "Remove from team…"
            description: screen.keeps(memberMenu.who)
            iconName: "trash"
            danger: true
            enabled: !!memberMenu.who && screen.keeps(memberMenu.who) === ""
            onTriggered: { removeDialog.who = memberMenu.who; removeDialog.open() }
        }
    }

    // Removing a member keeps its conversation; the tasks it owned go back to the leader to assign.
    KetDialog {
        id: removeDialog
        property var who: null
        readonly property var owned: who ? screen.tasks.filter(t => t.owner === who.number && t.state !== "done") : []
        title: "Remove " + (who ? screen.reference(who.number) : "") + " from " + (screen.team ? screen.team.name : "") + "?"
        // KetDialog lays its body out; a column gives the text its width.
        Column {
            width: removeDialog.availableWidth
            Label {
                width: parent.width
                text: (removeDialog.who ? "“" + removeDialog.who.title + "” stays as a conversation of the repository, with its worktree and branch. " : "")
                      + (removeDialog.owned.length > 0
                         ? screen.count(removeDialog.owned.length, "open task", "open tasks") + " ("
                           + removeDialog.owned.map(t => t.id).join(", ") + ") become unassigned for the leader to assign again."
                         : "It owns no open task.")
                color: KetTheme.t.textSecondary
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.4
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: removeDialog.reject() },
            KetButton {
                text: "Remove"
                variant: KetTheme.ButtonVariant.Danger
                onClicked: {
                    const number = removeDialog.who.number
                    removeDialog.accept()
                    screen.removeMember(number)
                }
            }
        ]
    }

    // T-37: a done task's worktree goes only when the user confirms it here. The daemon refuses
    // in words what it will not remove, and a refusal changes nothing.
    KetDialog {
        id: removeWorktreeDialog
        objectName: "removeWorktreeDialog"
        property var entry: null
        property string refusal: ""
        // Said after a removal: a conversation that could not be moved out of the folder.
        property string note: ""
        title: "Remove the worktree of " + (entry ? entry.id : "") + "?"
        Column {
            width: removeWorktreeDialog.availableWidth
            spacing: screen.px(10)
            Label {
                width: parent.width
                text: removeWorktreeDialog.entry
                      ? "Its folder is deleted from disk, freeing " + removeWorktreeDialog.entry.size + ". The task's branch and its commits stay."
                      : ""
                color: KetTheme.t.textSecondary
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.4
            }
            Mono {
                width: parent.width
                text: removeWorktreeDialog.entry ? removeWorktreeDialog.entry.path : ""
                wrapMode: Text.WrapAnywhere
                elide: Text.ElideNone
            }
            Label {
                width: parent.width
                text: "ketPlus runs git worktree remove, without force, and then git worktree prune from the repository's main checkout. It refuses a worktree with uncommitted or untracked changes or with commits on no remote branch, the main checkout, and the folder of a conversation that is running or waits for you. An idle conversation in the folder moves to the main checkout."
                color: KetTheme.t.textMuted
                font.pixelSize: screen.px(12)
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.4
            }
            KetBanner {
                objectName: "removeWorktreeRefusal"
                width: parent.width
                visible: removeWorktreeDialog.refusal !== ""
                tone: KetTheme.BannerTone.Danger
                title: "Not removed"
                message: removeWorktreeDialog.refusal
            }
            KetBanner {
                objectName: "removeWorktreeNote"
                width: parent.width
                visible: removeWorktreeDialog.note !== ""
                tone: KetTheme.BannerTone.Warning
                title: "Removed, but a conversation stayed"
                message: removeWorktreeDialog.note
            }
        }
        actions: [
            KetButton { text: removeWorktreeDialog.note !== "" ? "Close" : "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: removeWorktreeDialog.reject() },
            KetButton {
                text: "Remove worktree"
                visible: removeWorktreeDialog.note === ""
                variant: KetTheme.ButtonVariant.Danger
                enabled: removeWorktreeDialog.refusal === ""
                onClicked: {
                    const id = removeWorktreeDialog.entry.id
                    removeWorktreeDialog.accept()
                    if (screen.removeWorktree(id))
                        screen.window.notify("Removed the worktree of " + id)
                }
            }
        ]
    }

    // T-62: Remove all… names how many worktrees and their size; nothing goes before Remove.
    KetDialog {
        id: removeAllDialog
        objectName: "removeAllWorktreesDialog"
        readonly property int total: screen.doneWorktrees.length
        title: "Remove " + screen.count(total, "worktree", "worktrees") + "?"
        Column {
            width: removeAllDialog.availableWidth
            spacing: screen.px(10)
            Label {
                width: parent.width
                text: "The folders of " + screen.doneWorktrees.map(w => w.id).join(", ") + " are deleted from disk, freeing "
                      + screen.totalSize(screen.doneWorktrees) + " in total. The tasks' branches and their commits stay."
                color: KetTheme.t.textSecondary
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.4
            }
            Label {
                width: parent.width
                text: "Each one goes through the same checks as Remove…, one after another: a worktree with uncommitted or untracked changes, with commits on no remote branch, or the folder of a conversation that is running or waits for you is kept with the reason, and the others are still removed."
                color: KetTheme.t.textMuted
                font.pixelSize: screen.px(12)
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.4
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: removeAllDialog.reject() },
            KetButton {
                text: removeAllDialog.total === 1 ? "Remove worktree" : "Remove " + removeAllDialog.total + " worktrees"
                variant: KetTheme.ButtonVariant.Danger
                onClicked: {
                    removeAllDialog.accept()
                    const outcome = screen.removeAllWorktrees({})
                    screen.window.notify(screen.removalSummary(outcome))
                }
            }
        ]
    }

    KetDialog {
        id: taskDialog
        readonly property var task: screen.openTask || ({ id: "", title: "", criteria: [], history: [], state: "todo" })
        readonly property var owner: screen.member(task.owner)
        readonly property var reviewer: screen.members.find(m => m.role === "reviewer" && (m.reviews || []).includes(task.lane)) || null
        title: task.id + " · " + task.title
        // A task carries a description to read, so its dialog is wider than a form's.
        width: parent ? Math.min(screen.px(640), parent.width - 2 * recipe.margin) : screen.px(640)
        // The description shows this much until the user asks for all of it.
        property bool wholeDescription: false
        onOpened: wholeDescription = false
        DialogBody {
            Flow {
                width: parent.width
                spacing: screen.px(8)
                KetBadge { text: screen.states[taskDialog.task.state].label; tone: screen.states[taskDialog.task.state].tone }
                KetBadge { text: taskDialog.task.lane || "Whole epic" }
                KetBadge { visible: !!taskDialog.task.decision; text: "Waits for " + taskDialog.task.decision; tone: KetTheme.BadgeTone.Warning }
            }
            KetBanner {
                visible: !!taskDialog.task.outsideLane
                width: parent.width
                tone: KetTheme.BannerTone.Warning
                title: "Edited a file outside its lane"
                message: taskDialog.task.outsideLane
                    ? taskDialog.task.outsideLane.path + " belongs to " + taskDialog.task.outsideLane.lane
                      + " (" + screen.reference(taskDialog.task.outsideLane.owner) + "). The leader decides who makes the change."
                    : ""
                actionText: "Open the leader"
                onActionTriggered: { taskDialog.accept(); screen.openConversation(screen.leader.number) }
            }
            GridLayout {
                width: parent.width
                columns: 2
                columnSpacing: screen.px(16)
                rowSpacing: screen.px(14)
                Fact { label: "Owner"; value: taskDialog.task.owner ? screen.reference(taskDialog.task.owner) : "Unassigned"; mono: !!taskDialog.task.owner }
                Fact { label: "Reviewer"; value: taskDialog.reviewer ? screen.reference(taskDialog.reviewer.number) : ""; mono: true }
                Fact { label: "Pull request"; value: taskDialog.task.pr || ""; mono: true }
                Fact { label: "Branch"; value: taskDialog.task.branch || ""; mono: true }
                Fact { label: "Worktree"; value: taskDialog.task.worktree || ""; mono: true }
                Fact { label: "Depends on"; value: taskDialog.task.dependsOn || ""; mono: true }
            }
            Rectangle { visible: descriptionBlock.visible; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
            // What the leader wrote for the owner, as Markdown. A long one is cut with a fade.
            Column {
                id: descriptionBlock
                readonly property bool cut: descriptionText.implicitHeight > screen.px(168)
                visible: !!taskDialog.task.description
                width: parent.width
                spacing: screen.px(8)
                Label { text: "Description"; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                Item {
                    width: parent.width
                    height: descriptionBlock.cut && !taskDialog.wholeDescription ? screen.px(150) : descriptionText.implicitHeight
                    clip: true
                    MarkdownView {
                        id: descriptionText
                        width: parent.width
                        window: screen.window
                        markdown: taskDialog.task.description || ""
                    }
                    Rectangle {
                        visible: descriptionBlock.cut && !taskDialog.wholeDescription
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: screen.px(48)
                        gradient: Gradient {
                            GradientStop { position: 0; color: Qt.alpha(KetTheme.t.panelBg, 0) }
                            GradientStop { position: 1; color: KetTheme.t.panelBg }
                        }
                    }
                }
                KetButton {
                    visible: descriptionBlock.cut
                    text: taskDialog.wholeDescription ? "Show less" : "Show all"
                    variant: KetTheme.ButtonVariant.Ghost
                    controlSize: KetTheme.ControlSize.Small
                    onClicked: taskDialog.wholeDescription = !taskDialog.wholeDescription
                }
            }
            Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
            Column {
                width: parent.width
                spacing: screen.px(10)
                Row {
                    spacing: screen.px(8)
                    Label { text: "Acceptance criteria"; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: taskDialog.task.criteria.filter(c => c.done).length + " of " + taskDialog.task.criteria.length + " met"
                        color: KetTheme.t.textSecondary
                    }
                }
                Repeater {
                    model: taskDialog.task.criteria
                    RowLayout {
                        required property var modelData
                        width: parent.width
                        spacing: screen.px(10)
                        KetIcon {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: screen.px(2)
                            name: modelData.done ? "check" : "circle"
                            box: screen.px(14)
                            color: modelData.done ? KetTheme.t.success : KetTheme.t.textMuted
                            Accessible.name: modelData.done ? "Met" : "Not met yet"
                        }
                        Label { Layout.fillWidth: true; text: modelData.text; wrapMode: Text.Wrap; elide: Text.ElideNone; lineHeight: 1.3 }
                    }
                }
                Row {
                    spacing: screen.px(8)
                    KetIcon { y: screen.px(2); name: "lock"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Label {
                        width: parent.parent.width - screen.px(21)
                        text: taskDialog.task.criteriaNote || "Only the leader changes these. The reviewer checks the pull request against them."
                        color: KetTheme.t.textMuted
                        font.pixelSize: screen.px(12)
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                    }
                }
            }
            Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
            Column {
                width: parent.width
                spacing: screen.px(8)
                Label { text: "History"; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                Repeater {
                    model: taskDialog.task.history
                    RowLayout {
                        required property var modelData
                        width: parent.width
                        spacing: screen.px(12)
                        Label { Layout.preferredWidth: screen.px(104); Layout.alignment: Qt.AlignTop; text: modelData.when; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                        Label { Layout.fillWidth: true; text: modelData.text; color: KetTheme.t.textSecondary; wrapMode: Text.Wrap; elide: Text.ElideNone }
                    }
                }
            }
        }
        actions: [
            KetButton {
                visible: taskDialog.task.state !== "done"
                text: taskDialog.task.owner ? "Hand over…" : "Assign…"
                variant: KetTheme.ButtonVariant.Ghost
                onClicked: { const task = taskDialog.task; taskDialog.accept(); screen.startHandover(task) }
            },
            KetButton {
                visible: !!taskDialog.task.owner
                text: "Open the owner"
                onClicked: { const owner = taskDialog.task.owner; taskDialog.accept(); screen.openConversation(owner) }
            },
            KetButton { text: "Close"; variant: KetTheme.ButtonVariant.Primary; onClicked: taskDialog.accept() }
        ]
    }

    // A handover changes the owner; the new one receives everything the task carries.
    KetDialog {
        id: handoverDialog
        readonly property var task: screen.handover || ({ id: "", title: "", owner: "", lane: "" })
        readonly property var candidates: screen.members.filter(m => m.role !== "reviewer" && m.number !== task.owner)
        title: (task.owner ? "Hand over " : "Assign ") + task.id
        Column {
            width: handoverDialog.availableWidth
            spacing: screen.px(14)
            Label { width: parent.width; text: handoverDialog.task.title; font.pixelSize: screen.px(14); font.weight: Font.Medium; wrapMode: Text.Wrap; elide: Text.ElideNone }
            KetFieldRow {
                width: parent.width
                label: "New owner"
                help: handoverDialog.task.owner
                    ? "Gets the task's history, pull request, branch, worktree and the decisions made for it. "
                      + screen.reference(handoverDialog.task.owner) + " can no longer write to the worktree."
                    : "Gets the acceptance criteria and a new worktree for the task."
                KetSelect {
                    id: newOwner
                    width: parent.width
                    model: handoverDialog.candidates.map(m => "@agent/" + m.number + " · " + m.title
                                                              + (m.lane ? " · " + m.lane : ""))
                }
            }
            KetBanner {
                readonly property var chosen: handoverDialog.candidates[newOwner.currentIndex] || null
                visible: !!chosen && chosen.role === "developer" && chosen.lane !== handoverDialog.task.lane
                         && handoverDialog.task.lane !== ""
                width: parent.width
                tone: KetTheme.BannerTone.Warning
                title: "Another lane"
                message: chosen ? "This task is in " + handoverDialog.task.lane + "; " + screen.reference(chosen.number)
                                  + " owns " + chosen.lane + ". Its edits here will be flagged." : ""
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: handoverDialog.reject() },
            KetButton {
                text: handoverDialog.task.owner ? "Hand over" : "Assign"
                variant: KetTheme.ButtonVariant.Primary
                enabled: handoverDialog.candidates.length > 0
                onClicked: {
                    const next = handoverDialog.candidates[newOwner.currentIndex]
                    const task = handoverDialog.task
                    screen.changeTask(task.id, { owner: next.number },
                        task.owner ? "You handed the task over from " + screen.reference(task.owner) + " to " + screen.reference(next.number)
                                   : "You assigned " + screen.reference(next.number))
                    handoverDialog.accept()
                    screen.window.notify(task.id + " now belongs to " + screen.reference(next.number))
                }
            }
        ]
    }

    // One more developer, as a new conversation. A conversation that exists joins from the
    // sidebar instead: dragged onto the team's folder, or from its own menu.
    KetDialog {
        id: addMemberDialog
        // The new conversation's agent, chosen in the app's own Model & context dialog.
        property string agentProvider: "codex"
        property string agentModel: Fixtures.providerModels.codex[0].name
        property string agentEffort: "Medium"
        readonly property string agentLabel: {
            const entry = Fixtures.providers.find(p => p.id === agentProvider)
            return entry ? entry.label : agentProvider
        }
        property bool tried: false
        // T-23: "developer", or "reviewer" while the team has one reviewer.
        property string role: "developer"
        readonly property bool reviewer: role === "reviewer"
        readonly property bool reviewerOpen: screen.reviewers < Fixtures.mostReviewers
        function chooseRole(next) {
            role = next
            agentProvider = next === "reviewer" ? "claude" : "codex"
            agentModel = Fixtures.providerModels[agentProvider][0].name
            agentEffort = next === "reviewer" ? "High" : "Medium"
        }
        title: "Add a member to " + (screen.team ? screen.team.name : "")
        onOpened: {
            tried = false
            laneName.text = ""
            lanePaths.text = ""
            memberName.text = ""
        }
        onClosed: chooseRole("developer")
        DialogBody {
            KetFieldRow {
                width: parent.width
                label: "Role"
                help: addMemberDialog.reviewerOpen
                      ? "A second reviewer shortens the queue: each task in review goes to one of the two."
                      : "The team has two reviewers, the most a team has."
                Row {
                    width: parent.width
                    spacing: screen.px(6)
                    KetChoice {
                        objectName: "addMemberDeveloper"
                        width: (parent.width - parent.spacing) / 2
                        kind: KetTheme.ToggleKind.Radio
                        checked: !addMemberDialog.reviewer
                        text: "Developer"
                        onToggled: if (checked) addMemberDialog.chooseRole("developer")
                    }
                    KetChoice {
                        objectName: "addMemberReviewer"
                        width: (parent.width - parent.spacing) / 2
                        kind: KetTheme.ToggleKind.Radio
                        enabled: addMemberDialog.reviewerOpen
                        checked: addMemberDialog.reviewer
                        text: "Reviewer"
                        onToggled: if (checked) addMemberDialog.chooseRole("reviewer")
                    }
                }
            }
            KetFieldRow {
                visible: !addMemberDialog.reviewer
                width: parent.width
                label: "Lane"
                help: "The part of the code this developer owns."
                error: addMemberDialog.tried && laneName.text.trim() === "" ? "Name the lane." : ""
                KetTextField { id: laneName; width: parent.width; placeholderText: "Onboarding"; invalid: addMemberDialog.tried && text.trim() === "" }
            }
            KetFieldRow {
                visible: !addMemberDialog.reviewer
                width: parent.width
                label: "Paths"
                help: "Folders separated by commas. An edit outside them is flagged on the task."
                error: addMemberDialog.tried && lanePaths.text.trim() === "" ? "Give at least one folder." : ""
                KetTextField { id: lanePaths; width: parent.width; placeholderText: "apps/desktop/src/onboarding/"; invalid: addMemberDialog.tried && text.trim() === "" }
            }
            KetFieldRow {
                width: parent.width
                label: "Agent"
                help: addMemberDialog.reviewer ? "A reviewer runs on Claude, the provider that keeps a review read-only."
                                               : "The new conversation's provider, model and effort."
                KetButton {
                    width: parent.width
                    text: addMemberDialog.agentLabel + " · " + addMemberDialog.agentModel + " · " + addMemberDialog.agentEffort + "  ▾"
                    Accessible.name: "Choose model and reasoning effort"
                    onClicked: { memberAgentDialog.target = ""; memberAgentDialog.open() }
                }
            }
            KetFieldRow {
                width: parent.width
                label: "Name"
                help: "How the conversation shows in the team's folder. Its role is a tag beside it."
                error: addMemberDialog.tried && !addMemberDialog.reviewer && memberName.text.trim() === "" ? "Name the conversation." : ""
                KetTextField {
                    id: memberName
                    width: parent.width
                    placeholderText: addMemberDialog.reviewer ? "Reviewer " + (screen.reviewers + 1)
                                                              : laneName.text.trim() || "Onboarding"
                    invalid: addMemberDialog.tried && !addMemberDialog.reviewer && text.trim() === ""
                }
            }
            KetBanner {
                visible: !addMemberDialog.reviewer && screen.developers >= 3 && screen.reviewers < Fixtures.mostReviewers
                width: parent.width
                tone: KetTheme.BannerTone.Info
                title: screen.developers === 3 ? "A fourth developer" : "Reviews already queue"
                message: "The team's one reviewer keeps up with two or three developers. Past that, reviews queue; a second reviewer shortens the queue."
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: addMemberDialog.reject() },
            KetButton {
                text: "Add"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    addMemberDialog.tried = true
                    if (addMemberDialog.reviewer) {
                        addMemberDialog.accept()
                        screen.addReviewer({ id: addMemberDialog.agentProvider, label: addMemberDialog.agentLabel,
                                             model: addMemberDialog.agentModel, effort: addMemberDialog.agentEffort },
                                           memberName.text.trim())
                        return
                    }
                    const name = memberName.text.trim()
                    const lane = laneName.text.trim()
                    if (name === "" || lane === "" || lanePaths.text.trim() === "")
                        return
                    addMemberDialog.accept()
                    screen.addDeveloper(name, lane, lanePaths.text.split(",").map(p => p.trim()).filter(p => p),
                                        { id: addMemberDialog.agentProvider, label: addMemberDialog.agentLabel,
                                          model: addMemberDialog.agentModel, effort: addMemberDialog.agentEffort })
                }
            }
        ]
    }

    // The app's own picker, as the composer opens it: provider, model, effort, auto-compact.
    ModelContextDialog {
        id: memberAgentDialog
        parent: Basic.Overlay.overlay
        window: screen.window
        // "" is Add a member's one conversation; a role is that role's row in New team.
        property string target
        readonly property var planned: target !== "" ? newTeamDialog.plan[target] : null
        provider: planned ? planned.provider : addMemberDialog.agentProvider
        model: planned ? planned.model : addMemberDialog.agentModel
        effort: planned ? planned.effort : addMemberDialog.agentEffort
        // A review must stay read-only, and Claude is the provider that enforces it (ADR 0067).
        onlyProvider: target === "reviewer" || (target === "" && addMemberDialog.reviewer) ? "claude" : ""
        onlyReason: "A team's reviewer runs on Claude, the provider that keeps a review read-only."
        onChosen: (provider, model, effort) => {
            if (target !== "") {
                newTeamDialog.plan = newTeamDialog.withRole(target, { provider: provider, model: model, effort: effort })
                return
            }
            addMemberDialog.agentProvider = provider
            addMemberDialog.agentModel = model
            addMemberDialog.agentEffort = effort
        }
    }

    KetDialog {
        id: newTeamDialog
        property bool tried: false
        // The repository whose folder the team joins.
        property string repository: Fixtures.repositories[0].name
        // The members Create starts: how many of each role, and the agent every one of them uses.
        readonly property var defaults: ({
            leader: { count: 1, provider: "claude", model: "Opus 5.5", effort: "High" },
            developer: { count: 2, provider: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
            reviewer: { count: 1, provider: "claude", model: "Opus 5.5", effort: "High" } })
        property var plan: defaults
        // Who merges a reviewed task into the integration branch: "leader" or "user".
        property string merges: "leader"
        readonly property var roles: [
            { role: "leader", label: "Leader" },
            { role: "developer", label: "Developers" },
            { role: "reviewer", label: "Reviewers" }]
        // One leader, always; the number of developers and of reviewers (T-23) is a choice.
        readonly property int mostDevelopers: 5
        readonly property int total: plan.leader.count + plan.developer.count + plan.reviewer.count
        readonly property string advice:
            plan.developer.count > 3 && plan.reviewer.count < Fixtures.mostReviewers
                ? "One reviewer keeps up with two or three developers; with more, reviews queue. A second reviewer shortens the queue."
            : plan.reviewer.count > 1 ? "Each task in review goes to one of the two reviewers, so two tasks are reviewed at the same time."
            : plan.developer.provider === plan.reviewer.provider ? "Developers on another provider than the reviewer get a more independent review."
            : ""
        function withRole(role, patch) {
            const next = Object.assign({}, plan)
            next[role] = Object.assign({}, plan[role], patch)
            return next
        }
        function agentText(role) {
            const entry = Fixtures.providers.find(p => p.id === plan[role].provider)
            return (entry ? entry.label : plan[role].provider) + " · " + plan[role].model + " · " + plan[role].effort
        }
        title: "New team in " + repository
        onOpened: { tried = false; teamName.text = ""; teamGoal.text = ""; teamBudget.text = String(Fixtures.turnBudget); plan = defaults; merges = "leader"; leaderMerges.checked = true; teamPermission.currentIndex = 1; teamReview.rounds = 2; teamReview.place = "board" }
        DialogBody {
            KetFieldRow {
                width: parent.width
                label: "Name"
                error: newTeamDialog.tried && teamName.text.trim() === "" ? "Name the team." : ""
                KetTextField { id: teamName; width: parent.width; placeholderText: "Onboarding rework"; invalid: newTeamDialog.tried && text.trim() === "" }
            }
            KetFieldRow {
                width: parent.width
                label: "Goal"
                // The leader's main input: the card's whole width.
                stacked: true
                help: "The leader breaks it into tasks and may ask the conversations it mentions for context."
                error: teamGoal.problem
                TeamGoalField { id: teamGoal; width: parent.width }
            }
            KetFieldRow {
                width: parent.width
                label: "Integration branch"
                help: "Reviewed tasks merge here. Merging this branch onward always stays with you."
                KetSelect { id: teamBranch; width: parent.width; model: ["develop", "integration", "release/1.4"] }
            }
            KetFieldRow {
                width: parent.width
                label: "Merges tasks"
                Row {
                    width: parent.width
                    spacing: screen.px(6)
                    KetChoice {
                        id: leaderMerges
                        width: (parent.width - parent.spacing) / 2
                        kind: KetTheme.ToggleKind.Radio
                        checked: true
                        text: "The leader"
                        onToggled: if (checked) newTeamDialog.merges = "leader"
                    }
                    KetChoice {
                        width: (parent.width - parent.spacing) / 2
                        kind: KetTheme.ToggleKind.Radio
                        text: "Me"
                        onToggled: if (checked) newTeamDialog.merges = "user"
                    }
                }
            }
            TeamReviewFields { id: teamReview; width: parent.width }
            KetFieldRow {
                width: parent.width
                label: "Automatic turns"
                help: "Turns members start on their own before the team asks you to continue. New teams start from Settings → Teams."
                error: newTeamDialog.tried ? Fixtures.turnBudgetError(teamBudget.text) : ""
                KetTextField {
                    id: teamBudget
                    width: screen.px(96)
                    Accessible.name: "Automatic turns"
                    invalid: newTeamDialog.tried && Fixtures.turnBudgetError(text) !== ""
                }
            }
            // What the leader and the developers may do without asking, as the composer's Mode
            // lists it. The reviewer is not in it: a review is always read-only.
            KetFieldRow {
                width: parent.width
                label: "Permissions"
                help: Fixtures.permissionModes[teamPermission.currentIndex].description
                      + ". The leader and the developers start with this; change it for one member in its conversation. The reviewer stays read-only."
                KetSelect {
                    id: teamPermission
                    width: parent.width
                    model: Fixtures.permissionModes.map(mode => mode.label)
                    currentIndex: 1
                }
            }
            // Scaffold: the team's conversations, started with it instead of one by one.
            Column {
                width: parent.width
                spacing: screen.px(8)
                Label { text: "Members"; font.weight: Font.Medium }
                Label {
                    width: parent.width
                    text: "A team has one leader and one or two reviewers. Create starts every member as a new conversation; the leader gives each developer its lane."
                    color: KetTheme.t.textSecondary
                    font.pixelSize: screen.px(12)
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                }
                Repeater {
                    model: newTeamDialog.roles
                    Item {
                        id: planRow
                        required property var modelData
                        readonly property var planned: newTeamDialog.plan[modelData.role]
                        width: parent.width
                        height: screen.px(32)
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            width: screen.px(96)
                            text: planRow.modelData.label
                        }
                        Item {
                            id: countChoice
                            x: screen.px(104)
                            width: screen.px(72)
                            height: parent.height
                            KetSelect {
                                visible: planRow.modelData.role === "developer"
                                anchors.fill: parent
                                Accessible.name: "Number of developers"
                                model: Array.from({ length: newTeamDialog.mostDevelopers }, (v, i) => String(i + 1))
                                currentIndex: planRow.planned.count - 1
                                onActivated: index => newTeamDialog.plan = newTeamDialog.withRole("developer", { count: index + 1 })
                            }
                            KetSelect {
                                objectName: "newTeamReviewers"
                                visible: planRow.modelData.role === "reviewer"
                                anchors.fill: parent
                                Accessible.name: "Number of reviewers"
                                model: Array.from({ length: Fixtures.mostReviewers }, (v, i) => String(i + 1))
                                currentIndex: planRow.planned.count - 1
                                onActivated: index => newTeamDialog.plan = newTeamDialog.withRole("reviewer", { count: index + 1 })
                            }
                            Label {
                                visible: planRow.modelData.role === "leader"
                                x: screen.px(11)
                                anchors.verticalCenter: parent.verticalCenter
                                text: "1"
                                color: KetTheme.t.textSecondary
                            }
                        }
                        KetButton {
                            anchors.left: countChoice.right
                            anchors.leftMargin: screen.px(8)
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: newTeamDialog.agentText(planRow.modelData.role) + "  ▾"
                            Accessible.name: "Choose model and reasoning effort for " + planRow.modelData.label.toLowerCase()
                            onClicked: { memberAgentDialog.target = planRow.modelData.role; memberAgentDialog.open() }
                        }
                    }
                }
                Label {
                    visible: text !== ""
                    width: parent.width
                    text: newTeamDialog.advice
                    color: KetTheme.t.textSecondary
                    font.pixelSize: screen.px(12)
                    wrapMode: Text.Wrap
                    elide: Text.ElideNone
                }
            }
            KetChoice {
                id: leaderWrites
                width: parent.width
                text: "The leader also writes the requirements"
                description: "The team's leader puts acceptance criteria on each task before assigning it. Turn this off when a separate conversation researches the requirements."
                checked: true
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: newTeamDialog.reject() },
            KetButton {
                id: createButton
                text: "Create with " + newTeamDialog.total + " members"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    newTeamDialog.tried = true
                    const name = teamName.text.trim()
                    if (name === "" || teamGoal.problem !== "" || Fixtures.turnBudgetError(teamBudget.text) !== "")
                        return
                    newTeamDialog.accept()
                    screen.createTeam(newTeamDialog.repository, name, teamGoal.text.trim(), teamBranch.currentText,
                                      leaderWrites.checked, newTeamDialog.plan, newTeamDialog.merges, teamPermission.currentText,
                                      Number(teamBudget.text.trim()), { rounds: teamReview.rounds, place: teamReview.place })
                }
            }
        ]
    }

    // T-40: the team's review, changed while it runs; tasks in review keep theirs.
    KetDialog {
        id: reviewDialog
        title: "Review for " + (screen.team ? screen.team.name : "")
        onOpened: { reviewFields.rounds = screen.reviewRounds; reviewFields.place = screen.reviewPlace }
        DialogBody {
            TeamReviewFields {
                id: reviewFields
                width: parent.width
                note: "Applies to tasks that enter review from now; a review in progress keeps its rounds and place."
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: reviewDialog.reject() },
            KetButton {
                objectName: "teamReviewSave"
                text: "Save"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    reviewDialog.accept()
                    screen.changeReview(reviewFields.rounds, reviewFields.place)
                }
            }
        ]
    }

    // The team's own budget, changed while it runs (ADR 0114, open point 2).
    KetDialog {
        id: turnBudgetDialog
        property bool tried: false
        readonly property string error: tried ? Fixtures.turnBudgetError(turnBudgetField.text) : ""
        title: "Automatic turns for " + (screen.team ? screen.team.name : "")
        onOpened: { tried = false; turnBudgetField.text = String(screen.turnBudget) }
        DialogBody {
            KetFieldRow {
                width: parent.width
                label: "Automatic turns"
                help: "Applies from the next automatic turn. The " + screen.automaticTurns + " turns used stay counted"
                      + (screen.team && screen.team.pausedReason === "budget"
                         ? "; a budget that leaves turns resumes the team." : ".")
                error: turnBudgetDialog.error
                KetTextField {
                    id: turnBudgetField
                    width: screen.px(96)
                    Accessible.name: "Automatic turns"
                    invalid: turnBudgetDialog.error !== ""
                }
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: turnBudgetDialog.reject() },
            KetButton {
                text: "Save"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    turnBudgetDialog.tried = true
                    if (Fixtures.turnBudgetError(turnBudgetField.text) !== "")
                        return
                    turnBudgetDialog.accept()
                    screen.changeTurnBudget(Number(turnBudgetField.text.trim()))
                }
            }
        ]
    }

    KetDialog {
        id: disbandDialog
        title: "Delete " + (screen.team ? screen.team.name : "") + "?"
        // KetDialog lays its body out; a column gives the text its width.
        Column {
            width: disbandDialog.availableWidth
            Label {
                width: parent.width
                text: "The team, its tasks and their history are removed. Every conversation, worktree, branch and pull request stays."
                color: KetTheme.t.textSecondary
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.4
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: disbandDialog.reject() },
            KetButton {
                text: "Delete team"
                variant: KetTheme.ButtonVariant.Danger
                onClicked: {
                    disbandDialog.accept()
                    screen.deleteTeam()
                    // Its conversations are back among the repository's own.
                    screen.window.screen = "workspace"
                }
            }
        ]
    }
}
