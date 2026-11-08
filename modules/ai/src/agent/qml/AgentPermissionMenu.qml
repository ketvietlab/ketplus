import Ket
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
    width: Math.min(KetTheme.px(320, KetTheme.zoom), root.width - KetTheme.px(24, KetTheme.zoom))
    height: permissionItems.implicitHeight + KetTheme.px(16, KetTheme.zoom)
    padding: KetTheme.px(8, KetTheme.zoom)
    background: Rectangle {
        color: KetTheme.t.surfaceRaised
        radius: KetTheme.px(12, KetTheme.zoom)
        border.width: 1
        border.color: KetTheme.t.borderStrong
    }
    Column {
        id: permissionItems
        width: parent.width
        spacing: KetTheme.px(2, KetTheme.zoom)
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
            leftPadding: KetTheme.px(8, KetTheme.zoom)
            topPadding: KetTheme.px(2, KetTheme.zoom)
            bottomPadding: KetTheme.px(6, KetTheme.zoom)
            text: "Mode"
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
        }
        Repeater {
            model: root.permissionProfiles
            Basic.ItemDelegate {
                id: permissionOption
                required property var modelData
                required property int index
                width: permissionItems.width
                height: KetTheme.px(52, KetTheme.zoom)
                leftPadding: KetTheme.px(8, KetTheme.zoom)
                rightPadding: KetTheme.px(8, KetTheme.zoom)
                contentItem: RowLayout {
                    spacing: KetTheme.px(8, KetTheme.zoom)
                    KetIcon {
                        Layout.preferredWidth: KetTheme.px(14, KetTheme.zoom)
                        Layout.preferredHeight: KetTheme.px(14, KetTheme.zoom)
                        Layout.alignment: Qt.AlignVCenter
                        name: KetTheme.iconForLucide((root.permissionIconSource(modelData.id)).toString().split("/").pop().replace(/\.svg$/, ""))
                        color: root.permissionLabelColor(modelData)
                        box: KetTheme.px(14, KetTheme.zoom)
                    }
                    Column {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: KetTheme.px(3, KetTheme.zoom)
                        Text {
                            width: parent.width
                            text: modelData.label
                            color: root.permissionLabelColor(modelData)
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                        }
                        Text {
                            width: parent.width
                            text: modelData.description || ""
                            elide: Text.ElideRight
                            color: modelData.elevated === true ? (KetTheme.t.warning) : KetTheme.t.textMuted
                            opacity: modelData.elevated === true ? 0.85 : 1
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                        }
                    }
                    Rectangle {
                        visible: modelData.elevated === true && root.permission !== modelData.id
                        Layout.preferredWidth: enableText.implicitWidth + KetTheme.px(12, KetTheme.zoom)
                        Layout.preferredHeight: KetTheme.px(18, KetTheme.zoom)
                        Layout.alignment: Qt.AlignVCenter
                        radius: KetTheme.px(5, KetTheme.zoom)
                        color: "transparent"
                        border.width: 1
                        border.color: KetTheme.t.warning
                        Text {
                            id: enableText
                            anchors.centerIn: parent
                            text: "Enable"
                            color: KetTheme.t.warning
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                        }
                    }
                    Text {
                        text: root.permission === modelData.id ? "✓" : ""
                        color: root.permissionLabelColor(modelData)
                        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
                        Layout.preferredWidth: KetTheme.px(12, KetTheme.zoom)
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        text: String(index + 1)
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                        Layout.preferredWidth: KetTheme.px(10, KetTheme.zoom)
                        Layout.alignment: Qt.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                    }
                }
                background: Rectangle {
                    radius: KetTheme.px(6, KetTheme.zoom)
                    color: parent.hovered ? KetTheme.t.interactiveActive : "transparent"
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
