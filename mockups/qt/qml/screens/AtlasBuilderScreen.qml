// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Edit an Atlas in KétPlus: flows on the left, the map of screens, and the AI Chat that
// edits it. Back returns to the conversation that opened it.
Item {
    id: screen

    required property var window
    property int flow: 0
    property real zoom: 1
    property bool chatOpen: true

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    component MiniScreen: Rectangle {
        property string number
        property string title
        property string heading
        property bool start: false
        width: screen.px(260) * screen.zoom
        height: screen.px(378) * screen.zoom
        radius: screen.px(7)
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.panelBorder
        Item {
            width: parent.width; height: screen.px(40)
            Text { x: screen.px(14); anchors.verticalCenter: parent.verticalCenter; text: number + "   <b>" + title + "</b>"; textFormat: Text.StyledText; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
        }
        Column {
            x: screen.px(22) * screen.zoom
            y: screen.px(64) * screen.zoom
            width: parent.width - 2 * x
            spacing: screen.px(10) * screen.zoom
            Text { text: "STEP " + number + " / 02"; color: KetTheme.t.accentText; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(6) * screen.zoom }
            Text { width: parent.width; wrapMode: Text.Wrap; text: heading; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(17) * screen.zoom; font.weight: Font.DemiBold }
            Repeater {
                model: 2
                Rectangle {
                    required property int index
                    width: parent.width; height: screen.px(32) * screen.zoom; radius: screen.px(3)
                    color: index === 0 ? KetTheme.t.accentSubtle : KetTheme.t.panelBg
                    border.color: index === 0 ? KetTheme.t.accentBorder : KetTheme.t.panelBorder
                }
            }
            Rectangle { width: parent.width; height: screen.px(12) * screen.zoom; radius: screen.px(2); color: KetTheme.t.accent }
        }
        Item {
            anchors.bottom: parent.bottom
            width: parent.width; height: screen.px(40)
            Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
            KetBadge { x: screen.px(12); anchors.verticalCenter: parent.verticalCenter; text: start ? "Start" : "End"; tone: start ? KetTheme.BadgeTone.Accent : KetTheme.BadgeTone.Success }
            KetButton { anchors.right: parent.right; anchors.rightMargin: screen.px(6); anchors.verticalCenter: parent.verticalCenter; text: "Try screen"; iconName: "arrow-up-right"; variant: KetTheme.ButtonVariant.Ghost; controlSize: KetTheme.ControlSize.Small; onClicked: screen.window.notify("Opened " + title + " in preview") }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: screen.px(44)
            Row {
                x: screen.px(10)
                anchors.verticalCenter: parent.verticalCenter
                spacing: screen.px(10)
                KetIconButton { iconName: "arrow-left"; text: "Back to the conversation"; onClicked: screen.window.showScreen("workspace/atlas") }
                KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "branch"; box: screen.px(14); color: KetTheme.t.textSecondary }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "Onboarding experience"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.DemiBold }
                    Text { text: Fixtures.atlasFlows[screen.flow].title + " · " + Fixtures.atlasFlows[screen.flow].screens + " screens"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(11) }
                }
            }
            Row {
                anchors.right: parent.right
                anchors.rightMargin: screen.px(12)
                anchors.verticalCenter: parent.verticalCenter
                spacing: screen.px(8)
                KetBadge { anchors.verticalCenter: parent.verticalCenter; text: "Validated preview"; tone: KetTheme.BadgeTone.Success }
                KetButton { text: screen.chatOpen ? "Hide AI Chat" : "Show AI Chat"; iconName: "chat"; controlSize: KetTheme.ControlSize.Small; onClicked: screen.chatOpen = !screen.chatOpen }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: screen.px(204)
                color: KetTheme.t.sidebarBg
                Column {
                    x: screen.px(10); y: screen.px(14)
                    width: parent.width - 2 * x
                    spacing: screen.px(4)
                    Item {
                        width: parent.width; height: screen.px(28)
                        Text { x: screen.px(8); anchors.verticalCenter: parent.verticalCenter; text: "Flows"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                        Text { anchors.right: parent.right; anchors.rightMargin: screen.px(8); anchors.verticalCenter: parent.verticalCenter; text: Fixtures.atlasFlows.length; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                    }
                    Repeater {
                        model: Fixtures.atlasFlows
                        KetTreeItem {
                            required property var modelData
                            required property int index
                            width: parent.width
                            text: modelData.title
                            detail: modelData.screens + " screens"
                            iconName: "branch"
                            checked: screen.flow === index
                            onClicked: screen.flow = index
                        }
                    }
                }
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: KetTheme.t.panelBorder }
            }

            // The map: a dotted canvas with the flow's screens and transitions.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Canvas {
                    anchors.fill: parent
                    property color dot: KetTheme.t.borderDefault
                    onDotChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.fillStyle = dot
                        const step = screen.px(16)
                        for (let x = step; x < width; x += step)
                            for (let y = step; y < height; y += step)
                                ctx.fillRect(x, y, 1, 1)
                    }
                }

                Flickable {
                    anchors.fill: parent
                    contentWidth: map.width + screen.px(160)
                    contentHeight: map.height + screen.px(160)
                    Column {
                        id: map
                        x: screen.px(80); y: screen.px(64)
                        spacing: screen.px(28)
                        Row {
                            spacing: screen.px(12)
                            Rectangle { width: screen.px(32); height: width; radius: screen.px(6); color: KetTheme.t.panelBg; border.color: KetTheme.t.panelBorder
                                Text { anchors.centerIn: parent; text: "0" + (screen.flow + 1); color: KetTheme.t.textSecondary; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(14) } }
                            Column {
                                Text { text: Fixtures.atlasFlows[screen.flow].title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(20); font.weight: Font.DemiBold }
                                Text { text: screen.flow === 0 ? "From opening a project to a workspace ready for your Agent." : "Reopen a workspace after the Mac restarts."; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14) }
                            }
                        }
                        Row {
                            spacing: 0
                            MiniScreen { number: "01"; title: screen.flow === 0 ? "Open a project" : "Recent workspaces"; heading: screen.flow === 0 ? "Great work starts here." : "Pick up where you left off."; start: true }
                            Item {
                                width: screen.px(150); height: screen.px(378) * screen.zoom
                                Text { anchors.horizontalCenter: parent.horizontalCenter; y: parent.height / 2 - screen.px(26); text: (screen.flow === 0 ? "Open workspace" : "Reconnect") + "  →"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                                Rectangle { y: parent.height / 2; width: parent.width; height: 1.5; color: KetTheme.t.accent }
                            }
                            MiniScreen { number: "02"; title: screen.flow === 0 ? "Workspace ready" : "Workspace restored"; heading: screen.flow === 0 ? "Your Agent is ready." : "Everything is back."; start: false }
                        }
                    }
                }

                Rectangle {
                    x: screen.px(22)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: screen.px(20)
                    width: zoomRow.implicitWidth + screen.px(16)
                    height: screen.px(40)
                    radius: screen.px(7)
                    color: KetTheme.t.panelBg
                    border.color: KetTheme.t.panelBorder
                    Row {
                        id: zoomRow
                        anchors.centerIn: parent
                        spacing: screen.px(4)
                        KetIconButton { iconName: "zoom-out"; text: "Zoom out"; onClicked: screen.zoom = Math.max(0.5, screen.zoom - 0.1) }
                        Text { anchors.verticalCenter: parent.verticalCenter; text: Math.round(screen.zoom * 100) + "%"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        KetIconButton { iconName: "zoom-in"; text: "Zoom in"; onClicked: screen.zoom = Math.min(1.6, screen.zoom + 0.1) }
                        KetButton { text: "Fit map"; iconName: "fullscreen"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.zoom = 1 }
                    }
                }
            }

            // AI Chat that edits the Atlas.
            Rectangle {
                visible: screen.chatOpen
                Layout.fillHeight: true
                Layout.preferredWidth: screen.px(410)
                color: KetTheme.t.panelBg
                Rectangle { width: 1; height: parent.height; color: KetTheme.t.panelBorder }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 1
                    spacing: 0
                    KetPaneHeader {
                        Layout.fillWidth: true
                        title: "AI Chat"
                        iconName: "chat"
                        KetIconButton { controlSize: KetTheme.ControlSize.Small; iconName: "panel-right-close"; text: "Hide AI Chat"; onClicked: screen.chatOpen = false }
                    }
                    Basic.ScrollView {
                        id: chatScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: availableWidth
                        Column {
                            x: screen.px(16); y: screen.px(14)
                            width: chatScroll.availableWidth - 2 * x
                            spacing: screen.px(14)
                            KetUserMessage { width: parent.width; text: "Design an onboarding flow for KetPlus Desktop using Két Design System, from opening a project to the first conversation with an Agent." }
                            KetMarkdown { width: parent.width; markdown: "I will create a focused experience that helps people open a project and get started right away." }
                            KetActivityGroup {
                                width: parent.width
                                text: "Built the Atlas in 4 steps"
                                expanded: true
                                KetActivityRow { text: "Read the context and current features"; iconName: "success" }
                                KetActivityRow { text: "Applied Két Design System"; iconName: "success" }
                                KetActivityRow { text: "Created screens and interactive flows"; iconName: "success" }
                                KetActivityRow { text: "Validated the bundle structure"; iconName: "success" }
                            }
                            KetBanner { width: parent.width; tone: KetTheme.BannerTone.Info; title: "Validated preview available"; message: "4 screens · 2 flows · Két Design System"; actionText: "Open preview"; actionIcon: "open"; onActionTriggered: screen.window.notify("Opened the preview") }
                        }
                    }
                    ComposerBar {
                        Layout.fillWidth: true
                        Layout.margins: screen.px(12)
                        window: screen.window
                        compact: true
                        placeholder: "Ask for a change to " + Fixtures.atlasFlows[screen.flow].title + "…"
                    }
                }
            }
        }
    }
}
