// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// The same picker on iPhone (KetPlus App), shown in a 390 × 844 device frame for
// cross-platform review. The desktop sidebar is hidden on this screen.
Rectangle {
    id: screen

    required property var window
    property int repository: 0

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    color: KetTheme.t.pageBg

    KetButton {
        x: screen.px(20); y: screen.px(16)
        text: "Back to desktop"
        iconName: "arrow-left"
        variant: KetTheme.ButtonVariant.Ghost
        onClicked: screen.window.showScreen("workspace")
    }

    Rectangle {
        id: phone
        anchors.centerIn: parent
        width: screen.px(390) + screen.px(20)
        height: Math.min(parent.height - screen.px(40), screen.px(844) + screen.px(20))
        radius: screen.px(54)
        color: KetTheme.t.textMain
        Rectangle {
            anchors.fill: parent
            anchors.margins: screen.px(10)
            radius: screen.px(46)
            color: KetTheme.t.panelBgSubtle
            clip: true

            Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: screen.px(12); width: screen.px(120); height: screen.px(34); radius: height / 2; color: KetTheme.t.textMain }

            Text { x: screen.px(28); y: screen.px(70); text: "Review the startup flow"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(17); font.weight: Font.DemiBold }
            Text { x: screen.px(28); y: screen.px(96); text: "@agent/9410 · Working"; color: KetTheme.t.textMuted; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(14) }

            // The bottom sheet.
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: parent.height * 0.72
                radius: screen.px(22)
                color: KetTheme.t.panelBg
                border.color: KetTheme.t.panelBorder
                Column {
                    x: screen.px(18); y: screen.px(10)
                    width: parent.width - 2 * x
                    spacing: screen.px(10)
                    Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: screen.px(36); height: screen.px(5); radius: height / 2; color: KetTheme.t.borderStrong }
                    Text { text: "Repository & worktree"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(17); font.weight: Font.DemiBold }
                    KetTextField { width: parent.width; leadingIcon: "search"; placeholderText: "Search repositories or branches" }
                    Text { text: "REPOSITORIES"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(11); font.letterSpacing: 1 }
                    Repeater {
                        model: Fixtures.pickerRepositories.slice(0, 4)
                        KetTreeItem {
                            required property var modelData
                            required property int index
                            width: parent.width
                            text: modelData.name
                            iconName: "folder"
                            count: String(modelData.count)
                            checked: screen.repository === index
                            onClicked: screen.repository = index
                        }
                    }
                    Text { text: "WORKTREES"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(11); font.letterSpacing: 1 }
                    Repeater {
                        model: Fixtures.pickerRepositories[screen.repository].worktrees
                        KetTreeItem {
                            required property var modelData
                            width: parent.width
                            text: modelData.branch
                            detail: modelData.path
                            iconName: "branch"
                        }
                    }
                    KetButton { width: parent.width; text: "Use for the next turn"; variant: KetTheme.ButtonVariant.Primary; onClicked: screen.window.notify("Applies from the next turn") }
                }
            }
        }
    }
}
