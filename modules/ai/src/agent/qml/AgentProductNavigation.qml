import "ChromeMetrics.js" as Metrics
import QtQuick
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

    Basic.Button {
        id: quickSearch
        objectName: "productQuickSearch"
        Layout.fillWidth: true
        Layout.preferredHeight: Metrics.px(32, workspace.t)
        Layout.leftMargin: Metrics.px(9, workspace.t)
        Layout.rightMargin: Metrics.px(9, workspace.t)
        Layout.bottomMargin: navigation.compactHeight ? Metrics.px(10, workspace.t) : Metrics.px(19, workspace.t)
        padding: 0
        onClicked: agentHost.requestProductNavigation("search")
        contentItem: RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Metrics.px(9, workspace.t)
            anchors.rightMargin: Metrics.px(9, workspace.t)
            spacing: Metrics.px(8, workspace.t)
            TintedIcon {
                Layout.preferredWidth: Metrics.px(14, workspace.t)
                Layout.preferredHeight: Metrics.px(14, workspace.t)
                source: "qrc:/agent/icons/search.svg"
                color: navigation.workspace.t.textMuted
            }
            Text {
                Layout.fillWidth: true
                text: "Quick search"
                color: navigation.workspace.t.textSecondary
                font.family: navigation.workspace.t.uiFont
                font.pixelSize: navigation.workspace.t.body
            }
            Text {
                text: "⌘ K"
                color: navigation.workspace.t.textMuted
                font.family: navigation.workspace.t.uiFont
                font.pixelSize: navigation.workspace.t.micro
            }
        }
        background: Rectangle {
            radius: Metrics.px(5, workspace.t)
            color: quickSearch.hovered ? navigation.workspace.t.surfaceHover
                                       : navigation.workspace.t.panelBackground
                                         || navigation.workspace.t.pageBackground
            border.color: quickSearch.visualFocus ? navigation.workspace.t.focus
                                                   : navigation.workspace.t.border
        }
        Accessible.name: "Quick search"
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
            delegate: Basic.Button {
                id: navigationButton
                required property var modelData
                objectName: "productNavigation_" + modelData.id
                width: parent.width
                height: Metrics.px(34, workspace.t)
                padding: 0
                checked: modelData.id === "workspace"
                onClicked: agentHost.requestProductNavigation(modelData.id)
                contentItem: RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Metrics.px(9, workspace.t)
                    anchors.rightMargin: Metrics.px(9, workspace.t)
                    spacing: Metrics.px(9, workspace.t)
                    TintedIcon {
                        Layout.preferredWidth: Metrics.px(16, workspace.t)
                        Layout.preferredHeight: Metrics.px(16, workspace.t)
                        source: "qrc:/agent/icons/" + navigationButton.modelData.icon
                        color: navigationButton.checked ? navigation.workspace.t.accent
                                                        : navigation.workspace.t.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: navigationButton.modelData.label
                        color: navigationButton.checked ? navigation.workspace.t.accent
                                                        : navigation.workspace.t.textSecondary
                        elide: Text.ElideRight
                        font.family: navigation.workspace.t.uiFont
                        font.pixelSize: navigation.workspace.t.body
                        font.weight: navigationButton.checked ? Font.DemiBold : Font.Medium
                    }
                }
                background: Rectangle {
                    radius: Metrics.px(5, workspace.t)
                    color: navigationButton.checked ? navigation.workspace.t.accentSubtle
                           : navigationButton.hovered ? navigation.workspace.t.surfaceHover
                                                      : "transparent"
                    border.color: navigationButton.visualFocus ? navigation.workspace.t.focus
                                                                : "transparent"
                }
                Accessible.name: modelData.label
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
