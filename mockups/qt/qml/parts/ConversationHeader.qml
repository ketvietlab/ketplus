// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// The conversation's 44 px chat-local header: reference, title, status, worktree, tags and the
// pane toggles.
Rectangle {
    id: header

    required property var window
    required property var conversation
    property bool detailsOpen: false
    readonly property int status: conversation.status

    signal toggleDetails(string tab)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function tone(status) {
        switch (status) {
        case KetTheme.ConversationStatus.Completed: return KetTheme.BadgeTone.Success
        case KetTheme.ConversationStatus.Working: return KetTheme.BadgeTone.Accent
        case KetTheme.ConversationStatus.NeedsInput: return KetTheme.BadgeTone.Warning
        case KetTheme.ConversationStatus.Error: return KetTheme.BadgeTone.Danger
        }
        return KetTheme.BadgeTone.Neutral
    }

    implicitHeight: px(44)
    color: KetTheme.t.pageBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: KetTheme.t.panelBorder
    }

    Row {
        x: header.px(16)
        anchors.verticalCenter: parent.verticalCenter
        spacing: header.px(8)

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "@agent/" + header.conversation.number
            color: KetTheme.t.textMuted
            font.family: KetTheme.monoFontFamily
            font.pixelSize: header.px(13)
        }

        Basic.AbstractButton {
            id: titleButton
            anchors.verticalCenter: parent.verticalCenter
            hoverEnabled: true
            text: header.conversation.title
            onDoubleClicked: header.window.renameDialog.open()
            Basic.ToolTip.visible: hovered
            Basic.ToolTip.delay: 800
            Basic.ToolTip.text: "Double-click to rename"
            background: null
            contentItem: Text {
                text: titleButton.text
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: header.px(15)
                font.weight: Font.Medium
            }
        }

        KetBadge {
            anchors.verticalCenter: parent.verticalCenter
            text: KetTheme.conversationStatusLabel(header.status)
            tone: header.tone(header.status)
        }

        KetButton {
            visible: !header.window.compact
            anchors.verticalCenter: parent.verticalCenter
            controlSize: KetTheme.ControlSize.Small
            iconName: "branch"
            text: header.conversation.worktree + "  ▾"
            onClicked: header.window.picker.open()
        }

        Repeater {
            model: header.window.compact ? [] : header.conversation.tags
            KetTag {
                required property var modelData
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.label
                tagColor: modelData.color
                filterable: true
                onClicked: header.window.tagFilter = modelData.label
            }
        }

        KetIconButton {
            id: addTag
            visible: !header.window.compact
            anchors.verticalCenter: parent.verticalCenter
            controlSize: KetTheme.ControlSize.Small
            iconName: "plus"
            text: "Add tag"
            onClicked: tagMenu.popup(addTag, 0, addTag.height)
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: header.px(10)
        anchors.verticalCenter: parent.verticalCenter
        spacing: header.px(2)

        KetIconButton {
            iconName: header.window.compact ? "fullscreen" : "panel-left"
            text: header.window.compact ? "Leave compact window" : "Compact window"
            onClicked: header.window.setCompact(!header.window.compact)
        }
        KetIconButton {
            iconName: "compose"
            text: "New conversation"
            onClicked: header.window.showScreen("workspace/welcome")
        }
        KetIconButton {
            visible: !header.window.compact
            iconName: "layers"
            text: "Atlases"
            checkable: true
            checked: header.detailsOpen
            onClicked: header.toggleDetails("atlas")
        }
        KetIconButton {
            iconName: "compare"
            text: "Changes"
            onClicked: header.toggleDetails("changes")
        }
        KetIconButton {
            iconName: "terminal"
            text: "Terminal"
            onClicked: header.toggleDetails("terminal")
        }
    }

    KetMenu {
        id: tagMenu
        Repeater {
            model: Object.keys(Fixtures.tagColors)
            KetMenuItem {
                required property string modelData
                text: modelData
                iconName: "tag"
                checkable: true
                checked: header.conversation.tags.some(t => t.label === modelData)
                onTriggered: header.window.notify((checked ? "Tagged " : "Removed tag ") + modelData)
            }
        }
        KetMenuSeparator {}
        KetMenuItem { text: "Manage tags…"; iconName: "settings"; onTriggered: header.window.notify("Tags are managed from any conversation") }
    }
}
