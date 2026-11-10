// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// A team in the sidebar (concept): a folder inside its repository, beside the conversations
// that belong to no team. Its icon folds the folder; its name opens the team's page; the count
// is the decisions that wait for the user. Inside, the members' conversations: leader,
// developers, reviewers, each with its role as its first tag.
// It draws ket::groupHeaderRecipe as KetGroupHeader does. The kit has no header whose name
// activates instead of folding, and Két Symbol has no team icon yet: both come first in ket-qt
// when the app builds this.
Column {
    id: folder

    required property var window
    required property var team
    // The repository's conversations, to draw each member's own row.
    required property var conversations
    property bool expanded: true
    readonly property bool selected: window.screen === "teams" && window.currentTeam === team.id
    readonly property bool hot: hover.hovered || fold.visualFocus || name.visualFocus
    readonly property var recipe: KetTheme.groupHeaderRecipe(
        KetTheme.dark, KetTheme.zoom, selected, hot, name.down, true, name.visualFocus)
    readonly property real indent: KetTheme.px(12, KetTheme.zoom)
    // Leader, developers, then reviewers. A tag filter keeps the members that carry the tag.
    readonly property var shown: ["leader", "developer", "reviewer"]
        .reduce((list, role) => list.concat(team.members.filter(m => m.role === role)), [])
        .filter(m => window.tagFilter === "" || folder.conversation(m.number).tags.some(t => t.label === window.tagFilter))
    readonly property var roleTags: ({ leader: Fixtures.tag("Leader"), developer: Fixtures.tag("Developer"),
                                       reviewer: Fixtures.tag("Reviewer") })

    signal contextMenuRequested(var conversation)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function conversation(number) {
        return conversations.find(c => c.number === number)
            || { number: number, title: "", tags: [], status: KetTheme.ConversationStatus.NotStarted, worktree: "" }
    }

    visible: shown.length > 0

    Item {
        width: folder.width
        height: folder.recipe.height

        HoverHandler { id: hover }

        Rectangle {
            anchors.fill: parent
            radius: folder.px(KetTheme.t.radiusSm)
            color: folder.selected ? KetTheme.t.navSelected : "transparent"
        }

        Basic.AbstractButton {
            id: fold
            x: folder.recipe.paddingStart
            width: folder.recipe.iconSize + 2
            height: parent.height
            checkable: true
            checked: folder.expanded
            onToggled: folder.expanded = checked
            Accessible.name: (folder.expanded ? "Fold " : "Unfold ") + folder.team.name
            contentItem: Item {
                KetIcon {
                    x: 1
                    anchors.verticalCenter: parent.verticalCenter
                    name: "bot"
                    box: folder.recipe.iconSize
                    color: folder.selected ? KetTheme.t.accentText : folder.recipe.text
                }
            }
            background: Rectangle {
                radius: folder.px(KetTheme.t.radiusSm)
                color: "transparent"
                border.width: fold.visualFocus ? 2 : 0
                border.color: KetTheme.t.focusBorder
            }
        }

        Basic.AbstractButton {
            id: name
            x: fold.x + fold.width + folder.recipe.nameGap - 1
            width: parent.width - x - folder.recipe.paddingEnd - (waiting.visible ? waiting.width + folder.px(6) : 0)
            height: parent.height
            text: folder.team.name
            Accessible.name: "Team " + folder.team.name
            onClicked: folder.window.openTeam(folder.team.id)
            contentItem: Text {
                verticalAlignment: Text.AlignVCenter
                text: name.text
                elide: Text.ElideRight
                color: folder.selected ? KetTheme.t.accentText : folder.recipe.text
                font.family: KetTheme.fontFamily
                font.pixelSize: folder.recipe.fontPixels
                font.weight: folder.recipe.fontWeight
            }
            background: Rectangle {
                radius: folder.px(KetTheme.t.radiusSm)
                color: "transparent"
                border.width: folder.recipe.focusRing.a > 0 ? 2 : 0
                border.color: folder.recipe.focusRing
            }
            KetToolTip {
                visible: name.hovered && !name.pressed
                delay: 1200
                text: folder.team.goal
            }
        }

        KetBadge {
            id: waiting
            visible: folder.team.decisions.length > 0
            anchors.right: parent.right
            anchors.rightMargin: folder.recipe.paddingEnd
            anchors.verticalCenter: parent.verticalCenter
            text: folder.team.decisions.length
            tone: KetTheme.BadgeTone.Accent
            Accessible.name: folder.team.decisions.length + " decisions wait for you"
        }
    }

    Repeater {
        model: folder.expanded ? folder.shown : []

        KetConversationRow {
            id: row
            required property var modelData
            readonly property var item: folder.conversation(modelData.number)
            x: folder.indent
            width: folder.width - folder.indent
            text: item.title
            number: item.number
            tags: [folder.roleTags[modelData.role]].concat(item.tags)
            status: folder.window.planStatus ? folder.window.planStatus(item.number, item.status) : item.status
            checked: folder.window.screen === "workspace"
                     && folder.window.conversation.number === item.number
            property bool tipDismissed: false
            onHoveredChanged: if (!hovered) tipDismissed = false
            onPressedChanged: if (pressed) tipDismissed = true
            KetToolTip {
                visible: row.hovered && !row.pressed && !row.tipDismissed
                delay: 1200
                text: row.modelData.role === "developer" ? row.modelData.lane + " · " + row.item.worktree
                                                         : row.item.worktree
            }
            onClicked: folder.window.openConversation(folder.team.repository, item)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: folder.contextMenuRequested(row.item)
            }
        }
    }

    // Space before the repository's own conversations.
    Item { width: 1; height: folder.px(4); visible: folder.expanded }
}
