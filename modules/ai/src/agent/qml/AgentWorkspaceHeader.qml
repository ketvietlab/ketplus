import "ChromeMetrics.js" as Metrics
import QtQuick
import Ket as Kit
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: header
    required property var workspace
    objectName: "agentWorkspaceHeader"
    implicitHeight: Metrics.px(44, workspace.t)

    readonly property var selectedConversation: {
        for (const conversation of agentHost.conversations || [])
            if (conversation.active) return conversation
        return ({})
    }
    readonly property var tags: (selectedConversation.tags || []).map(tag =>
        typeof tag === "string" ? {label: tag, color: Kit.KetTheme.t.accent} : tag)
    readonly property var selectedWorktree: {
        for (let candidate of agentHost.worktrees) {
            if (candidate.selected) return candidate
        }
        return ({})
    }
    readonly property real maximumTitleWidth: Math.max(Metrics.px(160, workspace.t), Math.min(Metrics.px(420, workspace.t), width * 0.45))
    readonly property string scopeLabel: {
        const branch = agentHost.executionBranch || selectedWorktree.branch || "workspace"
        const repository = agentHost.repositoryName || "Repository"
        return repository + " · " + branch
    }

    // One compact row: title, status and a muted scope chip on the left, icon actions right.
    RowLayout {
        id: titleRow
        anchors.left: parent.left
        anchors.right: headerActions.left
        anchors.leftMargin: Metrics.px(16, workspace.t)
        anchors.rightMargin: Metrics.px(8, workspace.t)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.px(8, workspace.t)

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
            Layout.maximumWidth: Math.max(0, Math.min(header.maximumTitleWidth, titleRow.width - conversationStatusBadge.width
                                             - Math.min(scopeChip.implicitWidth, 80)
                                             - (conversationNumber.visible ? conversationNumber.implicitWidth + titleRow.spacing : 0)
                                             - (conversationTags.visible ? conversationTags.implicitWidth + titleRow.spacing : 0)
                                             - titleRow.spacing * 2))
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
            Layout.preferredHeight: Metrics.px(18, workspace.t)
            Layout.preferredWidth: statusText.implicitWidth + Metrics.px(12, workspace.t)
            Layout.maximumWidth: Metrics.px(120, workspace.t)
            radius: Metrics.px(4, workspace.t)
            color: agentHost.sessionRunning ? workspace.t.accentSubtle : "transparent"
            border.color: agentHost.sessionRunning ? workspace.t.accent : workspace.t.border
            Text {
                id: statusText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - Metrics.px(12, workspace.t))
                text: agentHost.sessionRunning ? "Working" : agentHost.sessionStatus
                color: agentHost.sessionRunning ? workspace.t.accentHover
                                                : workspace.t.textMuted
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
            // Measure the unelided text independently of the allocated chip width.
            implicitWidth: scopeText.implicitWidth + Metrics.px(30, workspace.t)
            Layout.preferredHeight: Metrics.px(20, workspace.t)
            Layout.preferredWidth: implicitWidth
            Layout.fillWidth: false
            Layout.maximumWidth: implicitWidth
            Layout.minimumWidth: 0
            radius: Metrics.px(5, workspace.t)
            color: workspace.t.interactiveBackground
            border.color: workspace.t.border
            clip: true
            Row {
                id: scopeRow
                anchors.left: parent.left
                anchors.leftMargin: Metrics.px(6, workspace.t)
                anchors.verticalCenter: parent.verticalCenter
                spacing: Metrics.px(4, workspace.t)
                TintedIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Metrics.px(14, workspace.t)
                    height: Metrics.px(14, workspace.t)
                    source: "qrc:/agent/icons/git-branch.svg"
                    color: workspace.t.textMuted
                }
                Text {
                    id: scopeText
                    objectName: "workspaceScopeLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(0, scopeChip.width - Metrics.px(30, workspace.t)))
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
                tokens: workspace.t
                visible: scopeMouse.containsMouse
                text: header.scopeLabel
            }
            Accessible.role: Accessible.Button
            Accessible.name: "Choose Files and Source Control worktree"
            Accessible.onPressAction: agentHost.requestExecutionWorktree()
        }
        Row {
            id: conversationTags
            visible: header.tags.length > 0 && header.width > Metrics.px(760, workspace.t)
            spacing: Metrics.px(4, workspace.t)
            Repeater {
                model: header.tags.slice(0, 2)
                Kit.KetTag {
                    required property var modelData
                    objectName: "conversationTag_" + modelData.label
                    text: modelData.label
                    tagColor: modelData.color || Kit.KetTheme.t.accent
                    maxWidth: Metrics.px(100, workspace.t)
                    filterable: true
                    onClicked: agentHost.requestTagFilter(text)
                }
            }
        }
        Item { Layout.fillWidth: true }
    }

    Row {
        id: headerActions
        anchors.right: parent.right
        anchors.rightMargin: Metrics.px(8, workspace.t)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.px(2, workspace.t)

        Kit.KetIconButton {
            id: atlasButton
            objectName: "headerAtlasButton"
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
            visible: !agentHost.compactMode
            controlSize: Kit.KetTheme.ControlSize.Small
            iconName: Kit.KetTheme.iconForLucide("square-pen")
            text: "New conversation"
            onClicked: agentHost.requestNewSession()
        }
        Kit.KetIconButton {
            objectName: "headerTerminalButton"
            visible: !agentHost.compactMode
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
