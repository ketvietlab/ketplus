import Ket
import QtQuick
import Ket as Kit
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

ColumnLayout {
    id: navigation
    objectName: "agentProductNavigation"
    required property var workspace
    required property bool compactHeight
    spacing: 0
    implicitHeight: compactHeight ? KetTheme.px(259, KetTheme.zoom) : KetTheme.px(293, KetTheme.zoom)
    Layout.minimumHeight: implicitHeight

    Item {
        id: brand
        objectName: "productBrand"
        Layout.fillWidth: true
        Layout.preferredHeight: navigation.compactHeight ? KetTheme.px(54, KetTheme.zoom) : KetTheme.px(67, KetTheme.zoom)

        Image {
            objectName: "productBrandImage"
            anchors.left: parent.left
            anchors.leftMargin: KetTheme.px(13, KetTheme.zoom)
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(KetTheme.px(176, KetTheme.zoom),
                            Math.max(0, parent.width - 2 * anchors.leftMargin))
            height: KetTheme.px(44, KetTheme.zoom)
            source: KetTheme.dark
                    ? "qrc:/agent/brand/ketplus-logo-dark.png"
                    : "qrc:/agent/brand/ketplus-logo-light.png"
            fillMode: Image.PreserveAspectFit
            horizontalAlignment: Image.AlignLeft
            smooth: true
            mipmap: true
            Accessible.name: "KétPlus Pro"
        }
    }

    Kit.KetNavItem {
        id: quickSearch
        objectName: "productQuickSearch"
        Layout.fillWidth: true
        Layout.leftMargin: KetTheme.px(9, KetTheme.zoom)
        Layout.rightMargin: KetTheme.px(9, KetTheme.zoom)
        Layout.bottomMargin: KetTheme.px(10, KetTheme.zoom)
        text: "Quick search"
        iconName: Kit.KetTheme.iconForLucide("search")
        shortcut: "⌘ K"
        checkable: false
        onClicked: agentHost.requestProductNavigation("search")
    }

    Column {
        id: productNavigation
        objectName: "productNavigation"
        Layout.fillWidth: true
        Layout.leftMargin: KetTheme.px(8, KetTheme.zoom)
        Layout.rightMargin: KetTheme.px(8, KetTheme.zoom)
        spacing: KetTheme.px(2, KetTheme.zoom)

        Repeater {
            objectName: "productNavigationRepeater"
            model: [
                { id: "workspace", label: "Agent Workspace", icon: "messages-square.svg" },
                { id: "explorer", label: "Files & Editor", icon: "folder.svg" },
                { id: "source-control", label: "Source Control", icon: "git-branch.svg" },
                { id: "connections", label: "Connections", icon: "unplug.svg" }
            ]
            delegate: Kit.KetNavItem {
                required property var modelData
                objectName: "productNavigation_" + modelData.id
                width: parent.width
                text: modelData.label
                iconName: Kit.KetTheme.iconForLucide(modelData.icon.replace(".svg", ""))
                checked: modelData.id === "workspace"
                onClicked: agentHost.requestProductNavigation(modelData.id)
            }
        }
    }

    Rectangle {
        id: divider
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        Layout.leftMargin: KetTheme.px(11, KetTheme.zoom)
        Layout.rightMargin: KetTheme.px(11, KetTheme.zoom)
        Layout.topMargin: navigation.compactHeight ? KetTheme.px(10, KetTheme.zoom) : KetTheme.px(18, KetTheme.zoom)
        Layout.bottomMargin: navigation.compactHeight ? KetTheme.px(10, KetTheme.zoom) : KetTheme.px(14, KetTheme.zoom)
        color: KetTheme.t.panelBorder
    }
}
