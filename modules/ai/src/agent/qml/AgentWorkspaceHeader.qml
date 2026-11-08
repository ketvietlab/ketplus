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
            color: Kit.KetTheme.t.accent
        } : tag)
    readonly property var selectedWorktree: {
        for (let candidate of agentHost.worktrees) {
            if (candidate.selected)
                return candidate;
        }
        return ({});
    }
    readonly property real maximumTitleWidth: Math.max(KetTheme.px(160, KetTheme.zoom), Math.min(KetTheme.px(420, KetTheme.zoom), width * 0.45))
    readonly property string scopeLabel: {
        const branch = agentHost.executionBranch || selectedWorktree.branch || "workspace";
        const repository = agentHost.repositoryName || "Repository";
        return repository + " · " + branch;
    }

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
            // ADR 0027: the number other conversations and `#` references use.
            readonly property string number: {
                const conversations = agentHost.conversations || [];
                for (let index = 0; index < conversations.length; ++index) {
                    if (conversations[index].active)
                        return conversations[index].number || "";
                }
                return "";
            }
            visible: number.length > 0
            text: "#" + number
            color: workspace.t.textMuted
            font.family: workspace.t.monoFont
            font.pixelSize: workspace.t.body
            font.weight: Font.Medium
            Accessible.name: "Conversation number " + number
        }
        Text {
            id: conversationTitle
            objectName: "conversationTitle"
            // Long handoff titles must not push the status badge and scope chip away.
            Layout.maximumWidth: Math.max(0, Math.min(header.maximumTitleWidth, titleRow.width - conversationStatusBadge.width - (scopeChip.visible ? Math.min(scopeChip.implicitWidth, 80) : 0) - (conversationNumber.visible ? conversationNumber.implicitWidth + titleRow.spacing : 0) - (conversationTags.visible ? conversationTags.implicitWidth + titleRow.spacing : 0) - titleRow.spacing * 2))
            text: agentHost.sessionTitle
            color: workspace.t.textMain
            elide: Text.ElideRight
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.reading
            font.weight: Font.Medium
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Rectangle {
            id: conversationStatusBadge
            objectName: "conversationStatusBadge"
            Layout.preferredHeight: KetTheme.px(18, KetTheme.zoom)
            Layout.preferredWidth: statusText.implicitWidth + KetTheme.px(12, KetTheme.zoom)
            Layout.maximumWidth: KetTheme.px(120, KetTheme.zoom)
            radius: KetTheme.px(4, KetTheme.zoom)
            color: agentHost.sessionRunning ? workspace.t.accentSubtle : "transparent"
            border.color: agentHost.sessionRunning ? workspace.t.accent : workspace.t.border
            Text {
                id: statusText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - KetTheme.px(12, KetTheme.zoom))
                text: agentHost.sessionRunning ? "Working" : agentHost.sessionStatus
                color: agentHost.sessionRunning ? workspace.t.accentHover : workspace.t.textMuted
                font.family: workspace.t.uiFont
                font.pixelSize: workspace.t.micro
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Accessible.role: Accessible.StaticText
            Accessible.name: statusText.text
        }
        Rectangle {
            id: scopeChip
            objectName: "workspaceScopeChip"
            visible: !agentHost.compactWindow
            // Measure the unelided text independently of the allocated chip width.
            implicitWidth: scopeText.implicitWidth + KetTheme.px(30, KetTheme.zoom)
            Layout.preferredHeight: KetTheme.px(20, KetTheme.zoom)
            Layout.preferredWidth: implicitWidth
            Layout.fillWidth: false
            Layout.maximumWidth: implicitWidth
            Layout.minimumWidth: 0
            radius: KetTheme.px(5, KetTheme.zoom)
            color: workspace.t.interactiveBackground
            border.color: workspace.t.border
            clip: true
            Row {
                id: scopeRow
                anchors.left: parent.left
                anchors.leftMargin: KetTheme.px(6, KetTheme.zoom)
                anchors.verticalCenter: parent.verticalCenter
                spacing: KetTheme.px(4, KetTheme.zoom)
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: KetTheme.px(14, KetTheme.zoom)
                    height: KetTheme.px(14, KetTheme.zoom)
                    name: KetTheme.iconForLucide("git-branch")
                    color: workspace.t.textMuted
                    box: KetTheme.px(14, KetTheme.zoom)
                }
                Text {
                    id: scopeText
                    objectName: "workspaceScopeLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(0, scopeChip.width - KetTheme.px(30, KetTheme.zoom)))
                    text: header.scopeLabel
                    color: workspace.t.textSecondary
                    elide: Text.ElideMiddle
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.caption
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
            Accessible.onPressAction: agentHost.requestExecutionWorktree()
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
                    tagColor: modelData.color || Kit.KetTheme.t.accent
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
                color: Kit.KetTheme.t.accent
                font.pixelSize: workspace.t.micro
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

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: workspace.t.border
    }
}
