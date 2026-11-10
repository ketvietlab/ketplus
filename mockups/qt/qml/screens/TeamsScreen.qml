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

    // `teams/empty`: no team yet. `teams/large`: a fourth developer, so reviews queue.
    function showState(state) {
        teams = state === "empty" ? [] : Fixtures.teams
        teamIndex = 0
        filter = ""
        if (state === "large")
            change({ members: members.concat([{ number: "9079", title: "Check this worktree", role: "developer",
                provider: "Claude · Opus 5.5", lane: "Onboarding", paths: ["apps/desktop/src/onboarding/"],
                state: "idle", now: "No task yet" }]) })
        if (state === "task")
            Qt.callLater(() => screen.showTask("T-3"))
    }

    // `--open=new-team|add-member|task|handover` opens a dialog for review captures.
    function openDialog(name) {
        if (name === "new-team")
            newTeamDialog.open()
        else if (name === "add-member" && team)
            addMemberDialog.open()
        else if (name === "task")
            showTask("T-3")
        else if (name === "handover")
            startHandover(tasks.find(t => t.id === "T-3"))
    }
    function openEveryOverlay() {
        for (const dialog of [newTeamDialog, addMemberDialog, taskDialog, handoverDialog, disbandDialog]) {
            if (dialog === taskDialog || dialog === handoverDialog)
                openTask = handover = Fixtures.teams[0].tasks[2]
            dialog.open()
            dialog.close()
        }
        openTask = handover = null
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
                screen.window.openConversation(r.name, match)
                return
            }
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
        onActionTriggered: newTeamDialog.open()
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
                onActionTriggered: { addMemberDialog.role = 1; addMemberDialog.open() }
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
                message: "The leader breaks the goal into tasks, writes what done means for each, then assigns them. Open the leader to start."
                actionText: "Open the leader"
                onActionTriggered: screen.openConversation(screen.members[0].number)
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
                count: "1 leader · " + screen.count(screen.developers, "developer", "developers") + " · "
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
        KetMenuItem { text: "New team…"; iconName: "plus"; onTriggered: newTeamDialog.open() }
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
        KetMenuItem { text: "Disband team…"; iconName: "trash"; danger: true; onTriggered: disbandDialog.open() }
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
                onActionTriggered: { taskDialog.accept(); screen.openConversation(screen.members[0].number) }
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
        // 0 developer, 1 reviewer.
        property int role: 0
        property bool tried: false
        readonly property var free: {
            const taken = screen.members.map(m => m.number)
            const list = []
            for (const r of Fixtures.repositories)
                for (const c of r.conversations)
                    if (!taken.includes(c.number) && !list.some(m => m.number === c.number))
                        list.push(c)
            return list
        }
        readonly property var lanes: screen.members.filter(m => m.role === "developer").map(m => m.lane)
        title: "Add a member"
        onOpened: { tried = false; laneName.text = ""; lanePaths.text = "" }
        onClosed: role = 0
        Column {
            width: addMemberDialog.availableWidth
            spacing: screen.px(14)
            KetFieldRow {
                width: parent.width
                label: "Conversation"
                help: "A conversation belongs to one team at a time."
                KetSelect {
                    id: memberChoice
                    width: parent.width
                    model: addMemberDialog.free.map(c => "@agent/" + c.number + " · " + c.title)
                }
            }
            KetSegmentedControl {
                options: ["Developer", "Reviewer"]
                currentIndex: addMemberDialog.role
                onActivated: index => addMemberDialog.role = index
            }
            KetFieldRow {
                visible: addMemberDialog.role === 0
                width: parent.width
                label: "Lane"
                help: "The part of the code this developer owns."
                error: addMemberDialog.tried && laneName.text.trim() === "" ? "Name the lane." : ""
                KetTextField { id: laneName; width: parent.width; placeholderText: "Onboarding"; invalid: addMemberDialog.tried && text.trim() === "" }
            }
            KetFieldRow {
                visible: addMemberDialog.role === 0
                width: parent.width
                label: "Paths"
                help: "Folders separated by commas. An edit outside them is flagged on the task."
                error: addMemberDialog.tried && lanePaths.text.trim() === "" ? "Give at least one folder." : ""
                KetTextField { id: lanePaths; width: parent.width; placeholderText: "apps/desktop/src/onboarding/"; invalid: addMemberDialog.tried && text.trim() === "" }
            }
            Label {
                visible: addMemberDialog.role === 1
                width: parent.width
                text: addMemberDialog.lanes.length > 0
                    ? "Reviews " + addMemberDialog.lanes.join(", ") + " with the team's other reviewers. A reviewer never reviews its own work and never merges."
                    : "Reviews every lane once the team has developers. A reviewer never merges."
                color: KetTheme.t.textSecondary
                wrapMode: Text.Wrap
                elide: Text.ElideNone
                lineHeight: 1.35
            }
            KetBanner {
                visible: addMemberDialog.role === 0 && screen.developers >= 3 && screen.reviewers < 2
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
                enabled: addMemberDialog.free.length > 0
                onClicked: {
                    const developer = addMemberDialog.role === 0
                    addMemberDialog.tried = true
                    if (developer && (laneName.text.trim() === "" || lanePaths.text.trim() === ""))
                        return
                    const chosen = addMemberDialog.free[memberChoice.currentIndex]
                    screen.change({ members: screen.members.concat([{
                        number: chosen.number, title: chosen.title, role: developer ? "developer" : "reviewer",
                        provider: (chosen.provider || "Claude") + " · " + (chosen.model || "Opus 5.5"),
                        lane: developer ? laneName.text.trim() : "",
                        paths: developer ? lanePaths.text.split(",").map(p => p.trim()).filter(p => p) : [],
                        reviews: developer ? [] : addMemberDialog.lanes,
                        state: "idle", now: developer ? "No task yet" : "No review yet" }]) })
                    addMemberDialog.accept()
                    screen.window.notify("Added @agent/" + chosen.number + " as a " + (developer ? "developer" : "reviewer"))
                }
            }
        ]
    }

    KetDialog {
        id: newTeamDialog
        property bool tried: false
        readonly property var free: {
            const taken = []
            for (const t of screen.teams)
                for (const m of t.members)
                    taken.push(m.number)
            return Fixtures.repositories[0].conversations.filter(c => !taken.includes(c.number))
        }
        title: "New team"
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
            KetFieldRow {
                width: parent.width
                label: "Leader"
                help: "The one conversation that reports to you. It assigns and coordinates; it does not write code or review."
                KetSelect {
                    id: teamLeader
                    width: parent.width
                    model: newTeamDialog.free.map(c => "@agent/" + c.number + " · " + c.title)
                }
            }
            KetChoice {
                id: leaderWrites
                width: parent.width
                text: "The leader also writes the requirements"
                description: "It puts acceptance criteria on each task before assigning it. Turn this off when a separate conversation researches the requirements."
                checked: true
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: newTeamDialog.reject() },
            KetButton {
                text: "Create"
                variant: KetTheme.ButtonVariant.Primary
                enabled: newTeamDialog.free.length > 0
                onClicked: {
                    newTeamDialog.tried = true
                    const name = teamName.text.trim()
                    if (name === "")
                        return
                    const leader = newTeamDialog.free[teamLeader.currentIndex]
                    screen.teams = screen.teams.concat([{
                        id: "team-" + screen.teams.length, name: name, goal: teamGoal.text.trim() || "No goal yet.",
                        repository: Fixtures.repositories[0].name, branch: teamBranch.currentText, paused: false,
                        members: [{ number: leader.number, title: leader.title, role: "leader",
                                    provider: (leader.provider || "Claude") + " · " + (leader.model || "Opus 5.5"),
                                    lane: "", paths: [], state: "idle", now: "Waiting for its first task list" }],
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
        title: "Disband " + (screen.team ? screen.team.name : "") + "?"
        Label {
            width: disbandDialog.availableWidth
            text: "The team, its tasks and their history are removed. Every conversation, worktree, branch and pull request stays."
            color: KetTheme.t.textSecondary
            wrapMode: Text.Wrap
            elide: Text.ElideNone
            lineHeight: 1.4
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: disbandDialog.reject() },
            KetButton {
                text: "Disband"
                variant: KetTheme.ButtonVariant.Danger
                onClicked: {
                    const name = screen.team.name
                    const next = screen.teams.filter((t, i) => i !== screen.teamIndex)
                    screen.teamIndex = 0
                    screen.teams = next
                    disbandDialog.accept()
                    screen.window.notify("Disbanded " + name)
                }
            }
        ]
    }
}
