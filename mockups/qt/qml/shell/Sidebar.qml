// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// The main sidebar: brand, Quick search, the four destinations, the machine and its
// repositories with their conversations, the update notice and the account.
Rectangle {
    id: sidebar

    required property var window
    property var expanded: ({})
    property var showAll: ({})
    // The conversation being dragged to a team's folder, and where the pointer holds it.
    property var dragged: null
    property string draggedRepository
    property point dragPoint
    property bool dragPreview: false
    readonly property real inset: KetTheme.px(6, KetTheme.zoom)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    // `--open=repo-menu|team-menu|row-menu` opens a sidebar menu for review captures.
    function developersOf(team) { return team.members.filter(m => m.role === "developer").length }
    function startDrag(conversation, repository) {
        draggedRepository = repository
        dragged = conversation
    }
    function finishDrag() {
        if (dragged && !dragPreview)
            ghost.Drag.drop()
        dragged = null
        dragPreview = false
    }
    // Window positions of a conversation's row and a team's folder, for --smoke's pointer drag.
    function findItem(root, name) {
        if (root.objectName === name)
            return root
        for (const child of root.children) {
            const found = findItem(child, name)
            if (found)
                return found
        }
        return null
    }
    // Folds the other teams, so the conversations below them are in view.
    function foldTeamsExcept(team) {
        for (const t of sidebar.window.teamList) {
            const folder = findItem(sidebar, "team-" + t.id)
            if (folder && t.id !== team)
                folder.expanded = false
        }
    }
    function dragCheckPoints(number, team) {
        const row = findItem(sidebar, "conversation-" + number)
        const folder = findItem(sidebar, "team-" + team)
        if (!row || !folder)
            return []
        const from = row.mapToItem(null, row.width / 2, row.height / 2)
        const to = folder.mapToItem(null, folder.width / 2, px(12))
        return [Math.round(from.x), Math.round(from.y), Math.round(to.x), Math.round(to.y)]
    }
    // What the tree is built on, checked by --smoke: a repository's name, the label of its
    // own conversations and their titles share a left edge; a team's name and its members'
    // titles the same; a team's icon stands over its members' marks; and every conversation
    // is one line high.
    function layoutProblems() {
        const problems = []
        const left = (item, x) => Math.round(item.mapToItem(sidebar, x, 0).x * 10) / 10
        const near = (a, b) => Math.abs(a - b) <= 0.5
        let rows = 0
        // A repository's heading is a sibling of what it heads; a team's folder contains it.
        const walk = (root, heading) => {
            for (const item of root.children) {
                const name = item.objectName || ""
                if (name.startsWith("repository-")) {
                    heading = { name: name, x: left(item, item.textLeft) }
                } else if (name.startsWith("team-")) {
                    walk(item, { name: name, x: left(item, item.textLeft),
                                 mark: left(item, item.rowLeft + px(4)) })
                } else if (name.startsWith("section-")) {
                    if (item.visible && heading && !near(left(item, item.textLeft), heading.x))
                        problems.push(name + " starts at " + left(item, item.textLeft)
                                      + ", its repository " + heading.name + " at " + heading.x)
                } else if (name.startsWith("conversation-") || name.startsWith("member-")) {
                    ++rows
                    const x = left(item, item.textLeft)
                    if (heading && !near(x, heading.x))
                        problems.push(name + " starts at " + x + ", its heading " + heading.name + " at " + heading.x)
                    if (heading && heading.mark !== undefined && !near(left(item, item.markLeft), heading.mark))
                        problems.push(name + " has its mark at " + left(item, item.markLeft)
                                      + ", its team's icon is at " + heading.mark)
                    if (!near(item.height, px(30)))
                        problems.push(name + " is " + item.height + " high: every row is one line")
                } else {
                    walk(item, heading)
                }
            }
        }
        walk(sidebar, null)
        if (!rows)
            problems.push("no conversation rows to check")
        return problems
    }
    function openMenu(name) {
        const repository = Fixtures.repositories[0]
        const team = sidebar.window.teamList[0]
        if (name === "repo-menu") {
            repoMenu.repository = repository.name
            repoMenu.popup(sidebar, px(150), px(300))
        } else if (name === "team-menu" && team) {
            teamMenu.team = team
            teamMenu.popup(sidebar, px(150), px(326))
        } else if (name === "repo-folded") {
            // Every repository folded: the headings alone say what runs and what waits.
            const next = {}
            for (const r of Fixtures.repositories)
                next[r.name] = false
            expanded = next
        } else if (name === "drag-member") {
            // A conversation held over the first team's folder.
            dragPoint = Qt.point(px(96), px(318))
            dragPreview = true
            startDrag(repository.conversations.find(c => c.number === "9410"), repository.name)
        } else if (name === "row-menu") {
            sidebar.window.repository = repository.name
            sidebar.window.conversation = repository.conversations.find(c => c.number === "9410")
            rowMenu.popup(sidebar, px(150), px(480))
        }
    }

    color: KetTheme.t.sidebarBg

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Brand.
        Row {
            Layout.leftMargin: sidebar.px(11)
            Layout.topMargin: sidebar.px(16)
            Layout.bottomMargin: sidebar.px(14)
            spacing: sidebar.px(6)

            Image {
                anchors.verticalCenter: parent.verticalCenter
                // The bare KétPlus Pro mark (Két Symbol, CC BY 4.0): two Chàm tones on light,
                // Chàm and a near-white plus on dark.
                source: "qrc:/qt/qml/KetPlusMock/assets/ketplus-mark-bare-"
                        + (KetTheme.dark ? "dark" : "light") + ".svg"
                sourceSize: Qt.size(width * 2, height * 2)
                width: sidebar.px(24)
                height: width
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.StyledText
                // The wordmark is Bold (700); "Pro" is Regular.
                text: "<b>Két<font color='" + KetTheme.t.accentText + "'>Plus</font></b> Pro"
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: sidebar.px(17)
                font.weight: Font.Normal
            }
        }

        // Quick search opens the command palette.
        Basic.AbstractButton {
            id: search
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.preferredHeight: sidebar.px(28)
            hoverEnabled: true
            text: "Quick search"
            Accessible.name: "Quick search"
            onClicked: sidebar.window.spotlight.open()

            background: Rectangle {
                radius: sidebar.px(6)
                color: search.hovered ? KetTheme.t.inputBgHover : KetTheme.t.inputBg
                border.color: search.visualFocus ? KetTheme.t.focusBorder : KetTheme.t.inputBorder
                border.width: search.visualFocus ? 2 : 1
            }

            contentItem: Item {
                KetIcon {
                    x: sidebar.px(9)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    box: sidebar.px(12)
                    color: KetTheme.t.textMuted
                }
                Text {
                    x: sidebar.px(29)
                    anchors.verticalCenter: parent.verticalCenter
                    text: search.text
                    font.weight: Font.Medium
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: sidebar.px(12)
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: sidebar.px(9)
                    anchors.verticalCenter: parent.verticalCenter
                    text: "⌘ K"
                    font.weight: Font.Medium
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: sidebar.px(11)
                }
            }
        }

        // Destinations.
        Column {
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.topMargin: sidebar.px(8)
            spacing: sidebar.px(2)

            Repeater {
                model: [
                    { id: "workspace", text: "Agent Workspace", icon: "chats" },
                    { id: "editor", text: "Files & Editor", icon: "folder" },
                    { id: "changes", text: "Source Control", icon: "branch", count: 3 },
                    { id: "connections", text: "Connections", icon: "network" }
                ]

                KetNavItem {
                    required property var modelData
                    width: parent.width
                    font.weight: Font.Medium
                    text: modelData.text
                    iconName: modelData.icon
                    count: modelData.count || -1
                    checkable: false
                    checked: sidebar.window.screen === modelData.id
                             || (modelData.id === "workspace"
                                 && ["atlas", "teams"].includes(sidebar.window.screen))
                    onClicked: sidebar.window.showScreen(modelData.id)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.topMargin: sidebar.px(8)
            Layout.preferredHeight: 1
            color: KetTheme.t.sidebarBorder
        }

        // The machine whose repositories the list shows.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: sidebar.px(40)

            Text {
                x: sidebar.px(14)
                anchors.verticalCenter: parent.verticalCenter
                text: sidebar.window.machine
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: sidebar.px(13)
                font.weight: Font.Medium
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: sidebar.px(12)
                anchors.verticalCenter: parent.verticalCenter
                spacing: sidebar.px(2)

                KetIconButton {
                    id: machineButton
                    controlSize: KetTheme.ControlSize.Small
                    iconName: "chevron-down"
                    text: "Choose machine"
                    onClicked: machineMenu.popup(machineButton, 0, machineButton.height)
                }
                KetIconButton {
                    controlSize: KetTheme.ControlSize.Small
                    iconName: "plus"
                    text: "Open repository"
                    onClicked: sidebar.window.picker.open()
                }
            }

            KetMenu {
                id: machineMenu
                Repeater {
                    model: Fixtures.machines
                    KetMenuItem {
                        required property string modelData
                        text: modelData
                        iconName: modelData === "This Mac" ? "display" : "terminal"
                        checkable: true
                        checked: sidebar.window.machine === modelData
                        onTriggered: sidebar.window.machine = modelData
                    }
                }
                KetMenuSeparator {}
                KetMenuItem {
                    text: "Manage connections"
                    iconName: "network"
                    onTriggered: sidebar.window.showScreen("connections")
                }
            }
        }

        // An active tag filter.
        Rectangle {
            visible: sidebar.window.tagFilter !== ""
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.bottomMargin: sidebar.px(6)
            Layout.preferredHeight: sidebar.px(32)
            radius: sidebar.px(6)
            color: KetTheme.t.accentSubtle
            border.color: KetTheme.t.accentBorder
            Row {
                x: sidebar.px(10)
                anchors.verticalCenter: parent.verticalCenter
                spacing: sidebar.px(6)
                KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "filter"; box: sidebar.px(12); color: KetTheme.t.accentText }
                Text { anchors.verticalCenter: parent.verticalCenter; text: "Tagged " + sidebar.window.tagFilter; color: KetTheme.t.accentText; font.family: KetTheme.fontFamily; font.pixelSize: sidebar.px(12); font.weight: Font.Medium }
            }
            KetIconButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                controlSize: KetTheme.ControlSize.Small
                iconName: "close"
                text: "Clear filter"
                onClicked: sidebar.window.tagFilter = ""
            }
        }

        // Repositories and conversations.
        Basic.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Column {
                width: parent.width

                Repeater {
                    model: sidebar.window.machine === "This Mac" ? Fixtures.repositories
                                                                 : Fixtures.repositories.slice(0, 1)

                    Column {
                        id: repo
                        required property var modelData
                        required property int index
                        readonly property bool open: sidebar.expanded[modelData.name] !== false
                        readonly property bool all: !!sidebar.showAll[modelData.name]
                        readonly property int limit: 7
                        // Its teams come first, as folders; a conversation of a team shows there
                        // and not among the repository's own.
                        readonly property var teams: sidebar.window.teamList.filter(t => t.repository === modelData.name)
                        readonly property var loose: {
                            const taken = []
                            for (const t of sidebar.window.teamList)
                                for (const m of t.members)
                                    taken.push(m.number)
                            return sidebar.window.conversationsOf(modelData.name).filter(c => !taken.includes(c.number))
                        }
                        // Those the tag filter keeps.
                        readonly property var matching: sidebar.window.tagFilter === "" ? loose
                            : loose.filter(c => c.tags.some(t => t.label === sidebar.window.tagFilter))
                        width: parent.width

                        SidebarRepoHeader {
                            id: repoHeader
                            objectName: "repository-" + repo.modelData.name
                            readonly property var every: sidebar.window.conversationsOf(repo.modelData.name)
                            function statusOf(c) {
                                return sidebar.window.planStatus ? sidebar.window.planStatus(c.number, c.status) : c.status
                            }
                            x: sidebar.inset
                            width: parent.width - 2 * sidebar.inset
                            name: repo.modelData.name
                            divider: repo.index > 0
                            expanded: repo.open
                            selected: sidebar.window.repository === repo.modelData.name
                            count: every.length
                            waiting: every.filter(c => [KetTheme.ConversationStatus.NeedsInput,
                                                        KetTheme.ConversationStatus.AwaitingReply,
                                                        KetTheme.ConversationStatus.Error].includes(statusOf(c))
                                                       || (!!c.unread && statusOf(c) === KetTheme.ConversationStatus.Completed)).length
                            onToggled: expanded => {
                                const next = Object.assign({}, sidebar.expanded)
                                next[repo.modelData.name] = expanded
                                sidebar.expanded = next
                            }
                            // New: a conversation, or a team (concept).
                            actionText: "New conversation or team"
                            onActionTriggered: {
                                repoMenu.repository = repo.modelData.name
                                repoMenu.popup()
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: {
                                    repoMenu.repository = repo.modelData.name
                                    repoMenu.popup()
                                }
                            }
                        }

                        // What the repository holds, along the guide line under its chevron:
                        // its teams, then the conversations that belong to no team.
                        Item {
                            visible: repo.open
                            width: repo.width
                            height: held.height

                            Rectangle {
                                x: sidebar.inset + repoHeader.guideLeft
                                width: 1
                                height: parent.height
                                color: KetTheme.t.sidebarBorder
                            }
                            Column {
                                id: held
                                width: parent.width

                                Repeater {
                                    model: repo.open ? repo.teams : []

                                    TeamFolder {
                                        required property var modelData
                                        required property int index
                                        x: sidebar.inset
                                        width: repo.width - 2 * sidebar.inset
                                        window: sidebar.window
                                        team: modelData
                                        conversations: sidebar.window.conversationsOf(repo.modelData.name)
                                        dropPreview: sidebar.dragPreview && index === 0
                                        onContextMenuRequested: conversation => {
                                            sidebar.window.conversation = conversation
                                            rowMenu.popup()
                                        }
                                        onMenuRequested: anchor => {
                                            teamMenu.team = modelData
                                            if (anchor)
                                                teamMenu.popup(anchor, 0, anchor.height)
                                            else
                                                teamMenu.popup()
                                        }
                                    }
                                }

                                // The label that tells them from the teams above; a repository
                                // without a team needs none.
                                SidebarSection {
                                    objectName: "section-" + repo.modelData.name
                                    visible: repo.teams.length > 0 && repo.matching.length > 0
                                    x: sidebar.inset
                                    width: repo.width - 2 * sidebar.inset
                                    count: repo.matching.length
                                    onActionTriggered: {
                                        sidebar.window.repository = repo.modelData.name
                                        sidebar.window.showScreen("workspace/welcome")
                                    }
                                }

                                Repeater {
                                    model: !repo.open ? []
                                           : repo.all || sidebar.window.tagFilter !== "" ? repo.matching
                                           : repo.matching.slice(0, repo.limit)

                                    SidebarRow {
                                        id: row
                                        required property var modelData
                                        unread: !!modelData.unread
                                        detail: modelData.from ? "from " + modelData.from : ""
                                        objectName: "conversation-" + modelData.number
                                        x: sidebar.inset
                                        width: repo.width - 2 * sidebar.inset
                                        text: modelData.title
                                        number: modelData.number
                                        tags: modelData.tags
                                        status: sidebar.window.planStatus ? sidebar.window.planStatus(modelData.number, modelData.status) : modelData.status
                                        checked: sidebar.window.screen === "workspace"
                                                 && sidebar.window.conversation.number === modelData.number
                                        worktree: modelData.worktree
                                        onClicked: sidebar.window.openConversation(repo.modelData.name,
                                                                                   modelData)
                                        // Dragged onto a team's folder, the conversation joins that team.
                                        DragHandler {
                                            target: null
                                            enabled: repo.teams.length > 0
                                            // The drop takes this row out of the list, so the sidebar
                                            // finishes the drag, not the row.
                                            onActiveChanged: {
                                                if (active)
                                                    sidebar.startDrag(row.modelData, repo.modelData.name)
                                                else
                                                    sidebar.finishDrag()
                                            }
                                            onCentroidChanged: if (active) sidebar.dragPoint = sidebar.mapFromItem(null, centroid.scenePosition)
                                        }
                                        TapHandler {
                                            acceptedButtons: Qt.RightButton
                                            onTapped: {
                                                sidebar.window.repository = repo.modelData.name
                                                sidebar.window.conversation = row.modelData
                                                rowMenu.popup()
                                            }
                                        }
                                    }
                                }

                                Basic.AbstractButton {
                                    visible: repo.open && !repo.all && sidebar.window.tagFilter === ""
                                             && repo.loose.length > repo.limit
                                    // Under the titles, and it says how many it adds.
                                    x: sidebar.inset + repoHeader.textLeft
                                    height: sidebar.px(28)
                                    hoverEnabled: true
                                    text: "Show " + (repo.loose.length - repo.limit) + " more"
                                    onClicked: {
                                        const next = Object.assign({}, sidebar.showAll)
                                        next[repo.modelData.name] = true
                                        sidebar.showAll = next
                                    }
                                    contentItem: Text {
                                        verticalAlignment: Text.AlignVCenter
                                        text: parent.text
                                        color: parent.hovered ? KetTheme.t.textMain : KetTheme.t.textSecondary
                                        font.family: KetTheme.fontFamily
                                        font.pixelSize: sidebar.px(12)
                                        font.weight: Font.Medium
                                    }
                                    background: null
                                }
                            }
                        }

                        Item { width: 1; height: sidebar.px(4) }
                    }
                }
            }
        }

        // The journal passed its size limit; the banner opens Settings → Storage.
        KetBanner {
            id: storageBanner
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.bottomMargin: sidebar.px(6)
            visible: sidebar.window.storageWarning
            tone: KetTheme.BannerTone.Warning
            title: "Journal is " + Fixtures.journalStorage.totalGb.toFixed(1) + " GB"
            message: "Over its " + Fixtures.journalStorage.limitGb + " GB limit"
            Accessible.role: Accessible.Button
            TapHandler { onTapped: sidebar.window.showScreen("settings/storage") }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        KetUpdatePill {
            Layout.fillWidth: true
            Layout.leftMargin: sidebar.inset
            Layout.rightMargin: sidebar.inset
            Layout.bottomMargin: sidebar.px(6)
            visible: sidebar.window.updateReady
            text: "Update ready"
            detail: "KétPlus Pro 1.8.0 · Restart to install"
            onClicked: sidebar.window.showScreen("settings/updates")
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: KetTheme.t.sidebarBorder
        }

        // Account.
        Basic.AbstractButton {
            id: account
            Layout.fillWidth: true
            Layout.preferredHeight: sidebar.px(48)
            hoverEnabled: true
            text: "An Nguyen"
            onClicked: accountMenu.popup(account, sidebar.px(8), -accountMenu.height)
            background: Rectangle {
                color: account.hovered ? KetTheme.t.surfaceHover : "transparent"
            }
            contentItem: Item {
                KetAvatar {
                    id: avatar
                    x: sidebar.px(13)
                    anchors.verticalCenter: parent.verticalCenter
                    name: account.text
                }
                Column {
                    x: avatar.x + avatar.width + sidebar.px(8)
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: account.text
                        color: KetTheme.t.textMain
                        font.family: KetTheme.fontFamily
                        font.pixelSize: sidebar.px(12)
                        font.weight: Font.Medium
                    }
                    Text {
                        text: "Personal workspace"
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: sidebar.px(10)
                        font.weight: Font.Medium
                    }
                }
                KetIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: sidebar.px(16)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron-down"
                    box: sidebar.px(14)
                    color: KetTheme.t.textMuted
                }
            }
        }
    }

    KetMenu {
        id: accountMenu
        KetMenuItem { text: "Settings"; iconName: "settings"; shortcut: "⌘ ,"; onTriggered: sidebar.window.showScreen("settings") }
        KetMenuItem { text: "Account & devices"; iconName: "smartphone"; onTriggered: sidebar.window.showScreen("settings/account") }
        KetMenuItem { text: "Compact window"; iconName: "panel-left"; shortcut: "⇧⌘M"; onTriggered: sidebar.window.setCompact(true) }
        KetMenuItem { text: "Check for updates"; iconName: "download"; onTriggered: sidebar.window.showScreen("settings/updates") }
        KetMenuSeparator {}
        KetMenuItem { text: "Sign out"; iconName: "log-out"; onTriggered: sidebar.window.notify("Signed out of the demo account") }
    }

    // A repository's New.
    KetMenu {
        id: repoMenu
        property string repository
        KetMenuItem {
            text: "New conversation"
            iconName: "compose"
            onTriggered: {
                sidebar.window.repository = repoMenu.repository
                sidebar.window.showScreen("workspace/welcome")
            }
        }
        KetMenuItem {
            text: "New team…"
            description: "A leader, developers and reviewers with one goal"
            iconName: "bot"
            onTriggered: sidebar.window.newTeam(repoMenu.repository)
        }
    }

    // A team folder's menu.
    KetMenu {
        id: teamMenu
        property var team: null
        KetMenuItem { text: "Open team"; iconName: "bot"; onTriggered: sidebar.window.openTeam(teamMenu.team.id) }
        KetMenuItem {
            readonly property bool full: !!teamMenu.team && sidebar.developersOf(teamMenu.team) >= Fixtures.mostDevelopers
            text: "Add member…"
            description: full ? "A team has at most " + Fixtures.mostDevelopers + " developers" : ""
            iconName: "plus"
            enabled: !full
            onTriggered: sidebar.window.addTeamMember(teamMenu.team.id, "")
        }
        KetMenuSeparator {}
        KetMenuItem { text: "Delete team…"; description: "Its conversations stay"; iconName: "trash"; danger: true; onTriggered: sidebar.window.deleteTeam(teamMenu.team.id) }
    }

    KetMenu {
        id: rowMenu
        // The conversation's team, and the teams of its repository it could join.
        readonly property var team: sidebar.window.teamOf(sidebar.window.conversation.number)
        // A team keeps its leader, its reviewer and at least one developer.
        readonly property bool leads: !!team && team.members.some(m => m.number === sidebar.window.conversation.number && m.role !== "developer")
        readonly property bool lastDeveloper: !!team && !leads && sidebar.developersOf(team) <= 1
        readonly property var joinable: sidebar.window.teamList.filter(t => t.repository === sidebar.window.repository)
        KetMenuItem { text: "Rename…"; iconName: "edit"; onTriggered: sidebar.window.renameDialog.open() }
        KetMenuItem { text: "Fork conversation"; iconName: "branch"; onTriggered: sidebar.window.notify("Forked into a new conversation") }
        KetMenuItem { text: "Change worktree…"; iconName: "branch"; onTriggered: sidebar.window.picker.open() }
        KetMenuItem { text: "Copy reference"; iconName: "copy"; shortcut: "@agent/" + sidebar.window.conversation.number; onTriggered: sidebar.window.notify("Copied @agent/" + sidebar.window.conversation.number) }
        KetMenuItem {
            visible: !!rowMenu.team
            height: visible ? implicitHeight : 0
            text: rowMenu.team ? "Remove from " + rowMenu.team.name + "…" : ""
            description: rowMenu.leads ? "A team keeps its leader and its reviewer"
                       : rowMenu.lastDeveloper ? "A team keeps at least one developer" : ""
            iconName: "bot"
            enabled: !rowMenu.leads && !rowMenu.lastDeveloper
            onTriggered: sidebar.window.removeTeamMember(rowMenu.team.id, sidebar.window.conversation.number)
        }
        // One entry for each team of the repository; the conversation joins as a developer.
        // Inserted in order: a Repeater inside a Menu misplaces its items.
        Instantiator {
            model: rowMenu.team ? [] : rowMenu.joinable
            onObjectAdded: (index, object) => rowMenu.insertItem(5 + index, object)
            onObjectRemoved: (index, object) => rowMenu.removeItem(object)
            KetMenuItem {
                required property var modelData
                readonly property bool full: sidebar.developersOf(modelData) >= Fixtures.mostDevelopers
                text: "Add to " + modelData.name
                description: full ? "Already has " + Fixtures.mostDevelopers + " developers" : "Joins as a developer"
                iconName: "bot"
                enabled: !full
                onTriggered: sidebar.window.addTeamMember(modelData.id, sidebar.window.conversation.number)
            }
        }
        KetMenuSeparator {}
        KetMenuItem { text: "Archive"; iconName: "archive"; onTriggered: sidebar.window.notify("Conversation archived") }
        KetMenuItem { text: "Delete…"; iconName: "trash"; danger: true; onTriggered: sidebar.window.deleteDialog.open() }
    }

    // What follows the pointer while a conversation is dragged.
    Rectangle {
        id: ghost
        visible: !!sidebar.dragged
        x: sidebar.dragPoint.x - sidebar.px(12)
        y: sidebar.dragPoint.y - height / 2
        z: 10
        width: Math.min(sidebar.width - sidebar.px(24), ghostText.implicitWidth + sidebar.px(20))
        height: sidebar.px(28)
        radius: sidebar.px(6)
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.focusBorder
        opacity: 0.95
        readonly property string number: sidebar.dragged ? sidebar.dragged.number : ""
        readonly property string repository: sidebar.draggedRepository
        Drag.active: !!sidebar.dragged && !sidebar.dragPreview
        Drag.keys: ["conversation"]
        Drag.source: ghost
        Drag.hotSpot: Qt.point(sidebar.px(12), height / 2)
        Text {
            id: ghostText
            anchors.verticalCenter: parent.verticalCenter
            x: sidebar.px(10)
            width: parent.width - sidebar.px(20)
            text: sidebar.dragged ? sidebar.dragged.title : ""
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: sidebar.px(13)
            elide: Text.ElideRight
        }
    }
}
