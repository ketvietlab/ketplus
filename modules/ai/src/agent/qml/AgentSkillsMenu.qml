import "ChromeMetrics.js" as Metrics
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
    width: Math.min(Metrics.px(420, root.t), root.width - Metrics.px(24, root.t))
    height: Math.min(root.height - Metrics.px(32, root.t), skillsContent.implicitHeight + Metrics.px(16, root.t))
    padding: Metrics.px(8, root.t)
    background: Rectangle {
        color: root.t.surfaceRaised
        radius: Metrics.px(12, root.t)
        border.color: root.t.borderStrong
    }
    ColumnLayout {
        id: skillsContent
        width: parent.width
        spacing: Metrics.px(6, root.t)
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
                height: Metrics.px(62, root.t)
                padding: Metrics.px(8, root.t)
                contentItem: RowLayout {
                    spacing: Metrics.px(10, root.t)
                    Rectangle {
                        Layout.preferredWidth: Metrics.px(18, root.t)
                        Layout.preferredHeight: Metrics.px(18, root.t)
                        radius: Metrics.px(5, root.t)
                        color: modelData.enabled ? root.t.accent : "transparent"
                        border.width: 1
                        border.color: modelData.enabled ? root.t.accentHover : root.t.borderStrong
                        Text {
                            anchors.centerIn: parent
                            text: modelData.enabled ? "✓" : ""
                            color: "#ffffff"
                            font.pixelSize: root.t.body
                        }
                    }
                    Column {
                        Layout.fillWidth: true
                        spacing: Metrics.px(3, root.t)
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
                    radius: Metrics.px(8, root.t)
                    color: modelData.enabled ? root.t.interactiveActive : parent.hovered ? root.t.surfaceHover : "transparent"
                }
                onClicked: agentHost.toggleSkill(modelData.path, !modelData.enabled)
                KetToolTip {
                    tokens: root.t
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
