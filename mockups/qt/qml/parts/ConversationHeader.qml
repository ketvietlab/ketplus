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
    property bool detailsOpen: false
    readonly property int status: conversation.status

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
    // half the workspace): tags first, then the Reviewer label, then the title elides.
    readonly property real gap: px(8)
    readonly property real room: width - px(16) - actions.width - px(10) - px(24)
    readonly property real fixedWidth: numberLabel.implicitWidth + statusBadge.implicitWidth
                                       + (worktreeButton.visible ? worktreeButton.implicitWidth + gap : 0) + 2 * gap
    readonly property bool fullReviewer: fixedWidth + titleButton.implicitWidth + gap
                                         + reviewerProbe.implicitWidth <= room
    readonly property bool showTags: fullReviewer && fixedWidth + titleButton.implicitWidth + gap
                                     + reviewerProbe.implicitWidth + tagsProbe.implicitWidth <= room

    // Off-screen measures of the full Reviewer button and the tag group.
    Row {
        visible: false
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
            anchors.verticalCenter: parent.verticalCenter
            text: "@agent/" + header.conversation.number
            color: KetTheme.t.textMuted
            font.family: KetTheme.monoFontFamily
            font.pixelSize: header.px(14)
        }

        Basic.AbstractButton {
            id: titleButton
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(header.px(80), Math.min(implicitWidth, header.room - header.fixedWidth - header.gap
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
            text: header.conversation.worktree + "  ▾"
            onClicked: header.window.picker.open()
        }

        KetButton {
            id: reviewerButton
            visible: !header.window.compact
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
            iconName: header.window.compact ? "fullscreen" : "panel-left"
            text: header.window.compact ? "Leave compact window" : "Compact window"
            onClicked: header.window.setCompact(!header.window.compact)
        }
        KetIconButton {
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
        KetIconButton {
            iconName: "compare"
            text: "Changes"
            onClicked: header.toggleDetails("changes")
        }
        KetIconButton {
            iconName: "terminal"
            text: "Terminal"
            onClicked: header.toggleDetails("terminal")
        }
    }

    KetMenu {
        id: tagMenu
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
