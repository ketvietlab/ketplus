import Ket
import QtQuick
import Ket as Kit
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: header
    required property var workspace
    objectName: "agentWorkspaceHeader"
    implicitHeight: KetTheme.px(44, KetTheme.zoom)

    readonly property var selectedConversation: {
        for (const conversation of agentHost.conversations || [])
            if (conversation.active)
                return conversation;
        return ({});
    }
    readonly property var tags: (selectedConversation.tags || []).map(tag => typeof tag === "string" ? {
            label: tag,
            color: KetTheme.t.accent
        } : tag)
    readonly property var selectedWorktree: {
        for (let candidate of agentHost.worktrees) {
            if (candidate.selected)
                return candidate;
        }
        return ({});
    }
    readonly property real maximumTitleWidth: Math.max(KetTheme.px(160, KetTheme.zoom), Math.min(KetTheme.px(420, KetTheme.zoom), width * 0.45))
    readonly property string scopeBranch: agentHost.executionBranch || selectedWorktree.branch || "workspace"
    readonly property string scopeLabel: (agentHost.repositoryName || "Repository") + " · " + scopeBranch

    // One compact row: title, status and a muted scope chip on the left, icon actions right.
    RowLayout {
        id: titleRow
        anchors.left: parent.left
        anchors.right: headerActions.left
        anchors.leftMargin: KetTheme.px(16, KetTheme.zoom)
        anchors.rightMargin: KetTheme.px(8, KetTheme.zoom)
        anchors.verticalCenter: parent.verticalCenter
        spacing: KetTheme.px(8, KetTheme.zoom)

        Text {
            id: conversationNumber
            objectName: "conversationNumber"
            // ADR 0027: the number other conversations use to address this one.
            readonly property string number: {
                const conversations = agentHost.conversations || [];
                for (let index = 0; index < conversations.length; ++index) {
                    if (conversations[index].active)
                        return conversations[index].number || "";
                }
                return "";
            }
            visible: number.length > 0
            // Jay's choice for agent references, as in the mock's header and the sidebar.
            text: "@agent/" + number
            color: KetTheme.t.textMuted
            font.family: agentHost.monoFontFamily
            font.pixelSize: KetTheme.px(14, KetTheme.zoom)
            Accessible.name: "Conversation number " + number
        }
        Text {
            id: conversationTitle
            objectName: "conversationTitle"
            // Long handoff titles must not push the status badge and scope chip away.
            // Everything else at its narrowest, each with its spacing; the trailing filler adds one more.
            Layout.maximumWidth: Math.max(0, Math.min(header.maximumTitleWidth, titleRow.width - Math.min(conversationStatusBadge.Layout.preferredWidth, conversationStatusBadge.Layout.maximumWidth) - titleRow.spacing - (scopeChip.visible ? scopeChip.Layout.minimumWidth + titleRow.spacing : 0) - (reviewerButton.visible ? reviewerButton.implicitWidth + titleRow.spacing : 0) - (conversationNumber.visible ? conversationNumber.implicitWidth + titleRow.spacing : 0) - (conversationTags.visible ? conversationTags.implicitWidth + titleRow.spacing : 0) - titleRow.spacing))
            text: agentHost.sessionTitle
            color: KetTheme.t.textMain
            elide: Text.ElideRight
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
            font.weight: Font.Medium
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Rectangle {
            id: conversationStatusBadge
            objectName: "conversationStatusBadge"
            // ket::badgeRecipe, as KetBadge draws the mock's status; long statuses still elide.
            readonly property var recipe: KetTheme.badgeRecipe(KetTheme.dark, KetTheme.zoom, agentHost.sessionRunning ? KetTheme.BadgeTone.Accent : KetTheme.BadgeTone.Neutral)
            Layout.preferredHeight: recipe.height
            Layout.preferredWidth: statusText.implicitWidth + 2 * recipe.paddingInline
            Layout.maximumWidth: KetTheme.px(120, KetTheme.zoom)
            radius: recipe.radius
            color: recipe.background
            border.width: 1
            border.color: recipe.border
            Text {
                id: statusText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 2 * conversationStatusBadge.recipe.paddingInline)
                text: agentHost.sessionRunning ? "Working" : agentHost.sessionStatus
                color: conversationStatusBadge.recipe.text
                font.family: KetTheme.fontFamily
                font.pixelSize: conversationStatusBadge.recipe.fontPixels
                font.weight: conversationStatusBadge.recipe.fontWeight
                elide: Text.ElideRight
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: statusText.text
        }
        // The mock's worktree KetButton (Secondary, Small): the branch, with the repository in its tooltip.
        Rectangle {
            id: scopeChip
            objectName: "workspaceScopeChip"
            readonly property var recipe: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Small, scopeMouse.containsMouse, scopeMouse.pressed, true, activeFocus)
            readonly property real chrome: 2 * recipe.paddingInline + recipe.iconSize + 2 * recipe.spacing + scopeChevron.implicitWidth
            visible: !agentHost.compactWindow
            // Measure the unelided text independently of the allocated chip width.
            implicitWidth: scopeText.implicitWidth + chrome
            Layout.preferredHeight: recipe.height
            Layout.preferredWidth: implicitWidth
            // Shrinks (its label elides) before the title and Reviewer button run out of room.
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            Layout.minimumWidth: Math.min(implicitWidth, KetTheme.px(80, KetTheme.zoom))
            radius: recipe.radius
            color: recipe.background
            border.width: 1
            border.color: recipe.focusRing.a > 0 ? recipe.focusRing : recipe.border
            opacity: recipe.opacity
            clip: true
            Row {
                id: scopeRow
                anchors.left: parent.left
                anchors.leftMargin: scopeChip.recipe.paddingInline
                anchors.verticalCenter: parent.verticalCenter
                spacing: scopeChip.recipe.spacing
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: scopeChip.recipe.iconSize
                    height: scopeChip.recipe.iconSize
                    name: "branch"
                    color: scopeChip.recipe.text
                    box: scopeChip.recipe.iconSize
                }
                Text {
                    id: scopeText
                    objectName: "workspaceScopeLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(0, scopeChip.width - scopeChip.chrome))
                    text: header.scopeBranch
                    color: scopeChip.recipe.text
                    elide: Text.ElideMiddle
                    font.family: KetTheme.fontFamily
                    font.pixelSize: scopeChip.recipe.fontPixels
                    font.weight: scopeChip.recipe.fontWeight
                }
                Text {
                    id: scopeChevron
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\u25BE"
                    color: scopeChip.recipe.text
                    font.family: KetTheme.fontFamily
                    font.pixelSize: scopeChip.recipe.fontPixels
                }
            }
            MouseArea {
                id: scopeMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: agentHost.requestExecutionWorktree()
            }
            KetToolTip {
                objectName: "ketToolTip"
                visible: scopeMouse.containsMouse
                text: header.scopeLabel
            }
            Accessible.role: Accessible.Button
            Accessible.name: "Choose Files and Source Control worktree"
            Accessible.description: header.scopeLabel
            Accessible.onPressAction: agentHost.requestExecutionWorktree()
        }
        Kit.KetButton {
            id: reviewerButton
            objectName: "headerReviewerButton"
            // ADR 0067: the conversation that reviews each finished turn and replies here.
            readonly property var assigned: agentHost.reviewerState.reviewer || null
            readonly property string name: assigned ? "@agent/" + assigned.number : ""
            // As in the mock, the Reviewer label collapses before the title or scope elide. It
            // counts every visible item and the spacing before the trailing filler.
            readonly property bool full: titleRow.width >= (conversationNumber.visible ? conversationNumber.implicitWidth + titleRow.spacing : 0) + conversationTitle.implicitWidth + conversationStatusBadge.width + (scopeChip.visible ? scopeChip.implicitWidth + titleRow.spacing : 0) + (conversationTags.visible ? conversationTags.implicitWidth + titleRow.spacing : 0) + reviewerProbe.implicitWidth + titleRow.spacing * 3
            readonly property string fullText: (name ? "Reviewer · " + name : "Reviewer") + "  ▾"
            readonly property string detail: "An agent reviews each finished turn and replies to this conversation. Reviewers never merge."
            visible: !agentHost.compactWindow && agentHost.reviewerState.available === true
            enabled: agentHost.reviewerState.pending !== true
            controlSize: Kit.KetTheme.ControlSize.Small
            variant: assigned ? Kit.KetTheme.ButtonVariant.Secondary : Kit.KetTheme.ButtonVariant.Ghost
            iconName: "eye"
            text: full ? fullText : "▾"
            onClicked: header.openReviewerMenu()
            KetToolTip {
                objectName: "reviewerToolTip"
                visible: reviewerButton.hovered && !reviewerMenu.visible
                text: agentHost.reviewerState.error ? agentHost.reviewerState.error : (reviewerButton.full ? "" : (reviewerButton.assigned ? "Reviewer: " + reviewerButton.name + ". " : "Reviewer. ")) + reviewerButton.detail
            }
            Accessible.name: assigned ? "Reviewer: " + name : "Reviewer"
            Accessible.description: detail
        }
        Row {
            id: conversationTags
            visible: header.tags.length > 0 && header.width > KetTheme.px(760, KetTheme.zoom)
            spacing: KetTheme.px(4, KetTheme.zoom)
            Repeater {
                model: header.tags.slice(0, 2)
                Kit.KetTag {
                    required property var modelData
                    objectName: "conversationTag_" + modelData.label
                    text: modelData.label
                    tagColor: modelData.color || KetTheme.t.accent
                    maxWidth: KetTheme.px(100, KetTheme.zoom)
                    filterable: true
                    onClicked: agentHost.requestTagFilter(text)
                }
            }
        }
        Item {
            Layout.fillWidth: true
        }
    }

    Row {
        id: headerActions
        anchors.right: parent.right
        anchors.rightMargin: KetTheme.px(8, KetTheme.zoom)
        anchors.verticalCenter: parent.verticalCenter
        spacing: KetTheme.px(2, KetTheme.zoom)

        Kit.KetIconButton {
            objectName: "headerCompactWindowButton"
            visible: !agentHost.compactMode
            controlSize: Kit.KetTheme.ControlSize.Small
            iconName: agentHost.compactWindow ? "fullscreen" : "panel-left"
            text: agentHost.compactWindow ? "Leave compact window" : "Compact window"
            onClicked: agentHost.requestCompactWindow()
        }
        Kit.KetIconButton {
            id: atlasButton
            objectName: "headerAtlasButton"
            visible: !agentHost.compactWindow
            controlSize: Kit.KetTheme.ControlSize.Small
            iconName: Kit.KetTheme.iconForLucide("network")
            text: agentHost.atlasRunningCount > 0 ? "Atlases, " + agentHost.atlasRunningCount + " running across worktrees" : "Atlases"
            onClicked: agentHost.requestProductNavigation("atlas")
            Text {
                objectName: "headerAtlasRunningBadge"
                visible: agentHost.atlasRunningCount > 0
                anchors.right: parent.right
                anchors.top: parent.top
                text: agentHost.atlasRunningCount
                color: KetTheme.t.accent
                font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
            }
        }
        Kit.KetIconButton {
            objectName: "headerNewConversationButton"
            visible: !agentHost.compactMode && !agentHost.compactWindow
            controlSize: Kit.KetTheme.ControlSize.Small
            iconName: Kit.KetTheme.iconForLucide("square-pen")
            text: "New conversation"
            onClicked: agentHost.requestNewSession()
        }
        Kit.KetIconButton {
            objectName: "headerTerminalButton"
            visible: !agentHost.compactMode && !agentHost.compactWindow
            controlSize: Kit.KetTheme.ControlSize.Small
            iconName: Kit.KetTheme.iconForLucide("square-terminal")
            text: "Open terminal"
            onClicked: agentHost.requestTerminal()
        }
    }

    // Off-screen measure of the full Reviewer button.
    Kit.KetButton {
        id: reviewerProbe
        visible: false
        controlSize: Kit.KetTheme.ControlSize.Small
        iconName: "eye"
        text: reviewerButton.fullText
    }

    function openReviewerMenu() {
        reviewerMenu.candidates = agentHost.reviewerCandidates();
        reviewerMenu.popup(reviewerButton, 0, reviewerButton.height);
    }

    Kit.KetMenu {
        id: reviewerMenu
        objectName: "reviewerMenu"
        property var candidates: []
        readonly property string assignedNumber: reviewerButton.assigned ? String(reviewerButton.assigned.number) : ""
        // Checks are set on every open: triggering a checkable item replaces a binding.
        onAboutToShow: {
            noReviewerItem.checked = assignedNumber.length === 0;
            for (let index = 0; index < count; ++index) {
                const item = itemAt(index);
                if (item && item.reviewerNumber)
                    item.checked = item.reviewerNumber === assignedNumber;
            }
        }
        Kit.KetMenuItem {
            id: noReviewerItem
            objectName: "noReviewerItem"
            text: "No reviewer"
            description: "Turns finish without a review"
            checkable: true
            onTriggered: agentHost.chooseReviewer("", "")
        }
        Kit.KetMenuSection {
            text: "New review agent"
        }
        Kit.KetMenuItem {
            objectName: "newClaudeReviewerItem"
            text: "Claude"
            description: "Starts a reviewer in this worktree, read only"
            iconName: "bot"
            onTriggered: agentHost.chooseReviewer("", "claude")
        }
        Kit.KetMenuSection {
            text: "Existing conversation"
        }
        // Appended in order after the section: a Repeater inside a Menu misplaces its items.
        Instantiator {
            model: reviewerMenu.candidates.length > 0 ? reviewerMenu.candidates : [
                {
                    number: "",
                    title: "No other Claude conversation"
                }
            ]
            onObjectAdded: (index, object) => reviewerMenu.addItem(object)
            onObjectRemoved: (index, object) => reviewerMenu.removeItem(object)
            Kit.KetMenuItem {
                required property var modelData
                readonly property string reviewerNumber: String(modelData.number)
                objectName: "existingReviewerItem_" + reviewerNumber
                enabled: reviewerNumber.length > 0
                text: modelData.title
                description: reviewerNumber.length > 0 ? ["@agent/" + reviewerNumber, modelData.worktree].filter(part => part).join(" · ") : ""
                iconName: "chat"
                checkable: reviewerNumber.length > 0
                onTriggered: agentHost.chooseReviewer(reviewerNumber, "")
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: KetTheme.t.panelBorder
    }
}
