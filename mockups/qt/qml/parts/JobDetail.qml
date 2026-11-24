// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// One managed job opened from the task bar or the turn's Show job (ADR 0127 §7a): what it
// runs, where it stands, who it wakes, its latest output and what the user can do. The app
// opens the same output under its task bar (BackgroundTaskBar.qml); the kit's task row has
// no open action yet, so the mock opens this on a tap on the row. `job` is a
// Fixtures.jobStates entry.
Rectangle {
    id: detail

    required property var window
    property var job: null
    readonly property var info: job ? job.detail : ({})
    readonly property var code: KetTheme.codeBlockRecipe(KetTheme.dark, KetTheme.zoom)

    signal closeRequested()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: content.implicitHeight + px(20)
    radius: px(8)
    color: KetTheme.t.panelBgSubtle
    border.color: KetTheme.t.panelBorder
    Accessible.role: Accessible.Grouping
    Accessible.name: "Background task " + Fixtures.jobTitle

    Column {
        id: content
        x: detail.px(12)
        y: detail.px(10)
        width: parent.width - 2 * x
        spacing: detail.px(8)

        // The job and its state, as the task bar's row words it.
        Item {
            width: parent.width
            height: detail.px(26)
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: detail.px(8)
                KetIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "terminal"
                    box: detail.px(14)
                    color: KetTheme.t.textSecondary
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Fixtures.jobTitle
                    color: KetTheme.t.textMain
                    font.family: KetTheme.fontFamily
                    font.pixelSize: detail.px(13)
                    font.weight: Font.DemiBold
                }
                KetBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    text: detail.info.state || ""
                    tone: detail.info.tone !== undefined ? detail.info.tone : KetTheme.BadgeTone.Neutral
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: detail.info.id || ""
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.monoFontFamily
                    font.pixelSize: detail.px(11)
                }
            }
            KetIconButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconName: "close"
                text: "Close task details"
                onClicked: detail.closeRequested()
            }
        }

        // What only the user can do: unverified cleanup.
        KetBanner {
            visible: !!detail.info.banner
            width: parent.width
            tone: KetTheme.BannerTone.Warning
            title: detail.info.banner ? detail.info.banner.title : ""
            message: detail.info.banner ? detail.info.banner.message : ""
        }

        // One fact a line: a muted name, then its value.
        Column {
            width: parent.width
            spacing: detail.px(4)
            Repeater {
                model: detail.info.facts || []
                Row {
                    required property var modelData
                    width: parent.width
                    spacing: detail.px(8)
                    Text {
                        width: detail.px(92)
                        text: modelData[0]
                        color: KetTheme.t.textMuted
                        font.family: KetTheme.fontFamily
                        font.pixelSize: detail.px(12)
                    }
                    Text {
                        width: parent.width - detail.px(100)
                        text: modelData[1]
                        wrapMode: Text.Wrap
                        color: KetTheme.t.textSecondary
                        font.family: KetTheme.fontFamily
                        font.pixelSize: detail.px(12)
                    }
                }
            }
        }

        // The command and its latest output, as the app's output pane shows them.
        Rectangle {
            width: parent.width
            height: output.implicitHeight + detail.px(16)
            radius: detail.px(6)
            color: detail.code.background
            border.color: detail.code.border
            Column {
                id: output
                x: detail.px(10)
                y: detail.px(8)
                width: parent.width - 2 * x
                spacing: detail.px(6)
                Text {
                    width: parent.width
                    text: "$ " + (detail.info.command || "")
                    wrapMode: Text.WrapAnywhere
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.monoFontFamily
                    font.pixelSize: detail.px(11)
                }
                Basic.ScrollView {
                    id: outputScroll
                    width: parent.width
                    height: Math.min(outputText.implicitHeight, detail.px(120))
                    clip: true
                    TextEdit {
                        id: outputText
                        width: outputScroll.availableWidth
                        readOnly: true
                        selectByMouse: true
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.WrapAnywhere
                        text: detail.info.output || "No output yet."
                        color: detail.info.output ? KetTheme.t.textMain : KetTheme.t.textMuted
                        font.family: KetTheme.monoFontFamily
                        font.pixelSize: detail.px(11)
                    }
                }
            }
        }

        // How fresh the output is, and the actions this state allows.
        Item {
            width: parent.width
            height: actions.height
            Text {
                anchors.left: parent.left
                anchors.right: actions.left
                anchors.rightMargin: detail.px(8)
                anchors.verticalCenter: parent.verticalCenter
                text: detail.info.footer || ""
                elide: Text.ElideRight
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: detail.px(11)
            }
            Row {
                id: actions
                anchors.right: parent.right
                spacing: detail.px(6)
                Repeater {
                    model: detail.info.actions || []
                    KetButton {
                        required property var modelData
                        required property int index
                        text: modelData
                        controlSize: KetTheme.ControlSize.Small
                        variant: index === 0 && detail.info.banner ? KetTheme.ButtonVariant.Secondary
                                                                   : KetTheme.ButtonVariant.Ghost
                        onClicked: detail.window.notify(modelData + " · " + Fixtures.jobTitle)
                    }
                }
            }
        }
    }
}
