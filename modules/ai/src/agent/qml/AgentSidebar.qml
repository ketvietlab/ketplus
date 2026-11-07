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
    property alias worktreeList: worktreeList
    objectName: "worktreeSidebar"
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

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Metrics.px(62, workspace.t)

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: Metrics.px(12, workspace.t)
                        anchors.rightMargin: Metrics.px(12, workspace.t)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Metrics.px(2, workspace.t)

                        Text {
                            width: parent.width
                            text: "WORKSPACE"
                            color: workspace.t.textMuted
                            elide: Text.ElideRight
                            font.family: workspace.t.uiFont
                            font.pixelSize: workspace.t.caption
                            font.weight: Font.Bold
                        }
                        Basic.ToolButton {
                            id: repositoryPicker
                            objectName: "agentRepositoryPicker"
                            width: parent.width
                            height: Metrics.px(26, workspace.t)
                            padding: 0
                            onClicked: agentHost.repositoryPickerRequested()
                            Accessible.name: "Switch repository"
                            contentItem: RowLayout {
                                spacing: Metrics.px(6, workspace.t)
                                TintedIcon {
                                    Layout.preferredWidth: Metrics.px(14, workspace.t)
                                    Layout.preferredHeight: Metrics.px(14, workspace.t)
                                    source: "qrc:/agent/icons/folder.svg"
                                    color: workspace.t.textMuted
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: agentHost.repositoryName || "Open repository"
                                    color: workspace.t.textMain
                                    elide: Text.ElideRight
                                    font.family: workspace.t.uiFont
                                    font.pixelSize: workspace.t.body
                                }
                                TintedIcon {
                                    Layout.preferredWidth: Metrics.px(14, workspace.t)
                                    Layout.preferredHeight: Metrics.px(14, workspace.t)
                                    source: "qrc:/agent/icons/chevron-down.svg"
                                    color: workspace.t.textMuted
                                }
                            }
                            background: Rectangle {
                                radius: Metrics.px(4, workspace.t)
                                color: repositoryPicker.hovered ? workspace.t.surfaceHover : "transparent"
                                border.color: repositoryPicker.visualFocus ? workspace.t.focus : "transparent"
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

                RowLayout {
                    Layout.leftMargin: Metrics.px(12, workspace.t)
                    Layout.rightMargin: Metrics.px(10, workspace.t)
                    Layout.topMargin: Metrics.px(10, workspace.t)
                    Layout.bottomMargin: Metrics.px(5, workspace.t)
                    Layout.fillWidth: true
                    height: Metrics.px(22, workspace.t)
                    spacing: Metrics.px(4, workspace.t)

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: "WORKTREES"
                        color: workspace.t.textMuted
                        font.family: workspace.t.uiFont
                        font.pixelSize: workspace.t.micro
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }
                    Item { Layout.fillWidth: true }
                    Basic.ToolButton {
                        id: worktreeScopeButton
                        objectName: "worktreeScopeButton"
                        Layout.alignment: Qt.AlignVCenter
                        visible: false
                        implicitWidth: Metrics.px(42, workspace.t)
                        implicitHeight: Metrics.px(20, workspace.t)
                        padding: 0
                        onClicked: {
                            workspace.showAllWorktrees = !workspace.showAllWorktrees
                            Qt.callLater(workspace.revealSelectedWorktree)
                        }
                        Accessible.name: workspace.showAllWorktrees
                                         ? "Show relevant worktrees"
                                         : "Show all worktrees"
                        contentItem: Text {
                            text: workspace.filteredWorktrees.length + "/" + agentHost.worktrees.length
                            color: worktreeScopeButton.hovered
                                   ? workspace.t.textMuted : workspace.t.textDisabled
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            font.family: workspace.t.uiFont
                            font.pixelSize: workspace.t.micro
                        }
                        background: Rectangle {
                            radius: Metrics.px(4, workspace.t)
                            color: worktreeScopeButton.hovered
                                   ? workspace.t.surfaceHover : "transparent"
                        }
                        KetToolTip {
                            tokens: workspace.t
                            visible: worktreeScopeButton.hovered
                            text: workspace.showAllWorktrees
                                  ? "Show worktrees with conversations"
                                  : "Show all linked worktrees"
                        }
                    }
                    Rectangle {
                        id: addWorktreeButton
                        objectName: "addWorktreeButton"
                        Layout.preferredWidth: Metrics.px(20, workspace.t)
                        Layout.preferredHeight: Metrics.px(20, workspace.t)
                        width: Metrics.px(20, workspace.t)
                        height: Metrics.px(20, workspace.t)
                        radius: Metrics.px(4, workspace.t)
                        color: addWorktreeMouse.containsMouse ? workspace.t.surfaceHover : "transparent"

                        TintedIcon {
                            anchors.centerIn: parent
                            width: Metrics.px(14, workspace.t)
                            height: Metrics.px(14, workspace.t)
                            source: "qrc:/agent/icons/plus.svg"
                            color: workspace.t.textMuted
                        }
                        MouseArea {
                            id: addWorktreeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: agentHost.requestAddWorktree()
                        }
                        KetToolTip {
                            tokens: workspace.t
                            visible: addWorktreeMouse.containsMouse
                            text: "Create worktree"
                        }
                    }
                }

                Rectangle {
                    id: worktreeSearchBox
                    objectName: "worktreeSearchBox"
                    Layout.fillWidth: true
                    Layout.preferredHeight: workspace.showAllWorktrees || workspace.worktreeQuery.length > 0 ? 30 : 0
                    Layout.leftMargin: Metrics.px(8, workspace.t)
                    Layout.rightMargin: Metrics.px(8, workspace.t)
                    Layout.bottomMargin: visible ? Metrics.px(6, workspace.t) : 0
                    visible: height > 0
                    radius: Metrics.px(6, workspace.t)
                    color: workspace.t.interactiveBackground
                    border.color: worktreeSearch.activeFocus ? workspace.t.focus : workspace.t.border

                    TintedIcon {
                        id: worktreeSearchIcon
                        anchors.left: parent.left
                        anchors.leftMargin: Metrics.px(8, workspace.t)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Metrics.px(14, workspace.t)
                        height: Metrics.px(14, workspace.t)
                        source: "qrc:/agent/icons/search.svg"
                        color: workspace.t.textMuted
                    }

                    Basic.TextField {
                        id: worktreeSearch
                        objectName: "worktreeSearch"
                        anchors.left: worktreeSearchIcon.right
                        anchors.right: clearWorktreeSearch.left
                        anchors.leftMargin: Metrics.px(6, workspace.t)
                        anchors.rightMargin: Metrics.px(4, workspace.t)
                        anchors.verticalCenter: parent.verticalCenter
                        height: parent.height
                        text: workspace.worktreeQuery
                        placeholderText: "Filter worktrees"
                        placeholderTextColor: workspace.t.textMuted
                        color: workspace.t.textMain
                        selectionColor: workspace.t.accentMuted
                        selectedTextColor: workspace.t.textMain
                        font.family: workspace.t.uiFont
                        font.pixelSize: workspace.t.body
                        background: null
                        onTextChanged: {
                            if (workspace.worktreeQuery !== text)
                                workspace.worktreeQuery = text
                        }
                        Keys.onEscapePressed: clear()
                    }

                    Basic.ToolButton {
                        id: clearWorktreeSearch
                        anchors.right: parent.right
                        anchors.rightMargin: Metrics.px(3, workspace.t)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Metrics.px(24, workspace.t)
                        height: Metrics.px(24, workspace.t)
                        visible: workspace.worktreeQuery.length > 0
                        text: "×"
                        padding: 0
                        contentItem: Text {
                            text: clearWorktreeSearch.text
                            color: workspace.t.textMuted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.family: workspace.t.uiFont
                            font.pixelSize: workspace.t.reading
                        }
                        background: Rectangle {
                            radius: Metrics.px(4, workspace.t)
                            color: clearWorktreeSearch.hovered ? workspace.t.surfaceHover : "transparent"
                        }
                        onClicked: worktreeSearch.clear()
                        KetToolTip {
                            tokens: workspace.t
                            visible: clearWorktreeSearch.hovered
                            text: "Clear filter"
                        }
                    }
                }

                ListView {
                    id: worktreeList
                    objectName: "worktreeList"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(contentHeight,
                        workspace.filteredWorktrees.length > 5
                        ? Math.max(260, workspace.height * 0.40) : 126)
                    Layout.maximumHeight: workspace.filteredWorktrees.length > 5
                                          ? Math.max(300, workspace.height * 0.55) : 126
                    Layout.leftMargin: Metrics.px(8, workspace.t)
                    Layout.rightMargin: Metrics.px(8, workspace.t)
                    spacing: Metrics.px(2, workspace.t)
                    clip: true
                    reuseItems: true
                    cacheBuffer: height * 2
                    model: workspace.filteredWorktrees
                    currentIndex: workspace.selectedWorktreeIndex()
                    onModelChanged: Qt.callLater(workspace.revealSelectedWorktree)
                    onCurrentIndexChanged: Qt.callLater(workspace.revealSelectedWorktree)

                    delegate: Rectangle {
                        required property var modelData
                        width: worktreeList.width
                        height: Metrics.px(42, workspace.t)
                        radius: Metrics.px(5, workspace.t)
                        color: modelData.selected ? workspace.t.accentSubtle
                                                : modelData.running ? workspace.t.panelSubtle
                                                : worktreeMouse.containsMouse ? workspace.t.surfaceHover : "transparent"
                        border.color: "transparent"

                        TintedIcon {
                            id: branchIcon
                            anchors.left: parent.left
                            anchors.leftMargin: Metrics.px(8, workspace.t)
                            anchors.verticalCenter: parent.verticalCenter
                            width: Metrics.px(14, workspace.t)
                            height: Metrics.px(14, workspace.t)
                            source: "qrc:/agent/icons/git-branch.svg"
                            color: modelData.running ? workspace.t.positive
                                  : modelData.selected ? workspace.t.tabActiveText : workspace.t.textMuted
                        }
                        Column {
                            anchors.left: branchIcon.right
                            anchors.right: statusDot.left
                            anchors.leftMargin: Metrics.px(8, workspace.t)
                            anchors.rightMargin: Metrics.px(6, workspace.t)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 1
                            Text {
                                width: parent.width
                                text: modelData.branch
                                color: workspace.t.textMain
                                elide: Text.ElideMiddle
                                font.family: workspace.t.uiFont
                                font.pixelSize: workspace.t.body
                                font.weight: modelData.running || modelData.selected ? Font.DemiBold : Font.Medium
                            }
                            Text {
                                width: parent.width
                                text: modelData.running ? "Agent working"
                                      : modelData.selected ? "Main workspace"
                                                           : "Independent workspace"
                                color: modelData.running ? workspace.t.positive : workspace.t.textMuted
                                elide: Text.ElideMiddle
                                font.family: workspace.t.uiFont
                                font.pixelSize: workspace.t.micro
                            }
                        }
                        Rectangle {
                            id: statusDot
                            anchors.right: parent.right
                            anchors.rightMargin: Metrics.px(9, workspace.t)
                            anchors.verticalCenter: parent.verticalCenter
                            width: Metrics.px(6, workspace.t)
                            height: Metrics.px(6, workspace.t)
                            radius: Metrics.px(3, workspace.t)
                            color: modelData.running ? workspace.t.positive : workspace.t.textDisabled
                        }
                        MouseArea {
                            id: worktreeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function(mouse) {
                                if (mouse.button === Qt.RightButton)
                                    agentHost.requestWorktreeContextMenu(modelData.path)
                                else
                                    agentHost.selectWorktree(modelData.path)
                            }
                        }
                        KetToolTip {
                            tokens: workspace.t
                            visible: worktreeMouse.containsMouse
                            delay: 500
                            text: modelData.branch + "\n" + modelData.status + "\n" + modelData.path
                        }
                    }

                    ScrollBar.vertical: Basic.ScrollBar {
                        policy: ScrollBar.AsNeeded
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: Metrics.px(16, workspace.t)
                    Layout.rightMargin: Metrics.px(16, workspace.t)
                    Layout.topMargin: Metrics.px(4, workspace.t)
                    visible: agentHost.worktrees.length > 0 && workspace.filteredWorktrees.length === 0
                    text: "No worktrees match this filter"
                    color: workspace.t.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.family: workspace.t.uiFont
                    font.pixelSize: workspace.t.body
                }

                Basic.Button {
                    id: allWorktreesButton
                    objectName: "allWorktreesButton"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Metrics.px(28, workspace.t)
                    Layout.leftMargin: Metrics.px(9, workspace.t)
                    Layout.rightMargin: Metrics.px(9, workspace.t)
                    Layout.topMargin: Metrics.px(4, workspace.t)
                    padding: 0
                    onClicked: {
                        workspace.showAllWorktrees = !workspace.showAllWorktrees
                        if (!workspace.showAllWorktrees)
                            workspace.worktreeQuery = ""
                        Qt.callLater(workspace.revealSelectedWorktree)
                    }
                    contentItem: RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Metrics.px(8, workspace.t)
                        anchors.rightMargin: Metrics.px(8, workspace.t)
                        spacing: Metrics.px(7, workspace.t)
                        Text {
                            Layout.fillWidth: true
                            text: workspace.showAllWorktrees ? "Show relevant worktrees"
                                                             : "View all worktrees"
                            color: workspace.t.textSecondary
                            font.family: workspace.t.uiFont
                            font.pixelSize: workspace.t.caption
                        }
                        TintedIcon {
                            Layout.preferredWidth: Metrics.px(14, workspace.t)
                            Layout.preferredHeight: Metrics.px(14, workspace.t)
                            source: "qrc:/agent/icons/chevron-right.svg"
                            color: workspace.t.textMuted
                            rotation: workspace.showAllWorktrees ? 90 : 0
                        }
                    }
                    background: Rectangle {
                        radius: Metrics.px(5, workspace.t)
                        color: allWorktreesButton.hovered ? workspace.t.surfaceHover : "transparent"
                        border.color: allWorktreesButton.visualFocus ? workspace.t.focus : "transparent"
                    }
                }

                RowLayout {
                    Layout.leftMargin: Metrics.px(12, workspace.t)
                    Layout.rightMargin: Metrics.px(10, workspace.t)
                    Layout.topMargin: Metrics.px(14, workspace.t)
                    Layout.bottomMargin: Metrics.px(5, workspace.t)
                    Layout.fillWidth: true
                    height: Metrics.px(22, workspace.t)
                    spacing: Metrics.px(4, workspace.t)

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: "CONVERSATIONS"
                        color: workspace.t.textMuted
                        font.family: workspace.t.uiFont
                        font.pixelSize: workspace.t.micro
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        id: addConversationButton
                        objectName: "addConversationButton"
                        Layout.preferredWidth: Metrics.px(20, workspace.t)
                        Layout.preferredHeight: Metrics.px(20, workspace.t)
                        width: Metrics.px(20, workspace.t)
                        height: Metrics.px(20, workspace.t)
                        radius: Metrics.px(4, workspace.t)
                        color: addConversationMouse.containsMouse ? workspace.t.surfaceHover : "transparent"

                        TintedIcon {
                            anchors.centerIn: parent
                            width: Metrics.px(14, workspace.t)
                            height: Metrics.px(14, workspace.t)
                            source: "qrc:/agent/icons/square-pen.svg"
                            color: workspace.t.textMuted
                        }
                        MouseArea {
                            id: addConversationMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: agentHost.requestNewSession()
                        }
                        KetToolTip {
                            tokens: workspace.t
                            visible: addConversationMouse.containsMouse
                            text: "New conversation"
                        }
                    }
                }

                ListView {
                    id: conversationList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: Metrics.px(8, workspace.t)
                    Layout.rightMargin: Metrics.px(8, workspace.t)
                    spacing: Metrics.px(2, workspace.t)
                    clip: true
                    model: agentHost.conversations
                    property string firstConversationId: ""
                    onModelChanged: {
                        const firstId = model.length > 0 ? model[0].id : ""
                        if (firstId !== firstConversationId) {
                            firstConversationId = firstId
                            Qt.callLater(function() { conversationList.positionViewAtBeginning() })
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
                        status: modelData.needsInput ? Kit.KetTheme.ConversationStatus.NeedsInput
                              : modelData.awaitingReply ? Kit.KetTheme.ConversationStatus.AwaitingReply
                              : modelData.running ? Kit.KetTheme.ConversationStatus.Working
                              : /fail|error|disconnect|missing/i.test(modelData.status || "") ? Kit.KetTheme.ConversationStatus.Error
                              : /stop/i.test(modelData.status || "") ? Kit.KetTheme.ConversationStatus.Stopped
                              : modelData.started ? Kit.KetTheme.ConversationStatus.Completed
                              : Kit.KetTheme.ConversationStatus.NotStarted
                        statusLabel: modelData.status || Kit.KetTheme.conversationStatusLabel(status)
                        readonly property bool animationAllowed: agentHost.motionEnabled
                            && !agentHost.restoringHistory && workspace.visible
                            && workspace.Window.active && workspace.Window.visibility !== Window.Minimized
                            && visible && y + height >= conversationList.contentY
                            && y <= conversationList.contentY + conversationList.height
                        // Preserve the host's motion/visibility policy while sharing the kit clock.
                        function syncClock() {
                            const active = spins && animationAllowed
                            if (holdsClock !== active) {
                                holdsClock = active
                                Kit.KetTheme.holdAnimationClock(active)
                            }
                        }
                        onAnimationAllowedChanged: syncClock()
                        MouseArea {
                            id: conversationMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            cursorShape: Qt.PointingHandCursor
                            onClicked: (mouse) => {
                                if (mouse.button === Qt.RightButton) {
                                    conversationMenu.popup()
                                } else {
                                    agentHost.selectConversation(conversationId)
                                    workspace.focusPrompt()
                                }
                            }
                            onDoubleClicked: (mouse) => {
                                if (mouse.button === Qt.LeftButton) {
                                    mouse.accepted = true
                                    Qt.callLater(function() {
                                        agentHost.requestRenameConversation(conversationId)
                                    })
                                }
                            }
                        }
                        Basic.Menu {
                            id: conversationMenu
                            objectName: "conversationContextMenu"
                            readonly property color backgroundColor: workspace.t.panelBackground
                                                                     || workspace.t.surfaceRaised
                                                                     || "#20252b"
                            readonly property color borderColor: workspace.t.borderStrong
                                                                 || "#3b424c"
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
                                    color: !renameConversationMenuItem.enabled
                                           ? workspace.t.textDisabled
                                           : renameConversationMenuItem.highlighted
                                             ? workspace.t.accentText : workspace.t.textMain
                                    font: renameConversationMenuItem.font
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    color: renameConversationMenuItem.highlighted
                                           ? workspace.t.accentSubtle : "transparent"
                                    border.color: renameConversationMenuItem.visualFocus
                                                  ? workspace.t.focus : "transparent"
                                    border.width: renameConversationMenuItem.visualFocus ? 1 : 0
                                    radius: Metrics.px(5, workspace.t)
                                }
                                onTriggered: Qt.callLater(function() {
                                    agentHost.requestRenameConversation(conversationId)
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
                                    color: !forkConversationMenuItem.enabled
                                           ? workspace.t.textDisabled
                                           : forkConversationMenuItem.highlighted
                                             ? workspace.t.accentText : workspace.t.textMain
                                    font: forkConversationMenuItem.font
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    color: forkConversationMenuItem.highlighted
                                           ? workspace.t.accentSubtle : "transparent"
                                    radius: Metrics.px(5, workspace.t)
                                }
                                onTriggered: Qt.callLater(function() {
                                    agentHost.requestForkConversation(conversationId)
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
                                    color: deleteConversationMenuItem.enabled
                                           ? (deleteConversationMenuItem.highlighted
                                              ? workspace.t.accentText : workspace.t.danger)
                                           : workspace.t.textDisabled
                                    font: deleteConversationMenuItem.font
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    color: deleteConversationMenuItem.highlighted
                                           ? workspace.t.accentSubtle : "transparent"
                                    radius: Metrics.px(5, workspace.t)
                                }
                                onTriggered: Qt.callLater(function() {
                                    agentHost.requestDeleteConversation(conversationId)
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
                                    color: archiveConversationMenuItem.enabled
                                           ? (archiveConversationMenuItem.highlighted
                                              ? workspace.t.accentText : workspace.t.textMain)
                                           : workspace.t.textDisabled
                                    font: archiveConversationMenuItem.font
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    color: archiveConversationMenuItem.highlighted
                                           ? workspace.t.accentSubtle : "transparent"
                                    radius: Metrics.px(5, workspace.t)
                                }
                                onTriggered: Qt.callLater(function() {
                                    agentHost.requestArchiveConversation(conversationId)
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
                        Item { Layout.fillWidth: true }
                        Text {
                            objectName: "weeklyLimitValue"
                            text: weeklyFooter.quota.available
                                  ? Math.round(weeklyFooter.quota.usedPercent) + "% used"
                                  : weeklyFooter.quota.loading ? "Loading..." : "--"
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
                            contentItem: TintedIcon { source: "qrc:/agent/icons/rotate-ccw.svg"; color: workspace.t.textMuted }
                            background: Rectangle { color: parent.hovered ? workspace.t.surfaceHover : "transparent"; radius: Metrics.px(4, workspace.t) }
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
                                text: weeklyFooter.quota.available
                                    ? (weeklyFooter.quota.resetsAt > 0
                                       ? "Resets " + Qt.formatDateTime(new Date(weeklyFooter.quota.resetsAt * 1000), "ddd, dd MMM yyyy hh:mm")
                                       : "Reset time unavailable")
                                    : "Weekly limit unavailable"
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
                        color: settingsNavigationButton.hovered ? workspace.t.surfaceHover
                                                                : "transparent"
                        border.color: settingsNavigationButton.visualFocus ? workspace.t.focus
                                                                           : "transparent"
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
