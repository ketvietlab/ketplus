import Ket
import QtQuick
import Ket as Kit
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
    // ADR 0114 (#467): a member's role, set by its team; it cannot be edited here.
    readonly property var teamRole: (agentHost.teamRoles || {})[selectedConversation.sessionId || ""] || null
    readonly property var selectedWorktree: {
        for (let candidate of agentHost.worktrees) {
            if (candidate.selected)
                return candidate;
        }
        return ({});
    }
    readonly property real maximumTitleWidth: Math.max(KetTheme.px(160, KetTheme.zoom), Math.min(KetTheme.px(420, KetTheme.zoom), width * 0.45))
    readonly property string scopeBranch: agentHost.executionBranch || selectedWorktree.branch || "workspace"
    // The header gives way from the right as it narrows, in the mock's order: a member's role
    // first, then the Reviewer label, then the title and the worktree elide, then the
    // worktree keeps only its icon, then the number leaves (the sidebar shows it), then Compact
    // window and New conversation (the sidebar and the menus have both). Each step is measured
    // from implicit widths, so hiding an item cannot feed back into its own test.
    readonly property real gap: titleRow.spacing
    readonly property real statusWidth: Math.min(conversationStatusBadge.Layout.preferredWidth, conversationStatusBadge.Layout.maximumWidth)
    readonly property real numberWidth: conversationNumber.number.length > 0 ? conversationNumber.implicitWidth + gap : 0
    readonly property real fixedWidth: numberWidth + Math.min(conversationTitle.implicitWidth, maximumTitleWidth) + gap + statusWidth + gap + (scopeChip.visible ? scopeChip.fullWidth + gap : 0)
    readonly property real minimumTitle: KetTheme.px(80, KetTheme.zoom)
    // What the row needs besides the worktree with the title at its minimum.
    readonly property real narrowWidth: numberWidth + minimumTitle + gap + statusWidth + gap + (reviewerButton.visible ? collapsedReviewerProbe.implicitWidth + gap : 0) + slack
    readonly property bool fullWorktree: titleRow.width >= narrowWidth + Math.min(scopeChip.fullWidth, minimumTitle) + gap
    readonly property bool showNumber: titleRow.width >= narrowWidth + scopeChip.iconWidth + gap
    // The row layout rounds every item to whole pixels, so a fit needs that much to spare.
    readonly property real slack: KetTheme.px(8, KetTheme.zoom)
    readonly property bool fullReviewer: titleRow.width >= fixedWidth + reviewerProbe.implicitWidth + gap + slack
    readonly property bool showRole: teamRole !== null && (!reviewerButton.visible || fullReviewer) && titleRow.width >= fixedWidth + (reviewerButton.visible ? reviewerProbe.implicitWidth + gap : 0) + roleChip.implicitWidth + gap + slack
    readonly property int actionCount: (agentHost.compactMode ? 0 : 1) + (agentHost.compactWindow ? 0 : 1) + (agentHost.compactMode || agentHost.compactWindow ? 0 : 4)
    readonly property real fullActionsWidth: actionCount * atlasButton.implicitWidth + Math.max(0, actionCount - 1) * headerActions.spacing
    readonly property bool globalActions: agentHost.compactWindow || width - titleRow.anchors.leftMargin - fullActionsWidth - headerActions.anchors.rightMargin - titleRow.anchors.rightMargin >= statusWidth + (scopeChip.visible ? scopeChip.iconWidth + gap : 0) + minimumTitle + (reviewerButton.visible ? collapsedReviewerProbe.implicitWidth + gap : 0) + 2 * gap
    // "Claude · Opus 5.5": the provider and its default model, when the catalog has it.
    function reviewerTitle(provider, title) {
        for (let model of agentHost.models)
            if (model.provider === provider && model.isDefault)
                return title + " · " + (model.displayName || model.model)
        return title
    }
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
            visible: number.length > 0 && (agentHost.compactWindow || header.showNumber)
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
            Layout.maximumWidth: Math.max(0, Math.min(header.maximumTitleWidth, titleRow.width - Math.min(conversationStatusBadge.Layout.preferredWidth, conversationStatusBadge.Layout.maximumWidth) - titleRow.spacing - (scopeChip.visible ? scopeChip.Layout.minimumWidth + titleRow.spacing : 0) - (reviewerButton.visible ? reviewerButton.implicitWidth + titleRow.spacing : 0) - (conversationNumber.visible ? conversationNumber.implicitWidth + titleRow.spacing : 0) - (roleChip.visible ? roleChip.implicitWidth + titleRow.spacing : 0) - titleRow.spacing))
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
            // The mock's KetBadge: the conversation's status in the kit's words and tone, read
            // from the same facts as its sidebar row. Long statuses still elide.
            // #125: a finished turn that asks the user to decide reads "Waiting for you" (Info).
            readonly property bool awaiting: !agentHost.sessionRunning && header.selectedConversation.awaitingReply === true
            readonly property string sessionText: String(agentHost.sessionStatus || "")
            readonly property int status: {
                const conversation = header.selectedConversation
                const text = String(conversation.status || sessionText).toLowerCase()
                if (conversation.needsInput === true || text.indexOf("input") >= 0 || text.indexOf("question") >= 0)
                    return KetTheme.ConversationStatus.NeedsInput
                if (awaiting)
                    return KetTheme.ConversationStatus.AwaitingReply
                if (agentHost.sessionRunning)
                    return KetTheme.ConversationStatus.Working
                if (/fail|error|disconnect|missing|not found/.test(text))
                    return KetTheme.ConversationStatus.Error
                if (text.indexOf("stop") >= 0)
                    return KetTheme.ConversationStatus.Stopped
                return conversation.started === true ? KetTheme.ConversationStatus.Completed : KetTheme.ConversationStatus.NotStarted
            }
            // A failed reply reads as the kit's "Reply failed"; another fault (a missing worktree,
            // a lost connection) and a conversation not started yet keep the session's own words.
            readonly property string label: status === KetTheme.ConversationStatus.Error && /disconnect|missing|not found/.test(String(header.selectedConversation.status || sessionText).toLowerCase())
                ? sessionText
                : status === KetTheme.ConversationStatus.NotStarted && sessionText.length > 0 ? sessionText
                : KetTheme.conversationStatusLabel(status)
            readonly property int tone: {
                switch (status) {
                case KetTheme.ConversationStatus.Completed: return KetTheme.BadgeTone.Success
                case KetTheme.ConversationStatus.Working: return KetTheme.BadgeTone.Accent
                case KetTheme.ConversationStatus.NeedsInput: return KetTheme.BadgeTone.Warning
                case KetTheme.ConversationStatus.AwaitingReply: return KetTheme.BadgeTone.Info
                case KetTheme.ConversationStatus.Error: return KetTheme.BadgeTone.Danger
                }
                return KetTheme.BadgeTone.Neutral
            }
            readonly property var recipe: KetTheme.badgeRecipe(KetTheme.dark, KetTheme.zoom, tone)
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
                text: conversationStatusBadge.label
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
            readonly property real fullWidth: scopeText.implicitWidth + chrome
            // The mock's narrow step: the icon and the chevron, the branch in the tooltip.
            readonly property real iconWidth: chrome - recipe.spacing
            readonly property bool iconOnly: !header.fullWorktree
            implicitWidth: iconOnly ? iconWidth : fullWidth
            Layout.preferredHeight: recipe.height
            Layout.preferredWidth: implicitWidth
            // Shrinks (its label elides) before the title and Reviewer button run out of room.
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            Layout.minimumWidth: Math.min(implicitWidth, header.minimumTitle)
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
                    visible: !scopeChip.iconOnly
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
            readonly property bool full: header.fullReviewer
            readonly property string fullText: (name ? "Reviewer · " + name : "Reviewer") + "  ▾"
            readonly property string detail: "An agent reviews each finished turn and replies to this conversation. Reviewers never merge."
            // The team's reviewer reviews a member's tasks (#460); its room goes to the title.
            visible: !agentHost.compactWindow && agentHost.reviewerState.available === true && !header.teamRole
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
        Kit.KetTag {
            id: roleChip
            objectName: "conversationRoleTag"
            visible: header.showRole
            text: header.teamRole ? header.teamRole.label : ""
            tagColor: (header.teamRole && header.teamRole.color) || KetTheme.t.accent
            maxWidth: KetTheme.px(100, KetTheme.zoom)
            locked: true
            filterable: false
            // The role opens its team's page; the kit never clicks a locked tag itself.
            Accessible.role: Accessible.Button
            Accessible.description: header.teamRole ? header.teamRole.label + " of " + header.teamRole.team_name + ". Opens the team." : ""
            Accessible.onPressAction: if (header.teamRole) agentHost.requestTeam(header.teamRole.team_id)
            MouseArea {
                id: roleMouse
                objectName: "conversationRoleTagArea"
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (header.teamRole) agentHost.requestTeam(header.teamRole.team_id)
            }
            KetToolTip {
                objectName: "roleTagToolTip"
                visible: roleMouse.containsMouse
                text: header.teamRole ? header.teamRole.label + " of " + header.teamRole.team_name : ""
            }
        }
        Item {
            Layout.fillWidth: true
        }
    }

    Row {
        id: headerActions
        anchors.right: parent.right
        anchors.rightMargin: KetTheme.px(10, KetTheme.zoom)
        anchors.verticalCenter: parent.verticalCenter
        spacing: KetTheme.px(2, KetTheme.zoom)

        Kit.KetIconButton {
            objectName: "headerCompactWindowButton"
            visible: !agentHost.compactMode && header.globalActions
            iconName: agentHost.compactWindow ? "fullscreen" : "panel-left"
            text: agentHost.compactWindow ? "Leave compact window" : "Compact window"
            onClicked: agentHost.requestCompactWindow()
        }
        Kit.KetIconButton {
            objectName: "headerNewConversationButton"
            visible: !agentHost.compactMode && !agentHost.compactWindow && header.globalActions
            iconName: "compose"
            text: "New conversation"
            onClicked: agentHost.requestNewSession()
        }
        Kit.KetIconButton {
            id: atlasButton
            objectName: "headerAtlasButton"
            visible: !agentHost.compactWindow
            iconName: "layers"
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
            objectName: "headerChangesButton"
            visible: !agentHost.compactMode && !agentHost.compactWindow
            iconName: "compare"
            text: "Changes"
            onClicked: agentHost.requestChanges()
        }
        Kit.KetIconButton {
            objectName: "headerTerminalButton"
            visible: !agentHost.compactMode && !agentHost.compactWindow
            iconName: "terminal"
            text: "Open terminal"
            onClicked: agentHost.requestTerminal()
        }
        Kit.KetIconButton {
            objectName: "headerWorkReportButton"
            visible: !agentHost.compactMode && !agentHost.compactWindow
            iconName: "view-list"
            text: "Work report"
            onClicked: agentHost.requestWorkReport()
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

    Kit.KetButton {
        id: collapsedReviewerProbe
        visible: false
        controlSize: Kit.KetTheme.ControlSize.Small
        iconName: "eye"
        text: "▾"
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
        // As the mock, each provider names the model a new reviewer starts with: the
        // provider's default, which the daemon uses.
        Kit.KetMenuItem {
            objectName: "newClaudeReviewerItem"
            text: header.reviewerTitle("claude", "Claude")
            description: "Starts a reviewer in this worktree, read only"
            iconName: "bot"
            onTriggered: agentHost.chooseReviewer("", "claude")
        }
        // ketplus#295: Codex reviews only with Full access, and a new reviewer starts read only,
        // so a new Codex reviewer is listed but cannot be chosen; pick a Codex conversation with
        // Full access below.
        Kit.KetMenuItem {
            objectName: "newCodexReviewerItem"
            text: header.reviewerTitle("codex", "Codex")
            description: "Codex can review only with Full access. Choose a Codex conversation with Full access below."
            iconName: "bot"
            enabled: false
        }
        Kit.KetMenuSection {
            text: "Existing conversation"
        }
        // Appended in order after the section: a Repeater inside a Menu misplaces its items.
        Instantiator {
            model: reviewerMenu.candidates.length > 0 ? reviewerMenu.candidates : [
                {
                    number: "",
                    title: "No other Claude or Codex conversation"
                }
            ]
            onObjectAdded: (index, object) => reviewerMenu.addItem(object)
            onObjectRemoved: (index, object) => reviewerMenu.removeItem(object)
            Kit.KetMenuItem {
                required property var modelData
                readonly property string reviewerNumber: String(modelData.number)
                objectName: "existingReviewerItem_" + reviewerNumber
                enabled: reviewerNumber.length > 0 && modelData.enabled !== false
                text: modelData.title
                description: reviewerNumber.length > 0 ? ["@agent/" + reviewerNumber, modelData.worktree, modelData.reason].filter(part => part).join(" · ") : ""
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
