import "ChromeMetrics.js" as Metrics
import QtQuick
import Ket as Kit
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Effects
import QtQuick.Layouts

Rectangle {
    id: sidebarRoot
    required property var workspace
    objectName: "conversationSidebar"
    SplitView.preferredWidth: 216
    SplitView.minimumWidth: 188
    SplitView.maximumWidth: Math.max(188, workspace.width - 326)
    color: workspace.t.sidebarBackground || "#171b20"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        AgentProductNavigation {
            workspace: sidebarRoot.workspace
            compactHeight: sidebarRoot.height < 790
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
        }

        Kit.KetGroupHeader {
            id: repositoryGroup
            objectName: "sidebarRepositoryGroup"
            Layout.fillWidth: true
            Layout.leftMargin: Metrics.px(6, workspace.t)
            Layout.rightMargin: Metrics.px(6, workspace.t)
            name: agentHost.repositoryName || "Open repository"
            onActionTriggered: agentHost.requestNewSession()
        }

        ListView {
            id: conversationList
            objectName: "sidebarConversationList"
            visible: repositoryGroup.expanded
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Metrics.px(8, workspace.t)
            Layout.rightMargin: Metrics.px(8, workspace.t)
            spacing: Metrics.px(2, workspace.t)
            clip: true
            model: agentHost.conversations
            property string firstConversationId: ""
            onModelChanged: {
                const firstId = model.length > 0 ? model[0].id : "";
                if (firstId !== firstConversationId) {
                    firstConversationId = firstId;
                    Qt.callLater(function () {
                        conversationList.positionViewAtBeginning();
                    });
                }
            }

            delegate: Kit.KetConversationRow {
                id: conversationRow
                required property var modelData
                readonly property string conversationId: modelData.id
                property alias contextMenu: conversationMenu
                property alias renameMenuItem: renameConversationMenuItem
                objectName: "conversationRow_" + conversationId
                width: conversationList.width
                text: modelData.title || "New conversation"
                number: modelData.number || ""
                checked: !!modelData.active
                tags: modelData.tags || []
                status: modelData.needsInput ? Kit.KetTheme.ConversationStatus.NeedsInput : modelData.awaitingReply ? Kit.KetTheme.ConversationStatus.AwaitingReply : modelData.running ? Kit.KetTheme.ConversationStatus.Working : /fail|error|disconnect|missing/i.test(modelData.status || "") ? Kit.KetTheme.ConversationStatus.Error : /stop/i.test(modelData.status || "") ? Kit.KetTheme.ConversationStatus.Stopped : modelData.started ? Kit.KetTheme.ConversationStatus.Completed : Kit.KetTheme.ConversationStatus.NotStarted
                statusLabel: modelData.status || Kit.KetTheme.conversationStatusLabel(status)
                readonly property bool animationAllowed: agentHost.motionEnabled && !agentHost.restoringHistory && workspace.visible && workspace.Window.active && workspace.Window.visibility !== Window.Minimized && visible && y + height >= conversationList.contentY && y <= conversationList.contentY + conversationList.height
                // Preserve the host's motion/visibility policy while sharing the kit clock.
                function syncClock() {
                    const active = spins && animationAllowed;
                    if (holdsClock !== active) {
                        holdsClock = active;
                        Kit.KetTheme.holdAnimationClock(active);
                    }
                }
                onAnimationAllowedChanged: syncClock()
                MouseArea {
                    id: conversationMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            conversationMenu.popup();
                        } else {
                            agentHost.selectConversation(conversationId);
                            workspace.focusPrompt();
                        }
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button === Qt.LeftButton) {
                            mouse.accepted = true;
                            Qt.callLater(function () {
                                agentHost.requestRenameConversation(conversationId);
                            });
                        }
                    }
                }
                Basic.Menu {
                    id: conversationMenu
                    objectName: "conversationContextMenu"
                    readonly property color backgroundColor: workspace.t.panelBackground || workspace.t.surfaceRaised || "#20252b"
                    readonly property color borderColor: workspace.t.borderStrong || "#3b424c"
                    readonly property real cornerRadius: 7
                    padding: Metrics.px(5, workspace.t)
                    margins: 8
                    opacity: 1
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.reading
                    background: Rectangle {
                        implicitWidth: Metrics.px(180, workspace.t)
                        implicitHeight: Metrics.px(146, workspace.t)
                        color: conversationMenu.backgroundColor
                        border.color: conversationMenu.borderColor
                        border.width: 1
                        radius: conversationMenu.cornerRadius
                    }
                    Basic.MenuItem {
                        id: renameConversationMenuItem
                        objectName: "renameConversationMenuItem"
                        text: "Rename..."
                        width: conversationMenu.availableWidth
                        implicitHeight: Metrics.px(34, workspace.t)
                        leftPadding: Metrics.px(10, workspace.t)
                        rightPadding: Metrics.px(28, workspace.t)
                        topPadding: Metrics.px(4, workspace.t)
                        bottomPadding: Metrics.px(4, workspace.t)
                        contentItem: Text {
                            text: renameConversationMenuItem.text
                            color: !renameConversationMenuItem.enabled ? workspace.t.textDisabled : renameConversationMenuItem.highlighted ? workspace.t.accentText : workspace.t.textMain
                            font: renameConversationMenuItem.font
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            color: renameConversationMenuItem.highlighted ? workspace.t.accentSubtle : "transparent"
                            border.color: renameConversationMenuItem.visualFocus ? workspace.t.focus : "transparent"
                            border.width: renameConversationMenuItem.visualFocus ? 1 : 0
                            radius: Metrics.px(5, workspace.t)
                        }
                        onTriggered: Qt.callLater(function () {
                            agentHost.requestRenameConversation(conversationId);
                        })
                    }
                    Basic.MenuItem {
                        id: forkConversationMenuItem
                        objectName: "forkConversationMenuItem"
                        text: "Fork this conversation"
                        enabled: modelData.forkable === true
                        width: conversationMenu.availableWidth
                        implicitHeight: Metrics.px(34, workspace.t)
                        leftPadding: Metrics.px(10, workspace.t)
                        rightPadding: Metrics.px(28, workspace.t)
                        contentItem: Text {
                            text: forkConversationMenuItem.text
                            color: !forkConversationMenuItem.enabled ? workspace.t.textDisabled : forkConversationMenuItem.highlighted ? workspace.t.accentText : workspace.t.textMain
                            font: forkConversationMenuItem.font
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            color: forkConversationMenuItem.highlighted ? workspace.t.accentSubtle : "transparent"
                            radius: Metrics.px(5, workspace.t)
                        }
                        onTriggered: Qt.callLater(function () {
                            agentHost.requestForkConversation(conversationId);
                        })
                    }
                    Basic.MenuItem {
                        id: deleteConversationMenuItem
                        objectName: "deleteConversationMenuItem"
                        text: "Delete…"
                        enabled: !modelData.running
                        width: conversationMenu.availableWidth
                        implicitHeight: Metrics.px(34, workspace.t)
                        leftPadding: Metrics.px(10, workspace.t)
                        rightPadding: Metrics.px(28, workspace.t)
                        contentItem: Text {
                            text: deleteConversationMenuItem.text
                            color: deleteConversationMenuItem.enabled ? (deleteConversationMenuItem.highlighted ? workspace.t.accentText : workspace.t.danger) : workspace.t.textDisabled
                            font: deleteConversationMenuItem.font
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            color: deleteConversationMenuItem.highlighted ? workspace.t.accentSubtle : "transparent"
                            radius: Metrics.px(5, workspace.t)
                        }
                        onTriggered: Qt.callLater(function () {
                            agentHost.requestDeleteConversation(conversationId);
                        })
                    }
                    Basic.MenuItem {
                        id: archiveConversationMenuItem
                        objectName: "archiveConversationMenuItem"
                        text: "Archive"
                        width: conversationMenu.availableWidth
                        implicitHeight: Metrics.px(34, workspace.t)
                        leftPadding: Metrics.px(10, workspace.t)
                        rightPadding: Metrics.px(28, workspace.t)
                        contentItem: Text {
                            text: archiveConversationMenuItem.text
                            color: archiveConversationMenuItem.enabled ? (archiveConversationMenuItem.highlighted ? workspace.t.accentText : workspace.t.textMain) : workspace.t.textDisabled
                            font: archiveConversationMenuItem.font
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            color: archiveConversationMenuItem.highlighted ? workspace.t.accentSubtle : "transparent"
                            radius: Metrics.px(5, workspace.t)
                        }
                        onTriggered: Qt.callLater(function () {
                            agentHost.requestArchiveConversation(conversationId);
                        })
                    }
                }
            }
        }

        Item {
            id: weeklyFooter
            objectName: "weeklyLimitFooter"
            property var quota: agentHost.statusProvider === "codex" ? agentHost.weeklyLimit : ({})
            visible: agentHost.statusProvider.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(58, workspace.t)
            RowLayout {
                anchors.left: parent.left
                anchors.leftMargin: Metrics.px(12, workspace.t)
                anchors.right: parent.right
                anchors.rightMargin: Metrics.px(8, workspace.t)
                anchors.top: parent.top
                anchors.topMargin: Metrics.px(8, workspace.t)
                spacing: Metrics.px(4, workspace.t)
                Text {
                    text: "Usage"
                    color: workspace.t.textSecondary
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.caption
                }
                Item {
                    Layout.fillWidth: true
                }
                Text {
                    objectName: "weeklyLimitValue"
                    text: weeklyFooter.quota.available ? Math.round(weeklyFooter.quota.usedPercent) + "% used" : weeklyFooter.quota.loading ? "Loading..." : "--"
                    color: workspace.t.textMuted
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.caption
                }
                Basic.ToolButton {
                    id: refreshWeeklyLimitButton
                    objectName: "refreshWeeklyLimitButton"
                    Layout.preferredWidth: Metrics.px(22, workspace.t)
                    Layout.preferredHeight: Metrics.px(24, workspace.t)
                    enabled: agentHost.statusProvider === "codex" && agentHost.agentAvailable && !weeklyFooter.quota.loading
                    contentItem: TintedIcon {
                        source: "qrc:/agent/icons/rotate-ccw.svg"
                        color: workspace.t.textMuted
                    }
                    background: Rectangle {
                        color: parent.hovered ? workspace.t.surfaceHover : "transparent"
                        radius: Metrics.px(4, workspace.t)
                    }
                    onClicked: agentHost.requestWeeklyLimit()
                    KetToolTip {
                        tokens: workspace.t
                        visible: refreshWeeklyLimitButton.hovered
                        text: "Refresh weekly limit"
                    }
                    Accessible.name: "Refresh weekly limit"
                }
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Metrics.px(12, workspace.t)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Metrics.px(12, workspace.t)
                height: Metrics.px(3, workspace.t)
                color: workspace.t.border
                Rectangle {
                    objectName: "weeklyLimitProgress"
                    height: parent.height
                    width: parent.width * (weeklyFooter.quota.available ? Math.min(1, Math.max(0, weeklyFooter.quota.usedPercent / Metrics.px(100, workspace.t))) : 0)
                    color: weeklyFooter.quota.usedPercent >= 90 ? workspace.t.warning : workspace.t.accent
                }
                MouseArea {
                    id: weeklyLimitMouse
                    anchors.fill: parent
                    anchors.margins: -Metrics.px(5, workspace.t)
                    hoverEnabled: true
                    KetToolTip {
                        tokens: workspace.t
                        visible: weeklyLimitMouse.containsMouse
                        text: weeklyFooter.quota.available ? (weeklyFooter.quota.resetsAt > 0 ? "Resets " + Qt.formatDateTime(new Date(weeklyFooter.quota.resetsAt * 1000), "ddd, dd MMM yyyy hh:mm") : "Reset time unavailable") : "Weekly limit unavailable"
                    }
                }
            }
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: workspace.t.border
            }
        }

        Basic.Button {
            id: settingsNavigationButton
            objectName: "productNavigation_settings"
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(34, workspace.t)
            Layout.leftMargin: Metrics.px(8, workspace.t)
            Layout.rightMargin: Metrics.px(8, workspace.t)
            Layout.topMargin: Metrics.px(4, workspace.t)
            padding: 0
            onClicked: agentHost.requestSettings()
            contentItem: RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Metrics.px(9, workspace.t)
                anchors.rightMargin: Metrics.px(9, workspace.t)
                spacing: Metrics.px(9, workspace.t)
                TintedIcon {
                    Layout.preferredWidth: Metrics.px(16, workspace.t)
                    Layout.preferredHeight: Metrics.px(16, workspace.t)
                    source: "qrc:/agent/icons/settings.svg"
                    color: workspace.t.textMuted
                }
                Text {
                    Layout.fillWidth: true
                    text: "Settings"
                    color: workspace.t.textSecondary
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.body
                }
                Text {
                    text: "⌘ ,"
                    color: workspace.t.textMuted
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.micro
                }
            }
            background: Rectangle {
                radius: Metrics.px(6, workspace.t)
                color: settingsNavigationButton.hovered ? workspace.t.surfaceHover : "transparent"
                border.color: settingsNavigationButton.visualFocus ? workspace.t.focus : "transparent"
            }
            Accessible.name: "Settings"
        }

        Item {
            objectName: "productIdentity"
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(54, workspace.t)
            Layout.leftMargin: Metrics.px(8, workspace.t)
            Layout.rightMargin: Metrics.px(8, workspace.t)

            Rectangle {
                anchors.fill: parent
                anchors.margins: Metrics.px(2, workspace.t)
                radius: Metrics.px(6, workspace.t)
                color: identityMouse.containsMouse ? workspace.t.surfaceHover : "transparent"
            }
            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: Metrics.px(9, workspace.t)
                anchors.verticalCenter: parent.verticalCenter
                width: Metrics.px(25, workspace.t)
                height: Metrics.px(25, workspace.t)
                radius: Metrics.px(13, workspace.t)
                color: workspace.t.accentMuted
                Text {
                    anchors.centerIn: parent
                    text: "KP"
                    color: workspace.t.tabActiveText
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.micro
                    font.weight: Font.Bold
                }
            }
            Column {
                anchors.left: parent.left
                anchors.leftMargin: Metrics.px(43, workspace.t)
                anchors.right: identityChevron.left
                anchors.rightMargin: Metrics.px(7, workspace.t)
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Text {
                    width: parent.width
                    text: "KetPlus"
                    color: workspace.t.textMain
                    elide: Text.ElideRight
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.caption
                    font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    text: "Personal workspace"
                    color: workspace.t.textMuted
                    elide: Text.ElideRight
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.micro
                }
            }
            TintedIcon {
                id: identityChevron
                anchors.right: parent.right
                anchors.rightMargin: Metrics.px(10, workspace.t)
                anchors.verticalCenter: parent.verticalCenter
                width: Metrics.px(14, workspace.t)
                height: Metrics.px(14, workspace.t)
                source: "qrc:/agent/icons/chevron-down.svg"
                color: workspace.t.textMuted
            }
            MouseArea {
                id: identityMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: agentHost.requestProductNavigation("connections")
                Accessible.name: "Account and connections"
            }
        }
    }
}
