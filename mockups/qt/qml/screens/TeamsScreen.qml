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
        blocked: { label: "Blocked", tone: KetTheme.BadgeTone.Warning },
        done: { label: "Done", tone: KetTheme.BadgeTone.Success }
    })
    readonly property var filters: ["", "doing", "review", "blocked", "done"]
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
    readonly property var decisions: team ? team.decisions : []
    readonly property var shownTasks: filter === "" ? tasks : tasks.filter(t => t.state === filter)
    readonly property var leader: members.find(m => m.role === "leader") || null
    readonly property int developers: members.filter(m => m.role === "developer").length
    readonly property int reviewers: members.filter(m => m.role === "reviewer").length
    readonly property int doneTasks: tasks.filter(t => t.state === "done").length
    // Two or three developers keep one reviewer busy; a fourth makes reviews queue.
    readonly property bool reviewQueue: developers >= 4 && reviewers < 2
    // Hides the secondary columns when the page is narrow.
    readonly property bool wide: content.width >= px(860)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function reference(number) { return number ? "@agent/" + number : "Unassigned" }
    function member(number) { return members.find(m => m.number === number) || null }
    function count(n, one, many) { return n + " " + (n === 1 ? one : many) }

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

    // `teams/empty`: no team yet. `teams/new`: a team nobody is on yet. `teams/large`: a fourth
    // developer, so reviews queue.
    function showState(state) {
        teams = state === "empty" ? [] : Fixtures.teams
        teamIndex = 0
        filter = ""
        if (state === "large")
            change({ members: members.concat([{ number: "9079", title: "Check this worktree", role: "developer",
                provider: "Claude · Opus 5.5", lane: "Onboarding", paths: ["apps/desktop/src/onboarding/"],
                state: "idle", now: "No task yet" }]) })
        if (state === "new") {
            // A team as New team leaves it: a name, a goal, a branch and nobody on it.
            teams = teams.concat([{ id: "onboarding", name: "Onboarding rework",
                goal: "Make the first run explain itself.", repository: Fixtures.repositories[0].name,
                branch: "develop", paused: false, leaderWrites: true, members: [], tasks: [],
                decisions: [], updates: [] }])
            teamIndex = teams.length - 1
        }
        if (state === "task")
            Qt.callLater(() => screen.showTask("T-3"))
    }

    // `--open=new-team|add-member|task|handover` opens a dialog for review captures.
    function openDialog(name) {
        if (name === "new-team")
            startNewTeam("")
        else if (name === "delete-team" && team)
            disbandDialog.open()
        else if (name === "remove-member" && team)
            startRemove(team.id, "8215")
        else if (name === "add-member" && team)
            addMemberDialog.open()
        else if (name === "member-agent" && team) {
            addMemberDialog.open()
            memberAgentDialog.open()
        }
        else if (name === "member-agent" && team) {
            addMemberDialog.open()
            memberAgentDialog.open()
        }
        else if (name === "task")
            showTask("T-3")
        else if (name === "handover")
            startHandover(tasks.find(t => t.id === "T-3"))
    }
    function openEveryOverlay() {
        for (const dialog of [newTeamDialog, addMemberDialog, taskDialog, handoverDialog, disbandDialog, removeDialog]) {
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
        addMemberDialog.preselect = number || ""
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
        if (!who)
            return
        removeDialog.who = who
        removeDialog.open()
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
        for (const r of Fixtures.repositories) {
            const match = r.conversations.find(c => c.number === number)
            if (match) {
                const who = member(number)
                screen.window.openConversation(r.name, who && who.name ? Object.assign({}, match, { title: who.name }) : match)
                return
            }
        }
        // A conversation Add a member started: new, with nothing sent yet.
        const who = member(number)
        const blank = Fixtures.repositories[0].conversations.find(c => c.transcript === "welcome")
        if (who && blank)
            screen.window.openConversation(team.repository, Object.assign({}, blank, {
                number: who.number, title: who.title, tags: [], status: KetTheme.ConversationStatus.NotStarted,
                provider: who.agent ? who.agent.label : blank.provider, providerId: who.agent ? who.agent.id : blank.providerId,
                model: who.agent ? who.agent.model : blank.model, effort: who.agent ? who.agent.effort : blank.effort }))
    }
    function addMember(role) {
        addMemberDialog.role = role
        addMemberDialog.open()
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
                description: screen.team ? screen.team.goal : ""
                KetButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Add member"
                    iconName: "plus"
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
                Label {
                    visible: screen.tasks.length > 0
                    text: screen.doneTasks + " of " + screen.count(screen.tasks.length, "task", "tasks") + " done"
                    color: KetTheme.t.textSecondary
                }
            }

            KetBanner {
                width: parent.width
                visible: screen.reviewQueue
                tone: KetTheme.BannerTone.Info
                title: screen.count(screen.developers, "developer", "developers") + " share "
                       + screen.count(screen.reviewers, "reviewer", "reviewers")
                message: "Reviews will queue. One reviewer keeps up with two or three developers; give a second one some of the lanes."
                actionText: "Add a reviewer"
                onActionTriggered: screen.addMember("reviewer")
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
                        // The choices, and beside them who asked and what the answer unblocks.
                        RowLayout {
                            width: parent.width
                            spacing: screen.px(8)
                            Repeater {
                                model: card.modelData.options
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
                                onClicked: screen.openConversation(card.modelData.askedBy)
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                text: card.modelData.id + " · " + screen.reference(card.modelData.askedBy) + " · " + card.modelData.when
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
                message: screen.leader
                    ? "The leader breaks the goal into tasks, writes what done means for each, then assigns them. Open the leader to start."
                    : "Tasks come from the team's leader. Add a leader first."
                actionText: screen.leader ? "Open the leader" : ""
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

            Item { width: 1; height: screen.px(4) }

            SectionTitle {
                title: "Members"
                count: screen.members.length === 0 ? "" : (screen.leader ? "1 leader · " : "No leader · ")
                       + screen.count(screen.developers, "developer", "developers") + " · "
                       + screen.count(screen.reviewers, "reviewer", "reviewers")
            }
            KetEmptyState {
                visible: screen.members.length === 0
                width: parent.width
                iconName: "bot"
                title: "Nobody is on this team yet"
                message: "Start with the leader: the one conversation that reports to you and assigns the tasks. Then add developers and a reviewer."
                actionText: "Add a leader"
                actionIcon: "plus"
                onActionTriggered: screen.addMember("leader")
            }
            Panel {
                visible: screen.members.length > 0
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
                                    Layout.preferredWidth: screen.px(260)
                                    spacing: screen.px(5)
                                    Label { width: parent.width; text: memberRow.modelData.title; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                                    Row {
                                        spacing: screen.px(8)
                                        Mono { text: screen.reference(memberRow.modelData.number) }
                                        Label { text: memberRow.modelData.provider; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                                    }
                                }
                                Item {
                                    Layout.preferredWidth: screen.px(158)
                                    Layout.fillHeight: true
                                    KetBadge {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: screen.roles[memberRow.modelData.role].label
                                        tone: screen.roles[memberRow.modelData.role].tone
                                    }
                                }
                                // A developer's lane and its paths; the lanes a reviewer checks.
                                Column {
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: screen.px(250)
                                    spacing: screen.px(5)
                                    Label {
                                        width: parent.width
                                        text: memberRow.modelData.role === "developer" ? memberRow.modelData.lane
                                            : memberRow.modelData.role === "reviewer" ? "Reviews " + memberRow.modelData.reviews.join(", ")
                                            : "Assigns tasks, orders the merges"
                                        color: memberRow.modelData.role === "developer" ? KetTheme.t.textMain : KetTheme.t.textSecondary
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
                            width: updateList.width
                            height: screen.px(52)
                            Rectangle { visible: updateRow.index > 0; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: screen.px(18)
                                anchors.rightMargin: screen.px(18)
                                spacing: screen.px(12)
                                KetIcon {
                                    name: updateRow.modelData.icon
                                    box: screen.px(14)
                                    color: updateRow.modelData.warning ? KetTheme.t.warning : KetTheme.t.textSecondary
                                }
                                Column {
                                    Layout.fillWidth: true
                                    spacing: screen.px(4)
                                    Label { width: parent.width; text: updateRow.modelData.text }
                                    Label { width: parent.width; text: updateRow.modelData.detail; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
                                }
                                Label { text: updateRow.modelData.when; color: KetTheme.t.textMuted; font.pixelSize: screen.px(12) }
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
        KetMenuItem { text: "Add member…"; iconName: "plus"; onTriggered: addMemberDialog.open() }
        KetMenuItem { text: "New team…"; iconName: "bot"; onTriggered: screen.startNewTeam(screen.team.repository) }
        KetMenuItem {
            text: screen.team && screen.team.paused ? "Resume automatic turns" : "Pause automatic turns"
            description: "Members finish their turn, then wait for you"
            iconName: screen.team && screen.team.paused ? "play" : "pause-circle"
            onTriggered: {
                const paused = !screen.team.paused
                screen.change({ paused: paused })
                screen.window.notify(paused ? "Paused " + screen.team.name : "Resumed " + screen.team.name)
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
            description: memberMenu.who && memberMenu.who.role === "leader" ? "A team keeps its leader; delete the team instead" : ""
            iconName: "trash"
            danger: true
            enabled: !!memberMenu.who && memberMenu.who.role !== "leader"
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
                    const who = removeDialog.who
                    screen.change({
                        members: screen.members.filter(m => m.number !== who.number)
                            .map(m => m.role !== "reviewer" ? m : Object.assign({}, m, { reviews: (m.reviews || []).filter(l => l !== who.lane) })),
                        tasks: screen.tasks.map(t => t.owner !== who.number || t.state === "done" ? t : Object.assign({}, t, {
                            owner: "", state: t.state === "review" ? "review" : "todo",
                            history: t.history.concat([{ when: "Just now", text: "You removed " + screen.reference(who.number) + " from the team; the task is unassigned" }]) }))
                    })
                    removeDialog.accept()
                    screen.window.notify("Removed " + screen.reference(who.number) + " from " + screen.team.name)
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
        Column {
            width: taskDialog.availableWidth
            spacing: screen.px(16)
            Row {
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
                    KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "lock"; box: screen.px(13); color: KetTheme.t.textMuted }
                    Label {
                        width: taskDialog.availableWidth - screen.px(21)
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

    KetDialog {
        id: addMemberDialog
        // "leader", "developer" or "reviewer"; "" opens on the role the team needs first.
        property string role
        readonly property var roleChoices: screen.leader ? ["developer", "reviewer"] : ["leader", "developer", "reviewer"]
        // Who takes the role: a conversation started for it, or one of `free` moved in.
        property bool existing: false
        readonly property var chosen: existing && free.length > 0 ? free[Math.max(0, memberChoice.currentIndex)] : null
        // The new conversation's agent, chosen in the app's own Model & context dialog.
        property string agentProvider: "codex"
        property string agentModel: Fixtures.providerModels.codex[0].name
        property string agentEffort: "Medium"
        readonly property string agentLabel: {
            const entry = Fixtures.providers.find(p => p.id === agentProvider)
            return entry ? entry.label : agentProvider
        }
        // What the name field suggests: the lane, or the role in this team.
        readonly property string suggestedName: role === "leader" ? "Lead " + (screen.team ? screen.team.name : "")
            : role === "developer" ? (laneName.text.trim() || "Onboarding") : "Review " + (screen.team ? screen.team.name : "")
        function fillName() { memberName.text = chosen ? chosen.title : "" }
        onChosenChanged: fillName()
        property bool tried: false
        // A conversation the sidebar's Add to team… chose.
        property string preselect
        readonly property var free: {
            // The repository's conversations that belong to no team.
            const taken = []
            for (const t of screen.teams)
                for (const m of t.members)
                    taken.push(m.number)
            const repository = Fixtures.repositories.find(r => screen.team && r.name === screen.team.repository)
            return repository ? repository.conversations.filter(c => !taken.includes(c.number)) : []
        }
        readonly property var lanes: screen.members.filter(m => m.role === "developer").map(m => m.lane)
        title: "Add a member to " + (screen.team ? screen.team.name : "")
        onOpened: {
            tried = false
            laneName.text = ""
            lanePaths.text = ""
            if (!roleChoices.includes(role))
                role = roleChoices[0]
            const index = free.findIndex(c => c.number === preselect)
            existing = index >= 0
            memberChoice.currentIndex = Math.max(0, index)
            fillName()
        }
        onClosed: { role = ""; preselect = "" }
        Column {
            width: addMemberDialog.availableWidth
            spacing: screen.px(14)
            // From a conversation's Add to a team…: which of the repository's teams it joins.
            KetFieldRow {
                visible: addMemberDialog.preselect !== "" && screen.teams.length > 1
                width: parent.width
                label: "Team"
                KetSelect {
                    width: parent.width
                    model: screen.teams.map(t => t.name)
                    currentIndex: screen.teamIndex
                    onActivated: index => screen.teamIndex = index
                }
            }
            // First the role, then who takes it.
            KetFieldRow {
                width: parent.width
                label: "Role"
                help: addMemberDialog.role === "leader"
                    ? "Reports to you, breaks the goal into tasks, assigns them and orders the merges. It does not write code or review."
                    : addMemberDialog.role === "developer"
                    ? "Takes tasks in its lane, works in the task's worktree and opens the pull request."
                    : addMemberDialog.lanes.length > 0
                    ? "Reviews " + addMemberDialog.lanes.join(", ") + ". It never reviews its own work and never merges."
                    : "Reviews every lane once the team has developers. It never merges."
                KetSegmentedControl {
                    options: addMemberDialog.roleChoices.map(r => r.charAt(0).toUpperCase() + r.slice(1))
                    currentIndex: Math.max(0, addMemberDialog.roleChoices.indexOf(addMemberDialog.role))
                    onActivated: index => addMemberDialog.role = addMemberDialog.roleChoices[index]
                }
            }
            KetFieldRow {
                visible: addMemberDialog.role === "developer"
                width: parent.width
                label: "Lane"
                help: "The part of the code this developer owns."
                error: addMemberDialog.tried && laneName.text.trim() === "" ? "Name the lane." : ""
                KetTextField { id: laneName; width: parent.width; placeholderText: "Onboarding"; invalid: addMemberDialog.tried && text.trim() === "" }
            }
            KetFieldRow {
                visible: addMemberDialog.role === "developer"
                width: parent.width
                label: "Paths"
                help: "Folders separated by commas. An edit outside them is flagged on the task."
                error: addMemberDialog.tried && lanePaths.text.trim() === "" ? "Give at least one folder." : ""
                KetTextField { id: lanePaths; width: parent.width; placeholderText: "apps/desktop/src/onboarding/"; invalid: addMemberDialog.tried && text.trim() === "" }
            }
            Label {
                text: "Who takes this role"
                font.weight: Font.Medium
            }
            KetChoice {
                width: parent.width
                kind: KetTheme.ToggleKind.Radio
                text: "Start a new conversation"
                description: "A fresh agent opens in the team's folder and waits for its first " + (addMemberDialog.role === "leader" ? "goal." : addMemberDialog.role === "developer" ? "task." : "review.")
                checked: !addMemberDialog.existing
                onClicked: addMemberDialog.existing = false
            }
            KetFieldRow {
                visible: !addMemberDialog.existing
                width: parent.width
                label: "Agent"
                help: addMemberDialog.role === "reviewer" ? "A reviewer from another provider than the developers catches more." : ""
                KetButton {
                    width: parent.width
                    text: addMemberDialog.agentLabel + " · " + addMemberDialog.agentModel + " · " + addMemberDialog.agentEffort + "  ▾"
                    Accessible.name: "Choose model and reasoning effort"
                    onClicked: memberAgentDialog.open()
                }
            }
            KetChoice {
                width: parent.width
                kind: KetTheme.ToggleKind.Radio
                enabled: addMemberDialog.free.length > 0
                text: "Move in an existing conversation"
                description: addMemberDialog.free.length > 0
                    ? "One of " + screen.team.repository + "'s conversations that is on no team. It keeps its history and moves into the team's folder."
                    : "Every conversation in " + (screen.team ? screen.team.repository : "") + " is already on a team."
                checked: addMemberDialog.existing
                onClicked: addMemberDialog.existing = true
            }
            KetFieldRow {
                visible: addMemberDialog.existing
                width: parent.width
                label: "Conversation"
                KetSelect {
                    id: memberChoice
                    width: parent.width
                    model: addMemberDialog.free.map(c => "@agent/" + c.number + " · " + c.title)
                }
            }
            KetFieldRow {
                width: parent.width
                label: "Name"
                help: "How the conversation shows in the team's folder. Its role is a tag beside it."
                error: addMemberDialog.tried && memberName.text.trim() === "" ? "Name the conversation." : ""
                KetTextField {
                    id: memberName
                    width: parent.width
                    placeholderText: addMemberDialog.suggestedName
                    invalid: addMemberDialog.tried && text.trim() === ""
                }
            }
            KetBanner {
                visible: addMemberDialog.role === "developer" && screen.developers >= 3 && screen.reviewers < 2
                width: parent.width
                tone: KetTheme.BannerTone.Info
                title: "A fourth developer"
                message: "One reviewer keeps up with two or three developers. Add a second reviewer too, or reviews will queue."
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: addMemberDialog.reject() },
            KetButton {
                text: "Add"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    const role = addMemberDialog.role
                    const developer = role === "developer"
                    addMemberDialog.tried = true
                    const name = memberName.text.trim()
                    if (name === "" || (developer && (laneName.text.trim() === "" || lanePaths.text.trim() === "")))
                        return
                    // A new conversation gets the next free number and a name from its role.
                    let number = 6101
                    while (screen.teams.some(t => t.members.some(m => m.number === String(number))))
                        number += 7
                    const moved = addMemberDialog.chosen
                    const chosen = moved || { number: String(number) }
                    screen.change({ members: screen.members.concat([{
                        number: chosen.number, title: name, name: name, role: role,
                        provider: moved ? (moved.provider || "Claude") + " · " + (moved.model || "Opus 5.5")
                                        : addMemberDialog.agentLabel + " · " + addMemberDialog.agentModel,
                        agent: moved ? null : { id: addMemberDialog.agentProvider, label: addMemberDialog.agentLabel,
                                                model: addMemberDialog.agentModel, effort: addMemberDialog.agentEffort },
                        lane: developer ? laneName.text.trim() : "",
                        paths: developer ? lanePaths.text.split(",").map(p => p.trim()).filter(p => p) : [],
                        reviews: role === "reviewer" ? addMemberDialog.lanes : [],
                        state: "idle",
                        now: role === "leader" ? "Waiting for the goal" : developer ? "No task yet" : "No review yet" }]) })
                    addMemberDialog.accept()
                    screen.window.notify("Added @agent/" + chosen.number + " as the team's " + role)
                }
            }
        ]
    }

    // The app's own picker, as the composer opens it: provider, model, effort, auto-compact.
    ModelContextDialog {
        id: memberAgentDialog
        parent: Basic.Overlay.overlay
        window: screen.window
        provider: addMemberDialog.agentProvider
        model: addMemberDialog.agentModel
        effort: addMemberDialog.agentEffort
        onChosen: (provider, model, effort) => {
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
        title: "New team in " + repository
        onOpened: { tried = false; teamName.text = ""; teamGoal.text = "" }
        Column {
            width: newTeamDialog.availableWidth
            spacing: screen.px(14)
            KetFieldRow {
                width: parent.width
                label: "Name"
                error: newTeamDialog.tried && teamName.text.trim() === "" ? "Name the team." : ""
                KetTextField { id: teamName; width: parent.width; placeholderText: "Onboarding rework"; invalid: newTeamDialog.tried && text.trim() === "" }
            }
            KetFieldRow {
                width: parent.width
                label: "Goal"
                help: "One sentence, or an issue. The leader breaks it into tasks."
                KetTextField { id: teamGoal; width: parent.width; placeholderText: "Finish epic #139" }
            }
            KetFieldRow {
                width: parent.width
                label: "Integration branch"
                help: "Tasks merge here. The leader orders the merges and runs the full tests at the end."
                KetSelect { id: teamBranch; width: parent.width; model: ["develop", "integration", "release/1.4"] }
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
                text: "Create"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    newTeamDialog.tried = true
                    const name = teamName.text.trim()
                    if (name === "")
                        return
                    // A team starts with nobody; its page asks for a leader first.
                    screen.teams = screen.teams.concat([{
                        id: "team-" + screen.teams.length, name: name, goal: teamGoal.text.trim() || "No goal yet.",
                        repository: newTeamDialog.repository, branch: teamBranch.currentText, paused: false,
                        leaderWrites: leaderWrites.checked, members: [],
                        tasks: [], decisions: [], updates: [] }])
                    screen.teamIndex = screen.teams.length - 1
                    screen.filter = ""
                    newTeamDialog.accept()
                    screen.window.notify("Created " + name)
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
                    const name = screen.team.name
                    const next = screen.teams.filter((t, i) => i !== screen.teamIndex)
                    screen.teamIndex = 0
                    screen.teams = next
                    disbandDialog.accept()
                    // Its conversations are back among the repository's own.
                    screen.window.screen = "workspace"
                    screen.window.notify("Deleted " + name)
                }
            }
        ]
    }
}
