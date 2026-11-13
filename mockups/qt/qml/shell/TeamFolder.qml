// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// A team in the sidebar tree (concept): a branch of its repository, beside the conversations
// that belong to no team. The chevron folds it; the name opens the team's page; the count is
// its members, and a warning mark says that a decision waits for the user. Inside, the
// members' conversations: leader, developers, reviewers, along a guide line under the chevron.
// The kit has no tree row yet (a chevron that folds beside a name that activates): it comes
// first in ket-qt when the app builds this.
Column {
    id: folder
    objectName: "team-" + team.id

    required property var window
    required property var team
    // The repository's conversations, to draw each member's own row.
    required property var conversations
    property bool expanded: true
    // A dragged conversation is over the folder (`dropPreview` draws it for a capture).
    property bool dropPreview: false
    readonly property bool dropping: drop.containsDrag || dropPreview
    readonly property bool full: team.members.filter(m => m.role === "developer").length >= Fixtures.mostDevelopers
    readonly property bool selected: window.screen === "teams" && window.currentTeam === team.id
    readonly property bool hot: hover.hovered || fold.visualFocus || name.visualFocus
    readonly property var recipe: KetTheme.groupHeaderRecipe(
        KetTheme.dark, KetTheme.zoom, selected, hot, name.down, true, name.visualFocus)
    // The tree's columns, one level in from the repository's: the chevron stands under the
    // repository's folder, then the team's icon and its name. The highlight starts right of
    // the chevron, and the members' rows start there too, so their marks stand under the
    // team's icon and their titles under its name.
    readonly property real chevronLeft: px(26)
    readonly property real guideLeft: px(31)
    readonly property real rowLeft: px(38)
    readonly property real textLeft: px(62)
    // A decision waits for the user.
    readonly property bool waits: team.decisions.length > 0
    // Leader, developers, then reviewers. A tag filter keeps the members that carry the tag.
    readonly property var shown: ["leader", "developer", "reviewer"]
        .reduce((list, role) => list.concat(team.members.filter(m => m.role === role)), [])
    readonly property var roleTags: ({ leader: Fixtures.roleTag("leader"), developer: Fixtures.roleTag("developer"),
                                       reviewer: Fixtures.roleTag("reviewer") })

    signal contextMenuRequested(var conversation)
    // The folder's own menu, at `anchor` or at the pointer when it is null.
    signal menuRequested(var anchor)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    // A member's conversation; one Add a member started is not among the repository's yet.
    function conversation(number) {
        const who = team.members.find(m => m.number === number)
        const found = conversations.find(c => c.number === number)
        if (found)
            return found
        return { number: number, title: who ? who.title : "", worktree: team.branch,
                 status: KetTheme.ConversationStatus.NotStarted }
    }

    Item {
        width: folder.width
        height: folder.px(30)

        HoverHandler { id: hover }
        // A conversation of the repository dropped here joins the team as a developer.
        DropArea {
            id: drop
            anchors.fill: parent
            keys: ["conversation"]
            // Only a conversation of this repository, and only while the team has room.
            onEntered: drag => drag.accepted = drag.source.repository === folder.team.repository && !folder.full
            onDropped: drag => folder.window.addTeamMember(folder.team.id, drag.source.number)
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: folder.menuRequested(null)
        }

        Rectangle {
            x: folder.rowLeft
            width: parent.width - x
            height: parent.height
            radius: folder.px(6)
            color: folder.selected || folder.dropping ? KetTheme.t.navSelected
                   : name.hovered ? KetTheme.t.surfaceHover : "transparent"
            border.width: folder.dropping ? 1 : 0
            border.color: KetTheme.t.focusBorder
        }

        Basic.AbstractButton {
            id: fold
            x: folder.chevronLeft - folder.px(4)
            width: folder.px(18)
            height: parent.height
            checkable: true
            checked: folder.expanded
            onToggled: folder.expanded = checked
            Accessible.name: (folder.expanded ? "Fold " : "Unfold ") + folder.team.name
            contentItem: Item {
                KetIcon {
                    anchors.centerIn: parent
                    name: "chevron-down"
                    box: folder.px(10)
                    rotation: folder.expanded ? 0 : -90
                    color: KetTheme.t.textMuted
                }
            }
            background: Rectangle {
                radius: folder.px(KetTheme.t.radiusSm)
                color: fold.hovered ? KetTheme.t.surfaceHover : "transparent"
                border.width: fold.visualFocus ? 2 : 0
                border.color: KetTheme.t.focusBorder
            }
        }

        Basic.AbstractButton {
            id: name
            x: folder.rowLeft
            padding: 0
            width: actions.x - x - folder.px(4)
            height: parent.height
            hoverEnabled: true
            text: folder.team.name
            Accessible.name: "Team " + folder.team.name
            Accessible.description: folder.team.members.length
                                    + (folder.team.members.length === 1 ? " member" : " members")
                                    + (folder.waits ? ", " + folder.team.decisions.length
                                                      + " decisions wait for you" : "")
            onClicked: folder.window.openTeam(folder.team.id)
            contentItem: Item {
                KetIcon {
                    x: folder.px(4)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "users"
                    box: folder.px(14)
                    color: folder.selected ? KetTheme.t.accentText : KetTheme.t.textSecondary
                }
                Text {
                    x: folder.textLeft - folder.rowLeft
                    width: parent.width - x
                    anchors.verticalCenter: parent.verticalCenter
                    text: name.text
                    elide: Text.ElideRight
                    // A branch, not a conversation: the secondary colour and medium, until a
                    // decision waits for the user.
                    color: folder.selected ? KetTheme.t.accentText
                           : folder.waits ? KetTheme.t.textMain : KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: folder.px(13)
                    font.weight: folder.waits ? Font.DemiBold : Font.Medium
                }
            }
            background: Rectangle {
                radius: folder.px(6)
                color: "transparent"
                border.width: folder.recipe.focusRing.a > 0 ? 2 : 0
                border.color: folder.recipe.focusRing
            }
            KetToolTip {
                visible: name.hovered && !name.pressed
                delay: 1200
                // Outside the team page the goal keeps one line, cut at the end (ADR 0114).
                text: {
                    const line = String(folder.team.goal || "").split("\n")[0]
                    return line.length > 120 ? line.slice(0, 119) + "…" : line
                }
            }
        }

        // Add member and the team's menu show while hovered or focused, as a repository's
        // action does; the warning mark and the count stay.
        Row {
            id: actions
            anchors.right: parent.right
            anchors.rightMargin: folder.px(6)
            anchors.verticalCenter: parent.verticalCenter
            spacing: folder.px(4)
            readonly property bool shown: folder.hot || addButton.visualFocus || menuButton.visualFocus

            KetIconButton {
                id: addButton
                visible: actions.shown
                anchors.verticalCenter: parent.verticalCenter
                width: folder.recipe.actionSize
                height: folder.recipe.actionSize
                controlSize: KetTheme.ControlSize.Small
                iconName: "plus"
                enabled: !folder.full
                text: folder.full ? "A team has at most " + Fixtures.mostDevelopers + " developers" : "Add member"
                onClicked: folder.window.addTeamMember(folder.team.id, "")
            }
            KetIconButton {
                id: menuButton
                visible: actions.shown
                anchors.verticalCenter: parent.verticalCenter
                width: folder.recipe.actionSize
                height: folder.recipe.actionSize
                controlSize: KetTheme.ControlSize.Small
                iconName: "more"
                text: "Team actions"
                onClicked: folder.menuRequested(menuButton)
            }
            Item {
                visible: folder.window.scheduleStore.forTeam(folder.team.id).some(e => e.enabled)
                width: folder.px(16); height: folder.px(20); anchors.verticalCenter: parent.verticalCenter
                KetIcon { anchors.centerIn: parent; name: "clock"; box: folder.px(12); color: KetTheme.t.textMuted }
                HoverHandler { id: clockHover }
                KetToolTip { visible: clockHover.hovered; text: folder.window.teamScheduleHint(folder.team.id) }
            }
            KetIcon {
                id: waitsMark
                objectName: "team-waits"
                visible: folder.waits
                anchors.verticalCenter: parent.verticalCenter
                readonly property var mark: KetTheme.conversationStatusRecipe(
                    KetTheme.dark, KetTheme.ConversationStatus.NeedsInput)
                name: mark.icon
                box: folder.px(14)
                color: mark.iconColor
            }
            KetBadge {
                anchors.verticalCenter: parent.verticalCenter
                text: folder.team.members.length
            }
        }
    }

    // The members, along the guide line under the team's chevron.
    Item {
        visible: folder.expanded
        width: folder.width
        height: members.height

        Rectangle {
            x: folder.guideLeft
            width: 1
            height: parent.height
            color: KetTheme.t.sidebarBorder
        }
        Column {
            id: members
            width: parent.width

            Repeater {
                model: folder.expanded ? folder.shown : []

                SidebarRow {
                    id: row
                    objectName: "member-" + modelData.number
                    required property var modelData
                    readonly property var item: folder.conversation(modelData.number)
                    scheduleHint: folder.window.scheduleHint(item.number)
                    unread: !!item.unread
                    detail: item.from ? "from " + item.from : ""
                    width: folder.width
                    indent: folder.rowLeft
                    text: item.title
                    number: item.number
                    status: folder.window.planStatus ? folder.window.planStatus(item.number, item.status) : item.status
                    checked: folder.window.screen === "workspace"
                             && folder.window.conversation.number === item.number
                    role: folder.roleTags[modelData.role].label
                    worktree: modelData.role === "developer" ? modelData.lane + " · " + item.worktree
                                                             : item.worktree
                    onClicked: folder.window.openTeamMember(folder.team.id, item.number)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: folder.contextMenuRequested(row.item)
                    }
                }
            }

            Text {
                visible: folder.expanded && folder.team.members.length === 0
                x: folder.textLeft
                height: folder.px(24)
                verticalAlignment: Text.AlignVCenter
                text: "No member yet"
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: folder.px(12)
            }
        }
    }

    // Space before the next branch.
    Item { width: 1; height: folder.px(2); visible: folder.expanded }
}
