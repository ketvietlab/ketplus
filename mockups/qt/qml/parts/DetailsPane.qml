// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// The right detail pane of Agent Workspace. It owns its top tab row: the conversation's
// changes, its Atlases, its terminal and its work report.
Rectangle {
    id: pane

    required property var window
    property string tab: "changes"
    readonly property var tabs: ["changes", "atlas", "terminal", "report"]

    signal tabChosen(string tab)
    signal closeRequested()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    color: KetTheme.t.panelBg

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: pane.px(44)

            KetSegmentedControl {
                x: pane.px(12)
                anchors.verticalCenter: parent.verticalCenter
                options: ["Changes", "Atlas", "Terminal", "Report"]
                currentIndex: pane.tabs.indexOf(pane.tab)
                onActivated: index => pane.tabChosen(pane.tabs[index])
            }
            KetIconButton {
                anchors.right: parent.right
                anchors.rightMargin: pane.px(8)
                anchors.verticalCenter: parent.verticalCenter
                iconName: "panel-right-close"
                text: "Close pane"
                onClicked: pane.closeRequested()
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
        }

        Basic.ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            visible: pane.tab !== "terminal"

            Column {
                x: pane.px(16)
                y: pane.px(16)
                width: scroll.availableWidth - 2 * x
                spacing: pane.px(14)

                // The whole ledger: every item reported in this conversation, by group.
                TurnReportCard {
                    visible: pane.tab === "report"
                    width: parent.width
                    report: Fixtures.turnReport
                    ledger: true
                }
                KetChangedFiles {
                    visible: pane.tab === "changes"
                    width: parent.width
                    files: Fixtures.changedFiles
                    note: "develop · not committed"
                    footer: KetButton {
                        text: "Open Source Control"
                        controlSize: KetTheme.ControlSize.Small
                        onClicked: pane.window.showScreen("changes")
                    }
                }
                KetDiffView {
                    id: diff
                    visible: pane.tab === "changes"
                    width: parent.width
                    lines: parseUnified(Fixtures.sampleDiff)
                }

                Repeater {
                    model: pane.tab === "atlas" ? [
                        { title: "Onboarding experience", detail: "2 flows · 4 screens · validated" },
                        { title: "Repository picker", detail: "1 flow · 2 screens · draft" }
                    ] : []
                    Rectangle {
                        required property var modelData
                        width: parent.width
                        height: atlasColumn.implicitHeight + pane.px(28)
                        radius: pane.px(7)
                        color: KetTheme.t.pageBg
                        border.color: KetTheme.t.panelBorder
                        Column {
                            id: atlasColumn
                            x: pane.px(14)
                            y: pane.px(14)
                            spacing: pane.px(8)
                            Row {
                                spacing: pane.px(8)
                                KetIcon { name: "layers"; box: pane.px(16); color: KetTheme.t.accentText }
                                Text { text: modelData.title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: pane.px(14); font.weight: Font.Medium }
                            }
                            Text { text: modelData.detail; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: pane.px(12) }
                            Row {
                                spacing: pane.px(6)
                                KetButton { text: "Edit"; controlSize: KetTheme.ControlSize.Small; iconName: "edit"; onClicked: pane.window.showScreen("atlas") }
                                KetButton { text: "Open in browser"; controlSize: KetTheme.ControlSize.Small; variant: KetTheme.ButtonVariant.Ghost; iconName: "open"; onClicked: pane.window.notify("Opened in the browser") }
                            }
                        }
                    }
                }
            }
        }

        // A terminal in the conversation's worktree.
        Rectangle {
            visible: pane.tab === "terminal"
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: KetTheme.t.pageBg
            Text {
                x: pane.px(14)
                y: pane.px(12)
                width: parent.width - 2 * x
                wrapMode: Text.WrapAnywhere
                color: KetTheme.t.textMain
                font.family: KetTheme.monoFontFamily
                font.pixelSize: pane.px(14)
                lineHeight: 1.35
                text: "➜ ketplus-desktop git:(develop) ✗ cmake --build build\n"
                      + "[214/214] Linking CXX executable KetPlus.app\n"
                      + "➜ ketplus-desktop git:(develop) ✗ ctest --test-dir build -R startup\n"
                      + "100% tests passed, 0 tests failed out of 6\n"
                      + "➜ ketplus-desktop git:(develop) ✗ ▍"
            }
        }
    }
}
