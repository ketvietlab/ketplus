import "ChromeMetrics.js" as Metrics
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
    Layout.preferredHeight: composer.height + composerFooter.height + Metrics.px(18, root.t)

    Rectangle {
        id: composer
        readonly property var recipe: Kit.KetTheme.composerRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom, prompt.activeFocus)
        objectName: "agentComposer"
        readonly property bool compactControls: width < 520
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        width: Math.min(parent.width - Metrics.px(40, root.t), root.conversationColumnWidth - Metrics.px(40, root.t))
        height: composerColumn.implicitHeight + 2 * (recipe.padding + recipe.ringWidth + 1)
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
            anchors.margins: composer.recipe.padding + composer.recipe.ringWidth + 1
            spacing: 0

            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: childrenRect.height
                visible: agentHost.attachments.length > 0
                spacing: Metrics.px(6, root.t)
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
                        property string hint: agentHost.presentationState.folder === "missing" ? "Continue in the main worktree…" : "Type / for commands, @ for files, $ for skills"
                        // Drafting must remain available even while the
                        // transport reconnects; submission is gated separately.
                        enabled: true
                        color: composer.recipe.text
                        placeholderTextColor: composer.recipe.placeholder
                        selectionColor: root.t.accentMuted
                        selectedTextColor: root.t.textMain
                        wrapMode: TextArea.Wrap
                        background: null
                        font.family: root.t.uiFont
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
        anchors.bottomMargin: Metrics.px(6, root.t)
        z: 20
        height: slashCommandList.implicitHeight + Metrics.px(8, root.t)
        visible: root.visibleSlashCommands.length > 0 && !agentHost.sessionRunning
        radius: Metrics.px(12, root.t)
        color: root.t.surfaceRaised
        border.width: 1
        border.color: root.t.borderStrong
        Column {
            id: slashCommandList
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: Metrics.px(4, root.t)
            spacing: Metrics.px(2, root.t)
            Repeater {
                model: root.visibleSlashCommands
                Basic.ItemDelegate {
                    required property var modelData
                    width: slashCommandList.width
                    height: Metrics.px(42, root.t)
                    padding: Metrics.px(8, root.t)
                    contentItem: RowLayout {
                        spacing: Metrics.px(10, root.t)
                        Text {
                            text: modelData.label
                            color: root.t.textMain
                            font.family: root.t.monoFont
                            font.pixelSize: root.t.reading
                            Layout.preferredWidth: Metrics.px(92, root.t)
                            verticalAlignment: Text.AlignVCenter
                        }
                        Text {
                            text: modelData.description
                            color: root.t.textMuted
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.body
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    background: Rectangle {
                        radius: Metrics.px(8, root.t)
                        color: parent.hovered ? root.t.interactiveActive : "transparent"
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
        anchors.bottomMargin: Metrics.px(6, root.t)
        z: 20
        height: Math.min(Metrics.px(360, root.t), skillSuggestionList.contentHeight + Metrics.px(8, root.t))
        visible: root.visibleSkillSuggestions.length > 0
        radius: Metrics.px(12, root.t)
        color: root.t.surfaceRaised
        border.width: 1
        border.color: root.t.borderStrong

        ListView {
            id: skillSuggestionList
            objectName: "skillSuggestionList"
            anchors.fill: parent
            anchors.margins: Metrics.px(4, root.t)
            clip: true
            spacing: Metrics.px(2, root.t)
            model: root.visibleSkillSuggestions
            currentIndex: Math.min(root.selectedSkillSuggestion, Math.max(0, count - 1))

            delegate: Basic.ItemDelegate {
                id: suggestionRow
                required property var modelData
                required property int index
                readonly property bool fileEntry: modelData.kind === "file"
                width: skillSuggestionList.width
                // A file shows its worktree path, then whether it is a file or folder.
                height: Metrics.px(46, root.t)
                padding: Metrics.px(8, root.t)
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
                            color: root.t.accentHover
                            font.family: root.t.monoFont
                            font.pixelSize: root.t.reading
                            // Long paths keep both the top folder and the file name.
                            elide: Text.ElideMiddle
                        }
                        Text {
                            text: suggestionRow.modelData.directory ? "Folder" : "File"
                            color: root.t.textMuted
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.micro
                        }
                    }
                    RowLayout {
                        anchors.fill: parent
                        visible: !suggestionRow.fileEntry
                        spacing: Metrics.px(10, root.t)
                        Text {
                            text: suggestionRow.modelData.mention || ""
                            color: root.t.accentHover
                            font.family: root.t.monoFont
                            font.pixelSize: root.t.reading
                            Layout.preferredWidth: Metrics.px(150, root.t)
                            elide: Text.ElideRight
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: suggestionRow.modelData.description || ""
                                color: root.t.textMain
                                font.family: root.t.uiFont
                                font.pixelSize: root.t.body
                                elide: Text.ElideRight
                            }
                            Text {
                                text: suggestionRow.modelData.kind === "conversation" ? (suggestionRow.modelData.detail || "") : (suggestionRow.modelData.source || "") + " skill"
                                color: root.t.textMuted
                                font.family: root.t.uiFont
                                font.pixelSize: root.t.micro
                            }
                        }
                    }
                }
                background: Rectangle {
                    radius: Metrics.px(8, root.t)
                    color: parent.highlighted || parent.hovered ? root.t.interactiveActive : "transparent"
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
        anchors.topMargin: Metrics.px(4, root.t)
        anchors.leftMargin: Metrics.px(2, root.t)
        anchors.rightMargin: Metrics.px(2, root.t)
        height: Metrics.px(28, root.t)
        spacing: Metrics.px(2, root.t)

        Kit.KetIconButton {
            id: attachButton
            objectName: "attachButton"
            Layout.preferredWidth: composer.recipe.toolbarHeight
            Layout.preferredHeight: composer.recipe.toolbarHeight
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
                tokens: root.t
                visible: attachButton.hovered
                text: "Add images or text files"
            }
        }
        Kit.KetButton {
            id: permissionButton
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            objectName: "permissionButton"
            Layout.minimumHeight: Metrics.px(28, root.t)
            text: root.currentPermission.label
            contentItem: Row {
                spacing: Metrics.px(6, root.t)
                TintedIcon {
                    width: Metrics.px(14, root.t)
                    height: Metrics.px(14, root.t)
                    anchors.verticalCenter: parent.verticalCenter
                    source: root.permissionIconSource(root.permission)
                    color: root.permissionAccent
                }
                Text {
                    text: permissionButton.text
                    visible: !composer.compactControls
                    color: root.permissionAccent
                    font.family: root.t.uiFont
                    font.pixelSize: permissionButton.recipe.fontPixels
                    anchors.verticalCenter: parent.verticalCenter
                }
                TintedIcon {
                    width: Metrics.px(14, root.t)
                    height: Metrics.px(14, root.t)
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/agent/icons/chevron-down.svg"
                    color: root.permissionElevated ? root.permissionAccent : root.t.textMuted
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
                tokens: root.t
                visible: permissionButton.hovered
                text: "Permissions for the next message. Restricted actions are blocked without approval prompts."
            }
        }
        Kit.KetButton {
            id: skillsButton
            variant: Kit.KetTheme.ButtonVariant.Ghost
            controlSize: Kit.KetTheme.ControlSize.Small
            objectName: "skillsButton"
            Layout.minimumHeight: Metrics.px(28, root.t)
            visible: false
            enabled: !agentHost.sessionRunning
            text: root.enabledSkillCount() > 0 ? "Skills " + root.enabledSkillCount() : "Skills"
            contentItem: Row {
                spacing: Metrics.px(6, root.t)
                TintedIcon {
                    width: Metrics.px(14, root.t)
                    height: Metrics.px(14, root.t)
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/agent/icons/zap.svg"
                    color: root.enabledSkillCount() > 0 ? root.t.accentHover : root.t.textSecondary
                }
                Text {
                    text: skillsButton.text
                    visible: !composer.compactControls
                    color: root.enabledSkillCount() > 0 ? root.t.accentHover : root.t.textSecondary
                    font.family: root.t.uiFont
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
                tokens: root.t
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
            Layout.minimumWidth: Metrics.px(40, root.t)
            Layout.minimumHeight: Metrics.px(28, root.t)
            Layout.maximumWidth: Math.max(Metrics.px(40, root.t), composer.width - (composer.compactControls ? Metrics.px(120, root.t) : Metrics.px(240, root.t)))
            contentItem: RowLayout {
                spacing: Metrics.px(6, root.t)
                Text {
                    Layout.fillWidth: true
                    text: root.composerModelText()
                    color: root.t.textSecondary
                    font.family: root.t.uiFont
                    font.pixelSize: modelButton.recipe.fontPixels
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                TintedIcon {
                    Layout.preferredWidth: Metrics.px(14, root.t)
                    Layout.preferredHeight: Metrics.px(14, root.t)
                    source: "qrc:/agent/icons/chevron-down.svg"
                    rotation: modelPopup.visible ? 180 : 0
                    color: root.t.textMuted
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
                tokens: root.t
                visible: modelButton.hovered && !modelPopup.visible
                text: root.composerModelTooltip()
            }
        }
        UsageRingButton {
            id: usageRingButton
            tokens: root.t
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            progress: root.usageRingProgress
            trackColor: root.t.borderStrong
            fillColor: root.usageRingProgress >= 0.9 ? root.t.warning : root.t.textSecondary
            hoverColor: root.t.surfaceHover
            onClicked: {
                skillsPopup.close();
                permissionMenu.close();
                modelPopup.close();
                usageLimitsPopover.toggleFromAnchor();
            }
            KetToolTip {
                tokens: root.t
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
