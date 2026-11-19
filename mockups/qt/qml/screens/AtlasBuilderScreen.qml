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

    property int selectedStep: 0
    property string selectedTrail: "Ember Ridge Loop"
    property string observation: "Loose gravel near the saddle."
    readonly property var currentFlow: Fixtures.atlasFlows[flow]

    function selectFlow(index) {
        if (flow !== index) {
            flow = index
            selectedStep = 0
            mapView.contentX = 0
            mapView.contentY = 0
        }
    }

    function evidenceChecks() {
        return flowNodes.width <= mapView.width - 2 * map.x
            && map.height + 2 * map.y <= mapView.height
    }

    function advance(step) {
        if (step + 1 < currentFlow.nodes.length)
            selectedStep = step + 1
        else
            selectFlow(flow === 0 ? 1 : 0)
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
                    Text { text: "First field entry"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.DemiBold }
                    Text { text: screen.currentFlow.title + " · " + screen.currentFlow.nodes.length + " screens"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(11) }
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
                Layout.preferredWidth: screen.px(174)
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
                            detail: modelData.nodes.length + " screens"
                            iconName: "branch"
                            checked: screen.flow === index
                            onClicked: screen.selectFlow(index)
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
                    id: mapView
                    anchors.fill: parent
                    contentWidth: map.width + 2 * map.x
                    contentHeight: map.height + 2 * map.y
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: map
                        x: screen.px(20)
                        y: screen.px(52)
                        spacing: screen.px(28)
                        width: Math.max(flowNodes.width, mapView.width - 2 * x)

                        Column {
                            width: mapView.width - 2 * map.x
                            spacing: screen.px(6)
                            Text {
                                width: parent.width
                                text: screen.currentFlow.title
                                wrapMode: Text.WordWrap
                                color: KetTheme.t.textMain
                                font.family: KetTheme.fontFamily
                                font.pixelSize: screen.px(22)
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: screen.currentFlow.summary
                                wrapMode: Text.WordWrap
                                color: KetTheme.t.textSecondary
                                font.family: KetTheme.fontFamily
                                font.pixelSize: screen.px(14)
                            }
                        }

                        Row {
                            id: flowNodes
                            readonly property real connectorWidth: screen.px(38) * screen.zoom
                            readonly property int nodeCount: screen.currentFlow.nodes.length
                            readonly property real nodeWidth: Math.max(screen.px(184),
                                Math.min(screen.px(242), Math.floor((mapView.width - 2 * map.x
                                    - screen.px(38) * (nodeCount - 1)) / nodeCount))) * screen.zoom

                            Repeater {
                                model: screen.currentFlow.nodes
                                Row {
                                    id: step
                                    required property var modelData
                                    required property int index
                                    Item {
                                        id: previewBounds
                                        width: flowNodes.nodeWidth
                                        height: preview.height * screen.zoom
                                        AtlasScreenPreview {
                                            id: preview
                                            width: parent.width / screen.zoom
                                            scale: screen.zoom
                                            transformOrigin: Item.TopLeft
                                            node: step.modelData
                                            step: step.index
                                            current: screen.selectedStep === step.index
                                            trail: screen.selectedTrail
                                            savedNote: screen.observation
                                            onActivated: screen.selectedStep = step.index
                                            onTrailSelected: name => {
                                                screen.selectedTrail = name
                                                screen.selectedStep = step.index
                                            }
                                            onAdvanced: screen.advance(step.index)
                                            onSaved: note => {
                                                screen.observation = note
                                                screen.advance(step.index)
                                            }
                                        }
                                    }
                                    Item {
                                        visible: !!step.modelData.transition
                                        width: flowNodes.connectorWidth
                                        height: previewBounds.height
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            anchors.bottom: connection.top
                                            anchors.bottomMargin: screen.px(6)
                                            text: step.modelData.transition || ""
                                            color: KetTheme.t.textSecondary
                                            font.family: KetTheme.fontFamily
                                            font.pixelSize: screen.px(11)
                                        }
                                        Rectangle {
                                            id: connection
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: parent.width
                                            height: 1
                                            color: KetTheme.t.accent
                                        }
                                        KetIcon {
                                            anchors.right: parent.right
                                            anchors.verticalCenter: connection.verticalCenter
                                            name: "chevron-right"
                                            box: screen.px(12)
                                            color: KetTheme.t.accentText
                                        }
                                    }
                                }
                            }
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
                Layout.preferredWidth: screen.px(344)
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
                            KetUserMessage { width: parent.width; text: Fixtures.atlasPrompt }
                            KetMarkdown { width: parent.width; markdown: "The first flow now ends with a local receipt. The sync flow picks up the same observation and confirms it uploaded." }
                            KetActivityGroup {
                                width: parent.width
                                text: "Built the Atlas in 4 steps"
                                expanded: true
                                KetActivityRow { text: "Read the field-note release brief"; iconName: "success" }
                                KetActivityRow { text: "Applied Két Design System"; iconName: "success" }
                                KetActivityRow { text: "Connected the field and sync flows"; iconName: "success" }
                                KetActivityRow { text: "Validated the bundle structure"; iconName: "success" }
                            }
                            KetBanner { width: parent.width; tone: KetTheme.BannerTone.Info; title: "Preview validated"; message: Fixtures.atlasSummary + " · Két Design System" }
                        }
                    }
                    ComposerBar {
                        Layout.fillWidth: true
                        Layout.margins: screen.px(12)
                        window: screen.window
                        compact: true
                        placeholder: "Ask AI to edit this flow…"
                    }
                }
            }
        }
    }
}
