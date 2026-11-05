// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Source Control: the repository's changes and staged changes, and the selected file's diff.
// Git commands stay in the terminal.
Item {
    id: screen

    required property var window
    property string selected: "NavigationRail.qml"
    property int mode: 0 // 0 inline, 1 side by side (shown inline in the mock)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    component Section: Item {
        property string title
        property int count
        property bool open: true
        width: parent.width
        height: screen.px(34)
        Basic.AbstractButton {
            anchors.fill: parent
            onClicked: parent.open = !parent.open
            background: null
            contentItem: Item {
                KetIcon { x: screen.px(12); anchors.verticalCenter: parent.verticalCenter; name: open ? "chevron-down" : "chevron-right"; box: screen.px(12); color: KetTheme.t.textMuted }
                Text { x: screen.px(30); anchors.verticalCenter: parent.verticalCenter; text: title; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                KetBadge { anchors.right: parent.right; anchors.rightMargin: screen.px(12); anchors.verticalCenter: parent.verticalCenter; text: count }
            }
        }
    }

    component ChangeRow: Basic.AbstractButton {
        required property var file
        width: parent.width
        height: screen.px(56)
        hoverEnabled: true
        checked: screen.selected === file.name
        onClicked: screen.selected = file.name
        background: Rectangle { color: parent.checked ? KetTheme.t.accentSubtle : parent.hovered ? KetTheme.t.surfaceHover : "transparent" }
        contentItem: Item {
            KetIcon { x: screen.px(14); anchors.verticalCenter: parent.verticalCenter; name: "file-code"; box: screen.px(14); color: KetTheme.t.textMuted }
            Column {
                x: screen.px(34)
                anchors.verticalCenter: parent.verticalCenter
                Text { text: file.name; color: checked ? KetTheme.t.accentText : KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14) }
                Text { text: file.path; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(11) }
            }
            Text { anchors.right: parent.right; anchors.rightMargin: screen.px(14); anchors.verticalCenter: parent.verticalCenter; text: "M"; color: KetTheme.t.warning; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(11); font.weight: Font.DemiBold }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: screen.px(224)
            color: KetTheme.t.sidebarBg

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                KetPaneHeader {
                    Layout.fillWidth: true
                    title: "SOURCE CONTROL"
                    KetIconButton { controlSize: KetTheme.ControlSize.Small; iconName: "refresh"; text: "Refresh"; onClicked: screen.window.notify("Status refreshed") }
                }

                Column {
                    Layout.fillWidth: true
                    Layout.margins: screen.px(14)
                    Row {
                        spacing: screen.px(8)
                        KetIcon { name: "folder-open"; box: screen.px(14); color: KetTheme.t.textSecondary; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: screen.window.repository; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                    }
                    Text { leftPadding: screen.px(22); text: "develop"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                }

                Column {
                    Layout.fillWidth: true
                    Section { id: changes; title: "Changes"; count: Fixtures.changedFiles.length }
                    Repeater { model: changes.open ? Fixtures.changedFiles : []; ChangeRow { required property var modelData; file: modelData } }
                    Section { id: staged; title: "Staged Changes"; count: Fixtures.stagedFiles.length }
                    Repeater { model: staged.open ? Fixtures.stagedFiles : []; ChangeRow { required property var modelData; file: modelData } }
                }

                Item { Layout.fillHeight: true }

                Column {
                    Layout.fillWidth: true
                    Layout.margins: screen.px(14)
                    spacing: screen.px(10)
                    Text { text: "Run Git commands in the terminal"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                    KetButton { text: "Open terminal"; iconName: "terminal"; onClicked: screen.window.notify("Terminal opened in develop") }
                }
            }

            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: KetTheme.t.panelBorder }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 0
                KetTabBar {
                    Layout.fillWidth: true
                    KetTabButton { text: screen.selected + "  ·  Working tree"; iconName: "compare"; width: implicitWidth }
                }
                KetButton {
                    text: "Open file"
                    iconName: "arrow-up-right"
                    variant: KetTheme.ButtonVariant.Ghost
                    controlSize: KetTheme.ControlSize.Small
                    onClicked: screen.window.showScreen("editor")
                }
                Item { width: screen.px(8) }
            }

            KetBreadcrumb {
                Layout.fillWidth: true
                path: ["modules", "ai", "src", "agent", "qml"]
                KetSegmentedControl {
                    options: ["Inline", "Side by side"]
                    currentIndex: screen.mode
                    onActivated: index => screen.mode = index
                }
            }

            Basic.ScrollView {
                id: scroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                Column {
                    width: scroll.availableWidth
                    Item {
                        width: parent.width
                        height: screen.px(46)
                        Text { x: screen.px(20); anchors.verticalCenter: parent.verticalCenter; text: "Working tree · develop"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: screen.px(20)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(8)
                            Text { text: "+28"; color: KetTheme.t.success; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(13) }
                            Text { text: "−9"; color: KetTheme.t.danger; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(13) }
                        }
                    }
                    KetDiffView {
                        id: diff
                        width: parent.width
                        lines: parseUnified(Fixtures.sampleDiff)
                    }
                    Item {
                        width: parent.width
                        height: screen.px(48)
                        Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                        Row {
                            x: screen.px(20)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(8)
                            KetIcon { name: "check"; box: screen.px(12); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: "End of changes"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        }
                        KetButton {
                            anchors.right: parent.right
                            anchors.rightMargin: screen.px(12)
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Open full file"
                            iconName: "arrow-up-right"
                            variant: KetTheme.ButtonVariant.Ghost
                            onClicked: screen.window.showScreen("editor")
                        }
                    }
                }
            }
        }
    }
}
