// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// The conversation's 44 px chat-local header: reference, title, status, worktree, reviewer,
// tags and the pane toggles.
Rectangle {
    id: header

    required property var window
    required property var conversation
    // The conversation's team and role, when it is a member of one (Main.teamOf).
    readonly property var membership: {
        const team = window && window.teamOf ? window.teamOf(conversation.number) : null
        const who = team ? team.members.find(m => m.number === conversation.number) : null
        return who ? { team: team, role: who.role } : null
    }
    property bool detailsOpen: false
    property var planRun: null
    property int status: conversation.status

    // The agent that reviews each finished turn and replies here, by conversation number.
    // A reviewer never merges. The running conversation starts with one, for the mock.
    property var reviewers: ({ "9410": "Claude · Opus 5.5" })
    readonly property string reviewer: reviewers[conversation.number] || ""
    readonly property var reviewCandidates: {
        const found = []
        for (const r of Fixtures.repositories)
            for (const c of r.conversations)
                if (c.number !== header.conversation.number && c.tags.some(t => t.label === "Review"))
                    found.push(c)
        return found
    }

    signal toggleDetails(string tab)

    function openReviewerMenu() { reviewerMenu.popup(reviewerButton, 0, reviewerButton.height) }
    function chooseReviewer(name) {
        const next = Object.assign({}, reviewers)
        if (name)
            next[conversation.number] = name
        else
            delete next[conversation.number]
        reviewers = next
        window.notify(name ? name + " reviews each finished turn of @agent/" + conversation.number
                           : "Turns finish without a review")
    }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function tone(status) {
        switch (status) {
        case KetTheme.ConversationStatus.Completed: return KetTheme.BadgeTone.Success
        case KetTheme.ConversationStatus.Working: return KetTheme.BadgeTone.Accent
        case KetTheme.ConversationStatus.NeedsInput: return KetTheme.BadgeTone.Warning
        case KetTheme.ConversationStatus.AwaitingReply: return KetTheme.BadgeTone.Info
        case KetTheme.ConversationStatus.Error: return KetTheme.BadgeTone.Danger
        }
        return KetTheme.BadgeTone.Neutral
    }

    implicitHeight: px(44)
    color: KetTheme.t.pageBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: KetTheme.t.panelBorder
    }

    // The header gives way from the right as the conversation narrows (the details pane takes
    // half the workspace): tags first, then the Reviewer label, then the title elides to its
    // minimum, then the worktree keeps only its icon, then the number (the sidebar shows it),
    // then Compact window and New conversation (the sidebar and the menus have both). With the
    // pane open, its Open content menu replaces the Changes, Terminal and Report openers.
    readonly property real gap: px(8)
    readonly property real room: width - px(16) - actions.width - px(10) - px(24)
    readonly property real minimumTitle: px(80)
    // Measured from the buttons, not the row, so hiding the global pair cannot feed back.
    readonly property int iconActions: 2 + (window.compact ? 0 : 1) + (detailsOpen ? 0 : 3)
    readonly property real fullActionsWidth: iconActions * compactButton.implicitWidth
        + (planButton.visible ? planButton.implicitWidth + actions.spacing : 0)
        + (iconActions - 1) * actions.spacing
    readonly property bool globalActions: window.compact
        || width - px(16) - fullActionsWidth - px(10) - px(24) >= statusBadge.implicitWidth
           + compactWorktreeProbe.implicitWidth + minimumTitle + compactReviewerProbe.implicitWidth + 3 * gap
    readonly property bool fullWorktree: numberLabel.implicitWidth + statusBadge.implicitWidth
                                         + worktreeProbe.implicitWidth + minimumTitle
                                         + compactReviewerProbe.implicitWidth + 4 * gap <= room
    readonly property bool showNumber: numberLabel.implicitWidth + statusBadge.implicitWidth
                                       + compactWorktreeProbe.implicitWidth + minimumTitle
                                       + compactReviewerProbe.implicitWidth + 4 * gap <= room
    readonly property real fixedWidth: (numberLabel.visible ? numberLabel.implicitWidth + gap : 0)
                                       + statusBadge.implicitWidth
                                       + (worktreeButton.visible ? worktreeButton.implicitWidth + gap : 0) + gap
    // A team's member is reviewed by its team's reviewer, so it has no Reviewer choice.
    readonly property real reviewerRoom: membership ? 0 : reviewerProbe.implicitWidth
    readonly property bool fullReviewer: fixedWidth + titleButton.implicitWidth + gap
                                         + reviewerRoom <= room
    readonly property bool showTags: fullReviewer && fixedWidth + titleButton.implicitWidth + gap
                                     + reviewerRoom + tagsProbe.implicitWidth <= room

    // Off-screen measures of the full Reviewer button and the tag group.
    Row {
        visible: false
        KetButton {
            id: worktreeProbe
            controlSize: KetTheme.ControlSize.Small
            iconName: "branch"
            text: worktreeButton.fullText
        }
        KetButton {
            id: compactWorktreeProbe
            controlSize: KetTheme.ControlSize.Small
            iconName: "branch"
            text: "▾"
        }
        KetButton {
            id: compactReviewerProbe
            controlSize: KetTheme.ControlSize.Small
            iconName: "eye"
            text: "▾"
        }
        KetButton {
            id: reviewerProbe
            controlSize: KetTheme.ControlSize.Small
            iconName: "eye"
            text: reviewerButton.fullText
        }
        Row {
            id: tagsProbe
            spacing: header.gap
            leftPadding: header.gap
            Repeater {
                model: header.conversation.tags
                KetTag { required property var modelData; text: modelData.label; tagColor: modelData.color }
            }
            KetIconButton { controlSize: KetTheme.ControlSize.Small; iconName: "plus"; text: "Add tag" }
        }
    }

    Row {
        x: header.px(16)
        anchors.verticalCenter: parent.verticalCenter
        spacing: header.gap

        Text {
            id: numberLabel
            visible: header.showNumber
            anchors.verticalCenter: parent.verticalCenter
            text: "@agent/" + header.conversation.number
            color: KetTheme.t.textMuted
            font.family: KetTheme.monoFontFamily
            font.pixelSize: header.px(14)
        }

        Basic.AbstractButton {
            id: titleButton
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(header.minimumTitle, Math.min(implicitWidth, header.room - header.fixedWidth - header.gap
                            - (reviewerButton.visible ? reviewerButton.implicitWidth : 0)))
            hoverEnabled: true
            text: header.conversation.title
            onDoubleClicked: header.window.renameDialog.open()
            KetToolTip { visible: titleButton.hovered; text: "Double-click to rename" }
            background: null
            contentItem: Text {
                text: titleButton.text
                elide: Text.ElideRight
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: header.px(15)
                font.weight: Font.Medium
            }
        }

        KetBadge {
            id: statusBadge
            anchors.verticalCenter: parent.verticalCenter
            text: KetTheme.conversationStatusLabel(header.status)
            tone: header.tone(header.status)
        }

        KetButton {
            id: worktreeButton
            visible: !header.window.compact
            anchors.verticalCenter: parent.verticalCenter
            controlSize: KetTheme.ControlSize.Small
            iconName: "branch"
            readonly property string fullText: header.conversation.worktree + "  ▾"
            text: header.fullWorktree ? fullText : "▾"
            onClicked: header.window.picker.open()
            KetToolTip {
                visible: worktreeButton.hovered && !header.fullWorktree
                text: "Worktree: " + header.conversation.worktree
            }
        }

        KetButton {
            id: reviewerButton
            visible: !header.window.compact && !header.membership
            anchors.verticalCenter: parent.verticalCenter
            controlSize: KetTheme.ControlSize.Small
            variant: header.reviewer ? KetTheme.ButtonVariant.Secondary : KetTheme.ButtonVariant.Ghost
            iconName: "eye"
            readonly property string fullText: (header.reviewer ? "Reviewer · " + header.reviewer : "Reviewer") + "  ▾"
            text: header.fullReviewer ? fullText : "▾"
            onClicked: header.openReviewerMenu()
            KetToolTip {
                visible: reviewerButton.hovered && !reviewerMenu.visible
                text: (header.fullReviewer ? "" : (header.reviewer ? "Reviewer: " + header.reviewer + ". " : "Reviewer. "))
                      + "An agent reviews each finished turn and replies to this conversation. Reviewers never merge."
            }
        }

        // A team member's role: the team sets it, so it has no way to remove it here.
        KetTag {
            id: roleTag
            visible: !!header.membership && !header.window.compact
            anchors.verticalCenter: parent.verticalCenter
            text: header.membership ? Fixtures.roleTag(header.membership.role).label : ""
            tagColor: header.membership ? Fixtures.roleTag(header.membership.role).color : "transparent"
            filterable: true
            onClicked: header.window.openTeam(header.membership.team.id)
            KetToolTip {
                visible: roleTag.hovered
                text: header.membership ? roleTag.text + " of " + header.membership.team.name
                      + ". The team sets this tag; change the role from the team's page." : ""
            }
        }

        Repeater {
            model: header.window.compact || !header.showTags ? [] : header.conversation.tags
            KetTag {
                required property var modelData
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.label
                tagColor: modelData.color
                filterable: true
                onClicked: header.window.tagFilter = modelData.label
            }
        }

        KetIconButton {
            id: addTag
            visible: !header.window.compact && header.showTags
            anchors.verticalCenter: parent.verticalCenter
            controlSize: KetTheme.ControlSize.Small
            iconName: "plus"
            text: "Add tag"
            onClicked: tagMenu.popup(addTag, 0, addTag.height)
        }
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: header.px(10)
        anchors.verticalCenter: parent.verticalCenter
        spacing: header.px(2)

        KetIconButton {
            id: compactButton
            visible: header.globalActions
            iconName: header.window.compact ? "fullscreen" : "panel-left"
            text: header.window.compact ? "Leave compact window" : "Compact window"
            onClicked: header.window.setCompact(!header.window.compact)
        }
        KetIconButton {
            id: newButton
            visible: header.globalActions
            iconName: "compose"
            text: "New conversation"
            onClicked: header.window.showScreen("workspace/welcome")
        }
        KetIconButton {
            visible: !header.window.compact
            iconName: "layers"
            text: "Atlases"
            checkable: true
            checked: header.detailsOpen
            onClicked: header.toggleDetails("atlas")
        }
        PlanProgressButton {
            id: planButton
            visible: !!header.conversation.hasPlan
            run: header.planRun
            onClicked: header.toggleDetails("plan")
        }
        KetIconButton {
            visible: !header.detailsOpen
            iconName: "compare"
            text: "Changes"
            onClicked: header.toggleDetails("changes")
        }
        KetIconButton {
            visible: !header.detailsOpen
            iconName: "terminal"
            text: "Terminal"
            onClicked: header.toggleDetails("terminal")
        }
        KetIconButton {
            visible: !header.detailsOpen
            iconName: "view-list"
            text: "Report"
            onClicked: header.toggleDetails("report")
        }
    }

    KetMenu {
        id: tagMenu
        KetMenuItem {
            visible: !!header.membership
            height: visible ? implicitHeight : 0
            enabled: false
            iconName: "lock"
            text: header.membership ? Fixtures.roleTag(header.membership.role).label : ""
            description: header.membership ? "Set by " + header.membership.team.name : ""
        }
        KetMenuSeparator { visible: !!header.membership; height: visible ? implicitHeight : 0 }
        Repeater {
            model: Object.keys(Fixtures.tagColors)
            KetMenuItem {
                required property string modelData
                text: modelData
                iconName: "tag"
                checkable: true
                checked: header.conversation.tags.some(t => t.label === modelData)
                onTriggered: header.window.notify((checked ? "Tagged " : "Removed tag ") + modelData)
            }
        }
        KetMenuSeparator {}
        KetMenuItem { text: "Manage tags…"; iconName: "settings"; onTriggered: header.window.notify("Tags are managed from any conversation") }
    }

    KetMenu {
        id: reviewerMenu
        KetMenuItem {
            text: "No reviewer"
            description: "Turns finish without a review"
            checkable: true
            checked: header.reviewer === ""
            onTriggered: header.chooseReviewer("")
        }
        KetMenuSection { text: "New review agent" }
        KetMenuItem {
            text: "Claude · Opus 5.5"
            description: "Starts a reviewer in this worktree, read only"
            iconName: "bot"
            checkable: true
            checked: header.reviewer === text
            onTriggered: header.chooseReviewer(text)
        }
        KetMenuItem {
            text: "Codex · GPT-5.6 Sol"
            description: "Starts a reviewer in this worktree, read only"
            iconName: "bot"
            checkable: true
            checked: header.reviewer === text
            onTriggered: header.chooseReviewer(text)
        }
        KetMenuSection { text: "Existing conversation" }
        // Appended in order after the section: a Repeater inside a Menu misplaces its items.
        Instantiator {
            model: header.reviewCandidates
            onObjectAdded: (index, object) => reviewerMenu.addItem(object)
            onObjectRemoved: (index, object) => reviewerMenu.removeItem(object)
            KetMenuItem {
                required property var modelData
                readonly property string label: "@agent/" + modelData.number
                text: modelData.title
                description: label + " · " + modelData.worktree
                iconName: "chat"
                checkable: true
                checked: header.reviewer === label
                onTriggered: header.chooseReviewer(label)
            }
        }
    }
}
