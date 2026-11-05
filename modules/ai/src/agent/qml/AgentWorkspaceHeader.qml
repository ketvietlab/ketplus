import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Item {
    id: header
    required property var workspace
    objectName: "agentWorkspaceHeader"
    implicitHeight: Metrics.px(44, workspace.t)

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
                                             - titleRow.spacing * 2))
            text: agentHost.sessionTitle
            color: workspace.t.textMain
            elide: Text.ElideRight
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.title
            font.weight: Font.DemiBold
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
                font.weight: Font.DemiBold
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
        Item {
            Layout.fillWidth: true
        }
    }

    Row {
        id: headerActions
        anchors.right: parent.right
        anchors.rightMargin: Metrics.px(8, workspace.t)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Metrics.px(2, workspace.t)

        Basic.ToolButton {
            id: atlasButton
            objectName: "headerAtlasButton"
            width: Metrics.px(28, workspace.t)
            height: Metrics.px(28, workspace.t)
            padding: Metrics.px(7, workspace.t)
            onClicked: agentHost.requestProductNavigation("atlas")
            contentItem: TintedIcon {
                source: "qrc:/agent/icons/network.svg"
                color: workspace.t.textSecondary
            }
            background: Rectangle {
                radius: Metrics.px(5, workspace.t)
                color: atlasButton.hovered ? workspace.t.surfaceHover : "transparent"
                border.color: atlasButton.visualFocus ? workspace.t.focus : "transparent"
            }
            Text {
                objectName: "headerAtlasRunningBadge"
                visible: agentHost.atlasRunningCount > 0
                anchors.right: parent.right
                anchors.rightMargin: -Metrics.px(4, workspace.t)
                anchors.top: parent.top
                anchors.topMargin: -Metrics.px(4, workspace.t)
                text: agentHost.atlasRunningCount > 9 ? "9+" : String(agentHost.atlasRunningCount)
                color: "#ffffff"
                font.family: workspace.t.uiFont
                font.pixelSize: workspace.t.micro
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                width: Math.max(implicitWidth + Metrics.px(6, workspace.t), Metrics.px(14, workspace.t))
                height: Metrics.px(14, workspace.t)
                Rectangle {
                    anchors.fill: parent
                    z: -1
                    radius: height / 2
                    color: workspace.t.accent
                }
            }
            Accessible.name: agentHost.atlasRunningCount > 0
                             ? "Atlases, " + agentHost.atlasRunningCount + " running across worktrees"
                             : "Atlases"
            KetToolTip {
                tokens: workspace.t
                visible: atlasButton.hovered
                text: "Atlases"
            }
        }

        Basic.ToolButton {
            id: newConversationButton
            objectName: "headerNewConversationButton"
            visible: !agentHost.compactMode
            width: Metrics.px(28, workspace.t)
            height: Metrics.px(28, workspace.t)
            padding: Metrics.px(7, workspace.t)
            onClicked: agentHost.requestNewSession()
            contentItem: TintedIcon {
                source: "qrc:/agent/icons/square-pen.svg"
                color: workspace.t.textSecondary
            }
            background: Rectangle {
                anchors.centerIn: parent
                width: Metrics.px(28, workspace.t)
                height: Metrics.px(28, workspace.t)
                radius: Metrics.px(5, workspace.t)
                color: newConversationButton.hovered ? workspace.t.surfaceHover : "transparent"
                border.color: newConversationButton.visualFocus ? workspace.t.focus : "transparent"
            }
            Accessible.name: "New conversation"
            KetToolTip {
                tokens: workspace.t
                visible: newConversationButton.hovered
                text: "New conversation"
            }
        }
        Basic.ToolButton {
            id: terminalButton
            objectName: "headerTerminalButton"
            visible: !agentHost.compactMode
            width: Metrics.px(28, workspace.t)
            height: Metrics.px(28, workspace.t)
            padding: Metrics.px(7, workspace.t)
            onClicked: agentHost.requestTerminal()
            contentItem: TintedIcon {
                source: "qrc:/agent/icons/square-terminal.svg"
                color: workspace.t.textSecondary
            }
            background: Rectangle {
                anchors.centerIn: parent
                width: Metrics.px(28, workspace.t)
                height: Metrics.px(28, workspace.t)
                radius: Metrics.px(5, workspace.t)
                color: terminalButton.hovered ? workspace.t.surfaceHover : "transparent"
                border.color: terminalButton.visualFocus ? workspace.t.focus : "transparent"
            }
            Accessible.name: "Open terminal"
            KetToolTip {
                tokens: workspace.t
                visible: terminalButton.hovered
                text: "Open terminal"
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: workspace.t.border
    }
}
