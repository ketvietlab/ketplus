import Ket
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket as Kit

Basic.Popup {
    id: skillsPopup
    required property var workspace
    required property var anchorItem
    readonly property var root: workspace
    objectName: "skillsPopup"
    focus: true
    y: -height - 4
    x: Math.min(0, root.width - anchorItem.mapToItem(root, 0, 0).x - width - 12)
    width: Math.min(KetTheme.px(420, KetTheme.zoom), root.width - KetTheme.px(24, KetTheme.zoom))
    height: Math.min(root.height - KetTheme.px(32, KetTheme.zoom), skillsContent.implicitHeight + KetTheme.px(16, KetTheme.zoom))
    padding: KetTheme.px(8, KetTheme.zoom)
    background: Rectangle {
        color: root.t.surfaceRaised
        radius: KetTheme.px(12, KetTheme.zoom)
        border.color: root.t.borderStrong
    }
    ColumnLayout {
        id: skillsContent
        width: parent.width
        spacing: KetTheme.px(6, KetTheme.zoom)
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: "Skills"
                color: root.t.textMain
                font.family: root.t.uiFont
                font.pixelSize: root.t.reading
                font.weight: Font.DemiBold
            }
            Text {
                text: root.enabledSkillCount() + " enabled"
                color: root.t.textMuted
                font.family: root.t.uiFont
                font.pixelSize: root.t.caption
            }
        }
        Text {
            Layout.fillWidth: true
            visible: agentHost.skills.length === 0
            text: "No SKILL.md files found in this project or user skills."
            color: root.t.textSecondary
            font.family: root.t.uiFont
            font.pixelSize: root.t.body
            wrapMode: Text.Wrap
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(root.height * 0.46, contentHeight)
            clip: true
            model: agentHost.skills
            delegate: Basic.ItemDelegate {
                id: skillOption
                required property var modelData
                width: ListView.view.width
                height: KetTheme.px(62, KetTheme.zoom)
                padding: KetTheme.px(8, KetTheme.zoom)
                contentItem: RowLayout {
                    spacing: KetTheme.px(10, KetTheme.zoom)
                    Rectangle {
                        Layout.preferredWidth: KetTheme.px(18, KetTheme.zoom)
                        Layout.preferredHeight: KetTheme.px(18, KetTheme.zoom)
                        radius: KetTheme.px(5, KetTheme.zoom)
                        color: modelData.enabled ? root.t.accent : "transparent"
                        border.width: 1
                        border.color: modelData.enabled ? root.t.accentHover : root.t.borderStrong
                        Text {
                            anchors.centerIn: parent
                            text: modelData.enabled ? "✓" : ""
                            color: Kit.KetTheme.t.actionPrimaryInk
                            font.pixelSize: root.t.body
                        }
                    }
                    Column {
                        Layout.fillWidth: true
                        spacing: KetTheme.px(3, KetTheme.zoom)
                        Text {
                            width: parent.width
                            text: modelData.name + " · " + modelData.source
                            color: root.t.textMain
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.reading
                            font.weight: modelData.enabled ? Font.DemiBold : Font.Medium
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: modelData.description || modelData.displayPath
                            color: root.t.textMuted
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.caption
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: modelData.displayPath
                            color: root.t.textDisabled
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.micro
                            elide: Text.ElideMiddle
                        }
                    }
                }
                background: Rectangle {
                    radius: KetTheme.px(8, KetTheme.zoom)
                    color: modelData.enabled ? root.t.interactiveActive : parent.hovered ? root.t.surfaceHover : "transparent"
                }
                onClicked: agentHost.toggleSkill(modelData.path, !modelData.enabled)
                KetToolTip {
                    objectName: "ketToolTip"
                    visible: skillOption.hovered
                    text: modelData.description || modelData.displayPath
                }
            }
            ScrollBar.vertical: Basic.ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }
    }
}
