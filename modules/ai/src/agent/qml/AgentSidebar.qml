import Ket
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
    color: KetTheme.t.sidebarBg

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
            Layout.leftMargin: KetTheme.px(6, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(6, KetTheme.zoom)
            name: agentHost.repositoryName || "Open repository"
            onActionTriggered: agentHost.requestNewSession()
        }

        ListView {
            id: conversationList
            objectName: "sidebarConversationList"
            visible: repositoryGroup.expanded
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: KetTheme.px(8, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(8, KetTheme.zoom)
            spacing: KetTheme.px(2, KetTheme.zoom)
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
                // The Working indicator's policy (docs/design-system.md): spin unless reduced motion,
                // restoring, hidden, minimized or scrolled away. Behind another app the kit clock
                // slows to 4 steps a second rather than stopping.
                readonly property bool animationAllowed: agentHost.workingMotionEnabled && !agentHost.restoringHistory && workspace.visible && workspace.Window.visibility !== Window.Minimized && workspace.Window.visibility !== Window.Hidden && visible && y + height >= conversationList.contentY && y <= conversationList.contentY + conversationList.height
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
                Kit.KetMenu {
                    id: conversationMenu
                    objectName: "conversationContextMenu"
                    Kit.KetMenuItem {
                        id: renameConversationMenuItem
                        objectName: "renameConversationMenuItem"
                        text: "Rename…"
                        iconName: "edit"
                        onTriggered: Qt.callLater(() => agentHost.requestRenameConversation(conversationId))
                    }
                    Kit.KetMenuItem {
                        objectName: "forkConversationMenuItem"
                        text: "Fork conversation"
                        iconName: "branch"
                        enabled: modelData.forkable === true
                        onTriggered: Qt.callLater(() => agentHost.requestForkConversation(conversationId))
                    }
                    Kit.KetMenuItem {
                        objectName: "changeConversationWorktreeMenuItem"
                        text: "Change worktree…"
                        iconName: "branch"
                        onTriggered: Qt.callLater(() => agentHost.requestConversationWorktree(conversationId))
                    }
                    // The reference other conversations use to message this one.
                    Kit.KetMenuItem {
                        objectName: "copyConversationReferenceMenuItem"
                        readonly property string reference: modelData.number ? "@agent/" + modelData.number : ""
                        visible: reference.length > 0
                        height: visible ? implicitHeight : 0
                        text: "Copy reference"
                        iconName: "copy"
                        shortcut: reference
                        onTriggered: agentHost.copyText(reference)
                    }
                    Kit.KetMenuSeparator {}
                    Kit.KetMenuItem {
                        objectName: "archiveConversationMenuItem"
                        text: "Archive"
                        iconName: "archive"
                        onTriggered: Qt.callLater(() => agentHost.requestArchiveConversation(conversationId))
                    }
                    Kit.KetMenuItem {
                        objectName: "deleteConversationMenuItem"
                        text: "Delete…"
                        iconName: "trash"
                        danger: true
                        enabled: !modelData.running
                        onTriggered: Qt.callLater(() => agentHost.requestDeleteConversation(conversationId))
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
            Layout.preferredHeight: KetTheme.px(58, KetTheme.zoom)
            RowLayout {
                anchors.left: parent.left
                anchors.leftMargin: KetTheme.px(12, KetTheme.zoom)
                anchors.right: parent.right
                anchors.rightMargin: KetTheme.px(8, KetTheme.zoom)
                anchors.top: parent.top
                anchors.topMargin: KetTheme.px(8, KetTheme.zoom)
                spacing: KetTheme.px(4, KetTheme.zoom)
                Text {
                    text: "Usage"
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                }
                Item {
                    Layout.fillWidth: true
                }
                Text {
                    objectName: "weeklyLimitValue"
                    text: weeklyFooter.quota.available ? Math.round(weeklyFooter.quota.usedPercent) + "% used" : weeklyFooter.quota.loading ? "Loading..." : "--"
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                }
                Basic.ToolButton {
                    id: refreshWeeklyLimitButton
                    objectName: "refreshWeeklyLimitButton"
                    Layout.preferredWidth: KetTheme.px(22, KetTheme.zoom)
                    Layout.preferredHeight: KetTheme.px(24, KetTheme.zoom)
                    enabled: agentHost.statusProvider === "codex" && agentHost.agentAvailable && !weeklyFooter.quota.loading
                    contentItem: Item {
                        implicitWidth: KetTheme.px(16, KetTheme.zoom)
                        implicitHeight: implicitWidth
                        KetIcon {
                            anchors.centerIn: parent
                            box: Math.min(parent.width, parent.height)
                            name: KetTheme.iconForLucide("rotate-ccw")
                            color: KetTheme.t.textMuted
                        }
                    }
                    background: Rectangle {
                        color: parent.hovered ? KetTheme.t.surfaceHover : "transparent"
                        radius: KetTheme.px(4, KetTheme.zoom)
                    }
                    onClicked: agentHost.requestWeeklyLimit()
                    KetToolTip {
                        objectName: "ketToolTip"
                        visible: refreshWeeklyLimitButton.hovered
                        text: "Refresh weekly limit"
                    }
                    Accessible.name: "Refresh weekly limit"
                }
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: KetTheme.px(12, KetTheme.zoom)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: KetTheme.px(12, KetTheme.zoom)
                height: KetTheme.px(3, KetTheme.zoom)
                color: KetTheme.t.panelBorder
                Rectangle {
                    objectName: "weeklyLimitProgress"
                    height: parent.height
                    width: parent.width * (weeklyFooter.quota.available ? Math.min(1, Math.max(0, weeklyFooter.quota.usedPercent / KetTheme.px(100, KetTheme.zoom))) : 0)
                    color: weeklyFooter.quota.usedPercent >= 90 ? KetTheme.t.warning : KetTheme.t.accent
                }
                MouseArea {
                    id: weeklyLimitMouse
                    anchors.fill: parent
                    anchors.margins: -KetTheme.px(5, KetTheme.zoom)
                    hoverEnabled: true
                    KetToolTip {
                        objectName: "ketToolTip"
                        visible: weeklyLimitMouse.containsMouse
                        text: weeklyFooter.quota.available ? (weeklyFooter.quota.resetsAt > 0 ? "Resets " + Qt.formatDateTime(new Date(weeklyFooter.quota.resetsAt * 1000), "ddd, dd MMM yyyy hh:mm") : "Reset time unavailable") : "Weekly limit unavailable"
                    }
                }
            }
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: KetTheme.t.panelBorder
            }
        }

        Basic.Button {
            id: settingsNavigationButton
            objectName: "productNavigation_settings"
            Layout.fillWidth: true
            Layout.preferredHeight: KetTheme.px(34, KetTheme.zoom)
            Layout.leftMargin: KetTheme.px(8, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(8, KetTheme.zoom)
            Layout.topMargin: KetTheme.px(4, KetTheme.zoom)
            padding: 0
            onClicked: agentHost.requestSettings()
            contentItem: RowLayout {
                anchors.fill: parent
                anchors.leftMargin: KetTheme.px(9, KetTheme.zoom)
                anchors.rightMargin: KetTheme.px(9, KetTheme.zoom)
                spacing: KetTheme.px(9, KetTheme.zoom)
                KetIcon {
                    Layout.preferredWidth: KetTheme.px(16, KetTheme.zoom)
                    Layout.preferredHeight: KetTheme.px(16, KetTheme.zoom)
                    name: KetTheme.iconForLucide("settings")
                    color: KetTheme.t.textMuted
                    box: KetTheme.px(16, KetTheme.zoom)
                }
                Text {
                    Layout.fillWidth: true
                    text: "Settings"
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                }
                Text {
                    text: "⌘ ,"
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                }
            }
            background: Rectangle {
                radius: KetTheme.px(6, KetTheme.zoom)
                color: settingsNavigationButton.hovered ? KetTheme.t.surfaceHover : "transparent"
                border.color: settingsNavigationButton.visualFocus ? KetTheme.t.colorFocus : "transparent"
            }
            Accessible.name: "Settings"
        }

        Item {
            objectName: "productIdentity"
            Layout.fillWidth: true
            Layout.preferredHeight: KetTheme.px(54, KetTheme.zoom)
            Layout.leftMargin: KetTheme.px(8, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(8, KetTheme.zoom)

            Rectangle {
                anchors.fill: parent
                anchors.margins: KetTheme.px(2, KetTheme.zoom)
                radius: KetTheme.px(6, KetTheme.zoom)
                color: identityMouse.containsMouse ? KetTheme.t.surfaceHover : "transparent"
            }
            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: KetTheme.px(9, KetTheme.zoom)
                anchors.verticalCenter: parent.verticalCenter
                width: KetTheme.px(25, KetTheme.zoom)
                height: KetTheme.px(25, KetTheme.zoom)
                radius: KetTheme.px(13, KetTheme.zoom)
                color: KetTheme.t.accentMuted
                Text {
                    anchors.centerIn: parent
                    text: "KP"
                    color: KetTheme.t.tabActiveText
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                    font.weight: Font.Bold
                }
            }
            Column {
                anchors.left: parent.left
                anchors.leftMargin: KetTheme.px(43, KetTheme.zoom)
                anchors.right: identityChevron.left
                anchors.rightMargin: KetTheme.px(7, KetTheme.zoom)
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Text {
                    width: parent.width
                    text: "KetPlus"
                    color: KetTheme.t.textMain
                    elide: Text.ElideRight
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                    font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    text: "Personal workspace"
                    color: KetTheme.t.textMuted
                    elide: Text.ElideRight
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                }
            }
            KetIcon {
                id: identityChevron
                anchors.right: parent.right
                anchors.rightMargin: KetTheme.px(10, KetTheme.zoom)
                anchors.verticalCenter: parent.verticalCenter
                width: KetTheme.px(14, KetTheme.zoom)
                height: KetTheme.px(14, KetTheme.zoom)
                name: KetTheme.iconForLucide("chevron-down")
                color: KetTheme.t.textMuted
                box: KetTheme.px(14, KetTheme.zoom)
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
