import Ket
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket as Kit

// The composer's Mode menu, drawn from ket::menuRecipe and ket::menuItemRecipe like the
// mock's KetMenu. It stays a popup of its own so a full-access row can carry its Enable chip.
Basic.Popup {
    id: permissionMenu
    required property var workspace
    required property var anchorItem
    readonly property var root: workspace
    readonly property var recipe: KetTheme.menuRecipe(KetTheme.dark, KetTheme.zoom)
    objectName: "permissionMenu"
    focus: true
    onOpened: permissionItems.forceActiveFocus()
    y: -height - 4
    x: Math.min(0, root.width - anchorItem.mapToItem(root, 0, 0).x - width - 12)
    width: Math.min(KetTheme.px(340, KetTheme.zoom), root.width - KetTheme.px(24, KetTheme.zoom))
    height: permissionItems.implicitHeight + 2 * padding
    padding: recipe.padding
    background: Item {
        KetShadow {
            anchors.fill: parent
            radius: permissionMenu.recipe.radius
            shadowX: permissionMenu.recipe.shadowX
            shadowY: permissionMenu.recipe.shadowY
            shadowBlur: permissionMenu.recipe.shadowBlur
            shadowColor: permissionMenu.recipe.shadowColor
        }
        Rectangle {
            anchors.fill: parent
            radius: permissionMenu.recipe.radius
            color: permissionMenu.recipe.background
            border.width: 1
            border.color: permissionMenu.recipe.border
        }
    }
    Column {
        id: permissionItems
        width: parent.width
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
            readonly property var item: KetTheme.menuItemRecipe(KetTheme.dark, KetTheme.zoom, false, true, 0)
            width: parent.width
            height: permissionMenu.recipe.sectionHeight
            leftPadding: item.paddingStart
            rightPadding: item.paddingEnd
            verticalAlignment: Text.AlignVCenter
            text: "Mode"
            color: permissionMenu.recipe.sectionText
            font.family: KetTheme.fontFamily
            font.pixelSize: permissionMenu.recipe.sectionPixels
            Accessible.role: Accessible.StaticText
        }
        Repeater {
            model: root.permissionProfiles
            Basic.ItemDelegate {
                id: permissionOption
                required property var modelData
                required property int index
                readonly property bool chosen: root.permission === modelData.id
                readonly property bool elevated: modelData.elevated === true
                readonly property var recipe: KetTheme.menuItemRecipe(KetTheme.dark, KetTheme.zoom, hovered || visualFocus, enabled, elevated ? 2 : 0)
                objectName: "permissionOption_" + modelData.id
                width: permissionItems.width
                implicitHeight: Math.max(recipe.height, optionRow.implicitHeight + 2 * recipe.paddingStart)
                leftPadding: recipe.paddingStart
                rightPadding: recipe.paddingEnd
                topPadding: 0
                bottomPadding: 0
                hoverEnabled: true
                Accessible.role: Accessible.MenuItem
                Accessible.name: modelData.label
                Accessible.description: modelData.description || ""
                Accessible.checkable: true
                Accessible.checked: chosen
                contentItem: Item {
                    implicitHeight: optionRow.implicitHeight
                    Row {
                        id: optionRow
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - trailing.width - 4 * spacing
                        spacing: permissionOption.recipe.spacing
                        Item {
                            width: permissionOption.recipe.checkSize
                            height: title.implicitHeight
                            KetIcon {
                                objectName: "permissionCheck"
                                anchors.centerIn: parent
                                visible: permissionOption.chosen
                                name: "check"
                                box: permissionOption.recipe.iconSize
                                color: permissionOption.recipe.text
                            }
                        }
                        KetIcon {
                            anchors.top: parent.top
                            anchors.topMargin: Math.round((title.implicitHeight - box) / 2)
                            name: KetTheme.iconForLucide((root.permissionIconSource(permissionOption.modelData.id)).toString().split("/").pop().replace(/\.svg$/, ""))
                            box: permissionOption.recipe.iconSize
                            color: permissionOption.recipe.text
                        }
                        Column {
                            width: Math.max(0, optionRow.width - 2 * optionRow.spacing - permissionOption.recipe.checkSize - permissionOption.recipe.iconSize)
                            spacing: permissionOption.recipe.descriptionGap
                            Text {
                                id: title
                                width: parent.width
                                text: permissionOption.modelData.label
                                elide: Text.ElideRight
                                color: permissionOption.recipe.text
                                font.family: KetTheme.fontFamily
                                font.pixelSize: permissionOption.recipe.fontPixels
                            }
                            Text {
                                width: parent.width
                                visible: text !== ""
                                text: permissionOption.modelData.description || ""
                                wrapMode: Text.Wrap
                                lineHeight: 1.5
                                color: permissionOption.recipe.description
                                font.family: KetTheme.fontFamily
                                font.pixelSize: permissionOption.recipe.descriptionPixels
                            }
                        }
                    }
                    // Beside the title line, like a KetMenuItem shortcut; Enable marks an elevated mode not in use.
                    Row {
                        id: trailing
                        anchors.right: parent.right
                        anchors.top: optionRow.top
                        height: title.implicitHeight
                        spacing: permissionOption.recipe.spacing
                        Rectangle {
                            visible: permissionOption.elevated && !permissionOption.chosen
                            anchors.verticalCenter: parent.verticalCenter
                            width: enableText.implicitWidth + KetTheme.px(12, KetTheme.zoom)
                            height: enableText.implicitHeight + KetTheme.px(4, KetTheme.zoom)
                            radius: KetTheme.px(5, KetTheme.zoom)
                            color: "transparent"
                            border.width: 1
                            border.color: permissionOption.recipe.text
                            Text {
                                id: enableText
                                anchors.centerIn: parent
                                text: "Enable"
                                color: permissionOption.recipe.text
                                font.family: KetTheme.fontFamily
                                font.pixelSize: permissionOption.recipe.descriptionPixels
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: String(permissionOption.index + 1)
                            color: KetTheme.menuItemRecipe(KetTheme.dark, KetTheme.zoom, permissionOption.hovered || permissionOption.visualFocus, permissionOption.enabled, 0).description
                            font.family: KetTheme.fontFamily
                            font.pixelSize: permissionOption.recipe.fontPixels
                        }
                    }
                }
                background: Rectangle {
                    radius: permissionOption.recipe.radius
                    color: permissionOption.recipe.background
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
