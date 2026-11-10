import Ket
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket as Kit

// Product composition of the kit recipe retains IME, attachment-only sends,
// steering, persisted drafts and the existing skill/command completion contracts.
Item {
    id: area
    required property var workspace
    required property var modelPopup
    required property var imagePreview
    required property var usageLimitsPopover
    readonly property var root: workspace
    property alias prompt: prompt
    property alias permissionMenu: permissionMenu
    property alias skillsPopup: skillsPopup
    property alias modelButton: modelButton
    property alias usageRingButton: usageRingButton
    Layout.fillWidth: true
    // As ket's KetComposer: the focus ring surrounds the card, inside the composer's own box.
    Layout.preferredHeight: composer.height + 2 * composer.recipe.ringWidth + composerFooter.height + KetTheme.px(18, KetTheme.zoom)

    Rectangle {
        id: composer
        readonly property var recipe: Kit.KetTheme.composerRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom, prompt.activeFocus)
        objectName: "agentComposer"
        readonly property bool compactControls: width < 520
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: recipe.ringWidth
        width: Math.min(parent.width - KetTheme.px(48, KetTheme.zoom), root.conversationColumnWidth) - 2 * recipe.ringWidth
        height: composerColumn.implicitHeight + 2 * (recipe.padding + 1)
        radius: recipe.radius
        color: recipe.background
        border.width: 1
        border.color: recipe.border
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.margins: -composer.recipe.ringWidth
            radius: composer.radius + composer.recipe.ringWidth
            color: composer.recipe.ring
        }
        Kit.KetShadow {
            z: -2
            anchors.fill: parent
            visible: composer.recipe.ring.a === 0
            radius: composer.recipe.radius
            shadowX: composer.recipe.shadowX
            shadowY: composer.recipe.shadowY
            shadowBlur: composer.recipe.shadowBlur
            shadowColor: composer.recipe.shadowColor
        }

        DropArea {
            anchors.fill: parent
            onDropped: function (drop) {
                if (drop.hasUrls) {
                    let paths = [];
                    for (let url of drop.urls)
                        paths.push(url.toString());
                    agentHost.addAttachments(paths);
                    drop.acceptProposedAction();
                }
            }
        }
        ColumnLayout {
            id: composerColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: composer.recipe.padding + 1
            spacing: 0

            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: childrenRect.height
                visible: agentHost.attachments.length > 0
                spacing: KetTheme.px(6, KetTheme.zoom)
                Repeater {
                    model: agentHost.attachments
                    AgentAttachmentChip {
                        workspace: area.workspace
                        imagePreview: area.imagePreview
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 0
                Basic.ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(composer.recipe.maxLines * Math.floor(composer.recipe.textPixels * composer.recipe.lineHeight), Math.max(composer.recipe.minInputHeight, prompt.implicitHeight))
                    clip: true
                    Basic.TextArea {
                        id: prompt
                        objectName: "agentPrompt"
                        leftPadding: composer.recipe.textPaddingInline
                        rightPadding: composer.recipe.textPaddingInline
                        topPadding: composer.recipe.textPaddingBlock - leadingShift
                        bottomPadding: composer.recipe.textPaddingBlock + leadingShift
                        // As in the mock's composer: a new conversation asks what to build, later turns name the provider.
                        property string hint: agentHost.presentationState.folder === "missing" ? "Continue in the main worktree…"
                                              : root.messageCount === 0 ? "Describe what you want to build…"
                                              : "Message " + root.providerTitle(root.selectedProvider) + "…"
                        // Drafting must remain available even while the
                        // transport reconnects; submission is gated separately.
                        enabled: true
                        color: composer.recipe.text
                        placeholderTextColor: composer.recipe.placeholder
                        selectionColor: KetTheme.t.accentMuted
                        selectedTextColor: KetTheme.t.textMain
                        wrapMode: TextArea.Wrap
                        background: null
                        font.family: KetTheme.fontFamily
                        font.pixelSize: composer.recipe.textPixels
                        Text {
                            id: composerPlaceholder
                            objectName: "composerPlaceholder"
                            x: prompt.leftPadding
                            width: prompt.width - prompt.leftPadding - prompt.rightPadding
                            text: prompt.hint
                            color: composer.recipe.placeholder
                            font: prompt.font
                            elide: Text.ElideRight
                            visible: !prompt.length && !prompt.preeditText
                        }
                        property real leadingShift: 0
                        property string lastObservedText: ""
                        onTextChanged: {
                            // QTextDocument formatting also emits textChanged.
                            // Save/reformat only after an actual text edit.
                            if (text === lastObservedText)
                                return;
                            lastObservedText = text;
                            root.scheduleDraftSave();
                            composerLineHeightTimer.restart();
                        }
                        onFontChanged: composerLineHeightTimer.restart()
                        onWidthChanged: composerLineHeightTimer.restart()
                        Component.onCompleted: composerLineHeightTimer.restart()
                        Timer {
                            id: composerLineHeightTimer
                            objectName: "composerLineHeightTimer"
                            interval: 0
                            onTriggered: {
                                agentHost.formatComposerDocument(prompt.textDocument, prompt.font.pixelSize);
                                const line = prompt.positionToRectangle(0);
                                const lineBox = Math.floor(prompt.font.pixelSize * composer.recipe.lineHeight);
                                prompt.leadingShift = line.y - prompt.topPadding - (lineBox - line.height) / 2;
                                composerPlaceholder.y = prompt.positionToRectangle(0).y;
                            }
                        }
                        Keys.onPressed: function (event) {
                            if (event.matches(StandardKey.Paste) && agentHost.pasteClipboardImage()) {
                                event.accepted = true;
                                return;
                            }
                            if (root.visibleSkillSuggestions.length > 0) {
                                if (event.key === Qt.Key_Down) {
                                    root.selectedSkillSuggestion = Math.min(root.visibleSkillSuggestions.length - 1, root.selectedSkillSuggestion + 1);
                                    event.accepted = true;
                                    return;
                                }
                                if (event.key === Qt.Key_Up) {
                                    root.selectedSkillSuggestion = Math.max(0, root.selectedSkillSuggestion - 1);
                                    event.accepted = true;
                                    return;
                                }
                                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                    const index = Math.min(root.selectedSkillSuggestion, root.visibleSkillSuggestions.length - 1);
                                    root.applySkillSuggestion(root.visibleSkillSuggestions[index]);
                                    event.accepted = true;
                                    return;
                                }
                            }
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !prompt.inputMethodComposing) {
                                root.submitCurrentPrompt();
                                event.accepted = true;
                            }
                        }
                    }
                }
                AgentComposerAction {
                    id: stopButton
                    objectName: "stopButton"
                    stop: true
                    visible: agentHost.sessionRunning && !prompt.text.trim().length && !agentHost.attachments.length
                    text: "Stop turn"
                    onClicked: agentHost.stopAgent()
                }
                AgentComposerAction {
                    id: sendButton
                    objectName: "sendButton"
                    visible: !stopButton.visible
                    enabled: agentHost.agentAvailable && !agentHost.steeringPending && (prompt.text.trim().length > 0 || agentHost.attachments.length > 0)
                    text: agentHost.sessionRunning ? "Steer current turn" : "Send message"
                    onClicked: root.submitCurrentPrompt()
                }
            }
        }
    }

    // Suggestion popovers float above the composer so the transcript never moves.
    Rectangle {
        id: slashPalette
        objectName: "slashPalette"
        anchors.left: composer.left
        anchors.right: composer.right
        anchors.bottom: composer.top
        anchors.bottomMargin: KetTheme.px(6, KetTheme.zoom)
        z: 20
        height: slashCommandList.implicitHeight + KetTheme.px(8, KetTheme.zoom)
        visible: root.visibleSlashCommands.length > 0 && !agentHost.sessionRunning
        radius: KetTheme.px(12, KetTheme.zoom)
        color: KetTheme.t.surfaceRaised
        border.width: 1
        border.color: KetTheme.t.borderStrong
        Column {
            id: slashCommandList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: KetTheme.px(4, KetTheme.zoom)
            spacing: KetTheme.px(2, KetTheme.zoom)
            Repeater {
                model: root.visibleSlashCommands
                Basic.ItemDelegate {
                    required property var modelData
                    width: slashCommandList.width
                    height: KetTheme.px(42, KetTheme.zoom)
                    padding: KetTheme.px(8, KetTheme.zoom)
                    contentItem: RowLayout {
                        spacing: KetTheme.px(10, KetTheme.zoom)
                        Text {
                            text: modelData.label
                            color: KetTheme.t.textMain
                            font.family: agentHost.monoFontFamily
                            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                            Layout.preferredWidth: KetTheme.px(92, KetTheme.zoom)
                            verticalAlignment: Text.AlignVCenter
                        }
                        Text {
                            text: modelData.description
                            color: KetTheme.t.textMuted
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    background: Rectangle {
                        radius: KetTheme.px(8, KetTheme.zoom)
                        color: parent.hovered ? KetTheme.t.interactiveActive : "transparent"
                    }
                    onClicked: root.applySlashCommand(modelData)
                }
            }
        }
    }

    Rectangle {
        id: skillMentionPalette
        objectName: "skillMentionPalette"
        anchors.left: composer.left
        anchors.right: composer.right
        anchors.bottom: composer.top
        anchors.bottomMargin: KetTheme.px(6, KetTheme.zoom)
        z: 20
        height: Math.min(KetTheme.px(360, KetTheme.zoom), skillSuggestionList.contentHeight + KetTheme.px(8, KetTheme.zoom))
        visible: root.visibleSkillSuggestions.length > 0
        radius: KetTheme.px(12, KetTheme.zoom)
        color: KetTheme.t.surfaceRaised
        border.width: 1
        border.color: KetTheme.t.borderStrong

        ListView {
            id: skillSuggestionList
            objectName: "skillSuggestionList"
            anchors.fill: parent
            anchors.margins: KetTheme.px(4, KetTheme.zoom)
            clip: true
            spacing: KetTheme.px(2, KetTheme.zoom)
            model: root.visibleSkillSuggestions
            currentIndex: Math.min(root.selectedSkillSuggestion, Math.max(0, count - 1))

            delegate: Basic.ItemDelegate {
                id: suggestionRow
                required property var modelData
                required property int index
                readonly property bool fileEntry: modelData.kind === "file"
                width: skillSuggestionList.width
                // A file shows its worktree path, then whether it is a file or folder.
                height: KetTheme.px(46, KetTheme.zoom)
                padding: KetTheme.px(8, KetTheme.zoom)
                highlighted: index === skillSuggestionList.currentIndex
                contentItem: Item {
                    ColumnLayout {
                        objectName: "fileMentionSuggestion"
                        anchors.fill: parent
                        visible: suggestionRow.fileEntry
                        spacing: 1
                        Text {
                            objectName: "fileMentionPath"
                            Layout.fillWidth: true
                            text: (suggestionRow.modelData.path || "").replace(/\/$/, "")
                            color: KetTheme.t.accentHover
                            font.family: agentHost.monoFontFamily
                            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                            // Long paths keep both the top folder and the file name.
                            elide: Text.ElideMiddle
                        }
                        Text {
                            text: suggestionRow.modelData.directory ? "Folder" : "File"
                            color: KetTheme.t.textMuted
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                        }
                    }
                    RowLayout {
                        anchors.fill: parent
                        visible: !suggestionRow.fileEntry
                        spacing: KetTheme.px(10, KetTheme.zoom)
                        Text {
                            text: suggestionRow.modelData.mention || ""
                            color: KetTheme.t.accentHover
                            font.family: agentHost.monoFontFamily
                            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                            Layout.preferredWidth: KetTheme.px(150, KetTheme.zoom)
                            elide: Text.ElideRight
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: suggestionRow.modelData.description || ""
                                color: KetTheme.t.textMain
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                                elide: Text.ElideRight
                            }
                            Text {
                                text: suggestionRow.modelData.kind === "conversation" ? (suggestionRow.modelData.detail || "") : (suggestionRow.modelData.source || "") + " skill"
                                color: KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                            }
                        }
                    }
                }
                background: Rectangle {
                    radius: KetTheme.px(8, KetTheme.zoom)
                    color: parent.highlighted || parent.hovered ? KetTheme.t.interactiveActive : "transparent"
                }
                onClicked: root.applySkillSuggestion(modelData)
            }
        }
    }

    RowLayout {
        id: composerFooter
        objectName: "composerFooter"
        anchors.left: composer.left
        anchors.right: composer.right
        anchors.top: composer.bottom
        // Measured from the composer's box, which the ring widens beyond the card.
        anchors.topMargin: KetTheme.px(4, KetTheme.zoom) + composer.recipe.ringWidth
        anchors.leftMargin: KetTheme.px(2, KetTheme.zoom) - composer.recipe.ringWidth
        anchors.rightMargin: KetTheme.px(2, KetTheme.zoom) - composer.recipe.ringWidth
        height: KetTheme.px(28, KetTheme.zoom)
        spacing: KetTheme.px(2, KetTheme.zoom)

        Kit.KetIconButton {
            id: attachButton
            objectName: "attachButton"
            controlSize: Kit.KetTheme.ControlSize.Small
            glyphSize: composer.recipe.attachIconSize
            iconName: Kit.KetTheme.iconForLucide("plus")
            iconWeight: Kit.KetTheme.IconWeight.Regular
            text: "Attach files"
            showToolTip: false
            contentItem: Item {
                Kit.KetIcon {
                    objectName: "attachGlyph"
                    anchors.centerIn: parent
                    name: attachButton.iconName
                    box: attachButton.glyphSize
                    weight: attachButton.iconWeight
                    color: attachButton.recipe.text
                }
            }
            onClicked: agentHost.chooseAttachments()
            KetToolTip {
                objectName: "ketToolTip"
                visible: attachButton.hovered
                text: "Add images or text files"
            }
        }
        Kit.KetButton {
            id: permissionButton
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            objectName: "permissionButton"
            // Keeps the hover fill while its menu is open, as the mock's footer buttons do.
            previewState: permissionMenu.visible ? {hovered: true, focused: visualFocus} : null
            Layout.minimumHeight: KetTheme.px(28, KetTheme.zoom)
            text: root.currentPermission.label
            contentItem: Row {
                spacing: KetTheme.px(6, KetTheme.zoom)
                KetIcon {
                    width: KetTheme.px(14, KetTheme.zoom)
                    height: KetTheme.px(14, KetTheme.zoom)
                    anchors.verticalCenter: parent.verticalCenter
                    name: KetTheme.iconForLucide((root.permissionIconSource(root.permission)).toString().split("/").pop().replace(/\.svg$/, ""))
                    color: root.permissionAccent
                    box: KetTheme.px(14, KetTheme.zoom)
                }
                Text {
                    text: permissionButton.text
                    visible: !composer.compactControls
                    color: root.permissionAccent
                    font.family: KetTheme.fontFamily
                    font.pixelSize: permissionButton.recipe.fontPixels
                    anchors.verticalCenter: parent.verticalCenter
                }
                KetIcon {
                    width: KetTheme.px(14, KetTheme.zoom)
                    height: KetTheme.px(14, KetTheme.zoom)
                    anchors.verticalCenter: parent.verticalCenter
                    name: KetTheme.iconForLucide("chevron-down")
                    rotation: permissionMenu.visible ? 180 : 0
                    color: root.permissionElevated ? root.permissionAccent : KetTheme.t.textMuted
                    box: KetTheme.px(14, KetTheme.zoom)
                }
            }
            onClicked: {
                skillsPopup.close();
                modelPopup.close();
                permissionMenu.open();
            }
            AgentPermissionMenu {
                id: permissionMenu
                workspace: area.workspace
                anchorItem: permissionButton
            }
            KetToolTip {
                objectName: "ketToolTip"
                visible: permissionButton.hovered
                text: "Permissions for the next message. Restricted actions are blocked without approval prompts."
            }
        }
        Kit.KetButton {
            id: skillsButton
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            objectName: "skillsButton"
            Layout.minimumHeight: KetTheme.px(28, KetTheme.zoom)
            visible: false
            enabled: !agentHost.sessionRunning
            text: root.enabledSkillCount() > 0 ? "Skills " + root.enabledSkillCount() : "Skills"
            contentItem: Row {
                spacing: KetTheme.px(6, KetTheme.zoom)
                KetIcon {
                    width: KetTheme.px(14, KetTheme.zoom)
                    height: KetTheme.px(14, KetTheme.zoom)
                    anchors.verticalCenter: parent.verticalCenter
                    name: KetTheme.iconForLucide("zap")
                    color: root.enabledSkillCount() > 0 ? KetTheme.t.accentHover : KetTheme.t.textSecondary
                    box: KetTheme.px(14, KetTheme.zoom)
                }
                Text {
                    text: skillsButton.text
                    visible: !composer.compactControls
                    color: root.enabledSkillCount() > 0 ? KetTheme.t.accentHover : KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: permissionButton.recipe.fontPixels
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            onClicked: {
                permissionMenu.close();
                modelPopup.close();
                agentHost.refreshSkills();
                skillsPopup.open();
            }
            AgentSkillsMenu {
                id: skillsPopup
                workspace: area.workspace
                anchorItem: skillsButton
            }
            KetToolTip {
                objectName: "ketToolTip"
                visible: skillsButton.hovered
                text: "Manage skills included with the next agent turn"
            }
        }
        Item {
            Layout.fillWidth: true
        }
        Kit.KetButton {
            id: modelButton
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            objectName: "modelButton"
            Layout.minimumWidth: KetTheme.px(40, KetTheme.zoom)
            Layout.minimumHeight: KetTheme.px(28, KetTheme.zoom)
            Layout.maximumWidth: Math.max(KetTheme.px(40, KetTheme.zoom), composer.width - (composer.compactControls ? KetTheme.px(120, KetTheme.zoom) : KetTheme.px(240, KetTheme.zoom)))
            contentItem: RowLayout {
                spacing: KetTheme.px(6, KetTheme.zoom)
                Text {
                    Layout.fillWidth: true
                    text: root.composerModelText()
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: modelButton.recipe.fontPixels
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                KetIcon {
                    Layout.preferredWidth: KetTheme.px(14, KetTheme.zoom)
                    Layout.preferredHeight: KetTheme.px(14, KetTheme.zoom)
                    name: KetTheme.iconForLucide("chevron-down")
                    rotation: modelPopup.visible ? 180 : 0
                    color: KetTheme.t.textMuted
                    box: KetTheme.px(14, KetTheme.zoom)
                }
            }
            onClicked: {
                if (modelPopup.visible)
                    modelPopup.close();
                else
                    root.openModelPicker();
            }
            Accessible.name: "Choose model and reasoning effort"
            KetToolTip {
                objectName: "ketToolTip"
                visible: modelButton.hovered && !modelPopup.visible
                text: root.composerModelTooltip()
            }
        }
        UsageRingButton {
            id: usageRingButton

            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            progress: root.usageRingProgress
            trackColor: KetTheme.t.borderStrong
            fillColor: root.usageRingProgress >= 0.9 ? KetTheme.t.warning : KetTheme.t.textSecondary
            hoverColor: KetTheme.t.surfaceHover
            onClicked: {
                skillsPopup.close();
                permissionMenu.close();
                modelPopup.close();
                usageLimitsPopover.toggleFromAnchor();
            }
            KetToolTip {
                objectName: "ketToolTip"
                visible: usageRingButton.hovered && !usageLimitsPopover.visible
                text: {
                    const pct = Math.round(root.usageRingProgress * 100);
                    if (root.usageContext.available)
                        return "Context " + pct + "% · usage limits";
                    if (root.weeklyQuota.available)
                        return "Weekly " + pct + "% · usage limits";
                    return "Context and usage limits";
                }
            }
        }
    }
}
