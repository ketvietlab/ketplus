import "ChromeMetrics.js" as Metrics
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
    implicitHeight: compactHeight ? Metrics.px(259, workspace.t) : Metrics.px(293, workspace.t)
    Layout.minimumHeight: implicitHeight

    Item {
        id: brand
        objectName: "productBrand"
        Layout.fillWidth: true
        Layout.preferredHeight: navigation.compactHeight ? Metrics.px(54, workspace.t) : Metrics.px(67, workspace.t)

        Image {
            anchors.left: parent.left
            anchors.leftMargin: Metrics.px(13, workspace.t)
            anchors.verticalCenter: parent.verticalCenter
            width: Metrics.px(176, workspace.t)
            height: Metrics.px(44, workspace.t)
            source: navigation.workspace.t.dark
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
        Layout.leftMargin: Metrics.px(9, workspace.t)
        Layout.rightMargin: Metrics.px(9, workspace.t)
        Layout.bottomMargin: Metrics.px(10, workspace.t)
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
        Layout.leftMargin: Metrics.px(8, workspace.t)
        Layout.rightMargin: Metrics.px(8, workspace.t)
        spacing: Metrics.px(2, workspace.t)

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
        Layout.leftMargin: Metrics.px(11, workspace.t)
        Layout.rightMargin: Metrics.px(11, workspace.t)
        Layout.topMargin: navigation.compactHeight ? Metrics.px(10, workspace.t) : Metrics.px(18, workspace.t)
        Layout.bottomMargin: navigation.compactHeight ? Metrics.px(10, workspace.t) : Metrics.px(14, workspace.t)
        color: navigation.workspace.t.border
    }
}
