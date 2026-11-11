import Ket
import QtQuick
import Ket as Kit
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Popup {
    id: root
    objectName: "usageLimitsPopover"
    readonly property var look: Kit.KetTheme.menuRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom)
    readonly property var contextLook: Kit.KetTheme.meterRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom, false, contextPercent / 100)
    property var workspace
    // Context is supplied only when a provider reports an exact active-window value.
    property var contextUsage: ({})
    // Account-scoped rows: [{ id, label, usedPercent, resetsAt, available }].
    property var limitRows: []
    property bool claude: false
    property var sessionUsage: ({})
    property var accountUsage: ({})
    signal accountRefreshRequested()
    property bool compactEnabled: false
    signal compactRequested()
    // CloseOnPressOutside runs before the anchor click; skip the matching reopen.
    property bool suppressAnchorOpen: false

    readonly property bool contextAvailable: contextUsage && contextUsage.available === true
                                         && Number(contextUsage.limitTokens) > 0
    readonly property real contextPercent: contextAvailable
                                           ? Math.max(0, Math.min(100,
                                               Number(contextUsage.usedTokens) * 100
                                               / Number(contextUsage.limitTokens))) : 0
    readonly property color panelColor: look.background
    readonly property color trackColor: contextLook.track
    readonly property color primaryColor: KetTheme.t.accent
    readonly property color mutedColor: KetTheme.t.textMuted
    readonly property color mainColor: KetTheme.t.textMain

    width: KetTheme.px(430, KetTheme.zoom)
    padding: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onAboutToHide: {
        suppressAnchorOpen = true
        Qt.callLater(function() { suppressAnchorOpen = false })
    }

    function toggleFromAnchor() {
        if (visible) {
            close()
            return
        }
        if (suppressAnchorOpen) {
            suppressAnchorOpen = false
            return
        }
        open()
    }

    function clamp(value) {
        return Math.max(0, Math.min(100, Number(value) || 0))
    }
    function compactNumber(value) {
        const number = Number(value)
        if (!isFinite(number)) return "--"
        if (number >= 1000000) return (number / 1000000).toFixed(number % 1000000 ? 1 : 0) + "m"
        if (number >= 1000) return Math.round(number / 1000) + "k"
        return Math.round(number).toString()
    }
    function resetText(value) {
        if (!value) return "Reset time unavailable"
        const millis = Number(value) > 100000000000 ? Number(value) : Number(value) * 1000
        const date = new Date(millis)
        return isNaN(date.getTime()) ? "Reset time unavailable"
                                   : "Resets " + Qt.formatDateTime(date, "ddd, dd MMM  h:mm AP")
    }
    function progressColor(percent) {
        return Kit.KetTheme.meterRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom, false, clamp(percent) / 100).fill
    }

    background: Rectangle {
        color: root.panelColor
        radius: root.look.radius
        border.width: 1
        border.color: root.look.border
    }

    contentItem: Flickable {
        clip: true
        implicitHeight: Math.min(body.implicitHeight, root.parent ? root.parent.height - 16 : 640)
        contentHeight: body.implicitHeight
        contentWidth: width
        boundsBehavior: Flickable.StopAtBounds
        Basic.ScrollBar.vertical: Basic.ScrollBar { }
        ColumnLayout {
            id: body
            width: parent.width
            spacing: 0
            Item {
                Layout.fillWidth: true
                Layout.leftMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.rightMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.topMargin: KetTheme.px(15, KetTheme.zoom)
                Layout.bottomMargin: root.contextAvailable ? 14 : 15
                implicitHeight: contextColumn.implicitHeight

                ColumnLayout {
                    id: contextColumn
                    anchors.fill: parent
                    spacing: KetTheme.px(8, KetTheme.zoom)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: KetTheme.px(8, KetTheme.zoom)
                        Text {
                            text: "Context window"
                            color: root.mainColor
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            objectName: "contextUsageValue"
                            text: root.contextAvailable
                                  ? root.compactNumber(root.contextUsage.usedTokens) + " / "
                                    + root.compactNumber(root.contextUsage.limitTokens) + " ("
                                    + Math.round(root.contextPercent) + "%)"
                                  : "Not reported"
                            color: root.contextAvailable ? root.mutedColor : KetTheme.t.textDisabled
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                        }
                    }

                    Rectangle {
                        visible: root.contextAvailable
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.contextLook.trackHeight
                        radius: height / 2
                        color: root.trackColor
                        Rectangle {
                            objectName: "contextUsageProgress"
                            width: parent.width * root.contextPercent / 100
                            height: parent.height
                            radius: height / 2
                            color: root.progressColor(root.contextPercent)
                        }
                        Rectangle {
                            visible: Number(root.contextUsage.autoCompactPercent) > 0
                            x: Math.max(0, Math.min(parent.width - width,
                                parent.width * Number(root.contextUsage.autoCompactPercent) / 100))
                            width: KetTheme.px(2, KetTheme.zoom)
                            height: parent.height + KetTheme.px(4, KetTheme.zoom)
                            anchors.verticalCenter: parent.verticalCenter
                            color: root.panelColor
                        }
                    }

                    RowLayout {
                        visible: root.contextAvailable
                        Layout.fillWidth: true
                        spacing: KetTheme.px(8, KetTheme.zoom)
                        Text {
                            Layout.fillWidth: true
                            text: Number(root.contextUsage.autoCompactPercent) > 0
                                  ? "Compacts automatically at "
                                    + Math.round(Number(root.contextUsage.autoCompactPercent)) + "%"
                                  : "Context compaction is managed by the provider"
                            color: root.mutedColor
                            font.family: KetTheme.fontFamily
                            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                        }
                        Basic.Button {
                            id: compactSessionButton
                            objectName: "compactSessionButton"
                            visible: root.compactEnabled
                            enabled: root.compactEnabled
                            implicitWidth: compactText.implicitWidth + KetTheme.px(12, KetTheme.zoom)
                            implicitHeight: KetTheme.px(24, KetTheme.zoom)
                            padding: 0
                            onClicked: root.compactRequested()
                            contentItem: Text {
                                id: compactText
                                text: "Compact session"
                                color: KetTheme.t.accentText
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: compactSessionButton.hovered ? KetTheme.t.accentSubtle : "transparent"
                                radius: KetTheme.px(4, KetTheme.zoom)
                            }
                            KetToolTip {
                                objectName: "ketToolTip"
                                visible: compactSessionButton.hovered
                                text: "Compact this Codex conversation"
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.rightMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.preferredHeight: 1
                color: root.trackColor
            }

            ClaudeUsageSection {
                visible: root.claude
                workspace: root.workspace
                session: root.sessionUsage
                account: root.accountUsage
                Layout.fillWidth: true
                Layout.margins: KetTheme.px(16, KetTheme.zoom)
                onRefreshRequested: root.accountRefreshRequested()
            }
            ColumnLayout {
                visible: !root.claude || root.sessionUsage.mode !== "api_key"
                Layout.fillWidth: true
                Layout.leftMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.rightMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.topMargin: KetTheme.px(13, KetTheme.zoom)
                Layout.bottomMargin: KetTheme.px(12, KetTheme.zoom)
                spacing: KetTheme.px(11, KetTheme.zoom)
                Text {
                    text: "Plan usage limits"
                    color: root.mutedColor
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                }

                Repeater {
                    model: root.limitRows
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: KetTheme.px(6, KetTheme.zoom)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: KetTheme.px(8, KetTheme.zoom)
                            Text {
                                Layout.fillWidth: true
                                text: modelData.label || "Usage limit"
                                color: root.mainColor
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.markdownRecipe(KetTheme.dark, KetTheme.zoom).bodyPixels
                                elide: Text.ElideRight
                            }
                            Text {
                                text: modelData.available
                                      ? root.resetText(modelData.resetsAt) + "  "
                                        + Math.round(root.clamp(modelData.usedPercent)) + "%"
                                      : "Not reported"
                                color: modelData.available ? root.mutedColor : KetTheme.t.textDisabled
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.contextLook.trackHeight
                            radius: height / 2
                            color: root.trackColor
                            Rectangle {
                                objectName: "usageLimitProgress_" + (modelData.id || index)
                                width: parent.width * (modelData.available
                                                      ? root.clamp(modelData.usedPercent) / 100 : 0)
                                height: parent.height
                                radius: height / 2
                                color: root.progressColor(root.clamp(modelData.usedPercent))
                            }
                        }
                    }
                }

                Text {
                    visible: root.limitRows.length === 0
                    text: "No plan limits are available for this provider."
                    color: KetTheme.t.textDisabled
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.rightMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.preferredHeight: 1
                color: root.trackColor
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.rightMargin: KetTheme.px(16, KetTheme.zoom)
                Layout.topMargin: KetTheme.px(11, KetTheme.zoom)
                Layout.bottomMargin: KetTheme.px(13, KetTheme.zoom)
                visible: !root.claude || root.sessionUsage.mode !== "api_key"
                text: "Limits are shared across conversations for this provider."
                color: root.mutedColor
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.px(13, KetTheme.zoom)
                wrapMode: Text.WordWrap
            }
        }
    }
}
