import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket as Kit

Basic.Popup {
    id: permissionMenu
    required property var workspace
    required property var anchorItem
    readonly property var root: workspace
    objectName: "permissionMenu"
    focus: true
    onOpened: permissionItems.forceActiveFocus()
    y: -height - 4
    x: Math.min(0, root.width - anchorItem.mapToItem(root, 0, 0).x - width - 12)
    width: Math.min(Metrics.px(320, root.t), root.width - Metrics.px(24, root.t))
    height: permissionItems.implicitHeight + Metrics.px(16, root.t)
    padding: Metrics.px(8, root.t)
    background: Rectangle {
        color: root.t.surfaceRaised
        radius: Metrics.px(12, root.t)
        border.width: 1
        border.color: root.t.borderStrong
    }
    Column {
        id: permissionItems
        width: parent.width
        spacing: Metrics.px(2, root.t)
        focus: true
        Keys.onPressed: function (event) {
            const choice = event.key - Qt.Key_1;
            if (choice >= 0 && choice < root.permissionProfiles.length) {
                root.permission = root.permissionProfiles[choice].id;
                root.publishConversationConfiguration();
                permissionMenu.close();
                event.accepted = true;
            }
        }
        Text {
            width: parent.width
            leftPadding: Metrics.px(8, root.t)
            topPadding: Metrics.px(2, root.t)
            bottomPadding: Metrics.px(6, root.t)
            text: "Mode"
            color: root.t.textMuted
            font.family: root.t.uiFont
            font.pixelSize: root.t.micro
        }
        Repeater {
            model: root.permissionProfiles
            Basic.ItemDelegate {
                id: permissionOption
                required property var modelData
                required property int index
                width: permissionItems.width
                height: Metrics.px(52, root.t)
                leftPadding: Metrics.px(8, root.t)
                rightPadding: Metrics.px(8, root.t)
                contentItem: RowLayout {
                    spacing: Metrics.px(8, root.t)
                    TintedIcon {
                        Layout.preferredWidth: Metrics.px(14, root.t)
                        Layout.preferredHeight: Metrics.px(14, root.t)
                        Layout.alignment: Qt.AlignVCenter
                        source: root.permissionIconSource(modelData.id)
                        color: root.permissionLabelColor(modelData)
                    }
                    Column {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Metrics.px(3, root.t)
                        Text {
                            width: parent.width
                            text: modelData.label
                            color: root.permissionLabelColor(modelData)
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.body
                        }
                        Text {
                            width: parent.width
                            text: modelData.description || ""
                            elide: Text.ElideRight
                            color: modelData.elevated === true ? (root.t.warning || "#d9a441") : root.t.textMuted
                            opacity: modelData.elevated === true ? 0.85 : 1
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.micro
                        }
                    }
                    Rectangle {
                        visible: modelData.elevated === true && root.permission !== modelData.id
                        Layout.preferredWidth: enableText.implicitWidth + Metrics.px(12, root.t)
                        Layout.preferredHeight: Metrics.px(18, root.t)
                        Layout.alignment: Qt.AlignVCenter
                        radius: Metrics.px(5, root.t)
                        color: "transparent"
                        border.width: 1
                        border.color: root.t.warning || "#d9a441"
                        Text {
                            id: enableText
                            anchors.centerIn: parent
                            text: "Enable"
                            color: root.t.warning || "#d9a441"
                            font.family: root.t.uiFont
                            font.pixelSize: root.t.micro
                        }
                    }
                    Text {
                        text: root.permission === modelData.id ? "✓" : ""
                        color: root.permissionLabelColor(modelData)
                        font.pixelSize: root.t.caption
                        Layout.preferredWidth: Metrics.px(12, root.t)
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        text: String(index + 1)
                        color: root.t.textMuted
                        font.family: root.t.uiFont
                        font.pixelSize: root.t.micro
                        Layout.preferredWidth: Metrics.px(10, root.t)
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                    }
                }
                background: Rectangle {
                    radius: Metrics.px(6, root.t)
                    color: parent.hovered ? root.t.interactiveActive : "transparent"
                }
                onClicked: {
                    root.permission = modelData.id;
                    root.publishConversationConfiguration();
                    permissionMenu.close();
                }
            }
        }
    }
}
