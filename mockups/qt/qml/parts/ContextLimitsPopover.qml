// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Context & limits, opened from the composer's ring: a port of KetPlus desktop's
// UsageLimitsPopover and ClaudeUsageSection. No title; sections split by rules: the
// context window, then for Claude the conversation and organization usage, then the plan
// limits shared across conversations. Codex offers Compact session instead.
Basic.Popup {
    id: popover

    property string provider: "claude"
    property real contextUsed: 0.156
    readonly property bool claude: provider === "claude"
    readonly property color track: KetTheme.t.panelBorder
    // A provider's setup state can supply its own rows (Fixtures.providerSetup.limit).
    property var rows: null
    readonly property var limitRows: rows ? rows : claude ? [
        { label: "Session · 5 hours", resets: "Resets Fri, 06 Nov  12:10 PM", used: 2 },
        { label: "Weekly · all models", resets: "Resets Sun, 08 Nov  7:00 AM", used: 85 },
        { label: "Weekly · Fable", resets: "Resets Sun, 08 Nov  7:00 AM", used: 0 }
    ] : [
        { label: "Session · 5 hours", resets: "Resets Fri, 06 Nov  9:40 PM", used: 38 },
        { label: "Weekly · all models", resets: "Resets Mon, 09 Nov  9:00 AM", used: 71 }
    ]
    // CloseOnPressOutside runs before the ring's click; skip the matching reopen.
    property bool suppressReopen: false

    signal compactRequested()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function fill(percent) {
        return percent >= 90 ? KetTheme.t.danger : percent >= 75 ? KetTheme.t.warning : KetTheme.t.accent
    }
    // Above the anchor, right edges aligned, inside the window.
    function toggleAbove(anchor) {
        if (visible) {
            close()
            return
        }
        if (suppressReopen) {
            suppressReopen = false
            return
        }
        const point = anchor.mapToItem(parent, anchor.width, 0)
        x = Math.max(px(8), Math.min(point.x - width, parent.width - width - px(8)))
        y = Math.max(px(8), point.y - height - px(8))
        open()
    }

    component Rule: Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: popover.px(16)
        Layout.rightMargin: popover.px(16)
        Layout.preferredHeight: 1
        color: popover.track
    }
    component Track: Rectangle {
        property real percent: 0
        Layout.fillWidth: true
        Layout.preferredHeight: popover.px(5)
        radius: height / 2
        color: popover.track
        Rectangle {
            width: parent.width * Math.max(0, Math.min(100, parent.percent)) / 100
            height: parent.height
            radius: height / 2
            color: popover.fill(parent.percent)
        }
    }
    component Label: Text {
        property int size: 15
        color: KetTheme.t.textMain
        font.family: KetTheme.fontFamily
        font.pixelSize: popover.px(size)
    }
    component Note: Label {
        Layout.fillWidth: true
        size: 13
        color: KetTheme.t.textMuted
        wrapMode: Text.WordWrap
    }

    parent: Basic.Overlay.overlay
    width: px(430)
    padding: 0
    modal: false
    focus: true
    closePolicy: Basic.Popup.CloseOnEscape | Basic.Popup.CloseOnPressOutside
    onAboutToHide: {
        suppressReopen = true
        Qt.callLater(() => suppressReopen = false)
    }

    background: Rectangle {
        color: KetTheme.t.panelBg
        radius: popover.px(10)
        border.width: 1
        border.color: popover.track
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Context window
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: popover.px(16)
            Layout.topMargin: popover.px(15)
            Layout.bottomMargin: popover.px(14)
            spacing: popover.px(8)

            RowLayout {
                Layout.fillWidth: true
                Label { text: "Context window" }
                Item { Layout.fillWidth: true }
                Label {
                    text: Math.round(500 * popover.contextUsed) + "k / 500k ("
                          + Math.round(100 * popover.contextUsed) + "%)"
                    color: KetTheme.t.textMuted
                }
            }
            Track { percent: 100 * popover.contextUsed }
            RowLayout {
                Layout.fillWidth: true
                spacing: popover.px(8)
                Label {
                    Layout.fillWidth: true
                    size: 14
                    text: popover.claude ? "Context compaction is managed by the provider"
                                         : "Compacts automatically at 90%"
                    color: KetTheme.t.textMuted
                }
                Basic.Button {
                    id: compactButton
                    visible: !popover.claude
                    implicitWidth: compactText.implicitWidth + popover.px(12)
                    implicitHeight: popover.px(24)
                    padding: 0
                    hoverEnabled: true
                    onClicked: { popover.close(); popover.compactRequested() }
                    KetToolTip { visible: compactButton.hovered; text: "Compact this Codex conversation" }
                    contentItem: Label {
                        id: compactText
                        size: 14
                        text: "Compact session"
                        color: compactButton.hovered ? KetTheme.t.accentActive : KetTheme.t.accent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: popover.px(4)
                        color: compactButton.hovered ? KetTheme.t.accentSubtle : "transparent"
                    }
                }
            }
        }

        Rule {}

        // Claude: this conversation's runtime session and the Admin key's organization.
        ColumnLayout {
            visible: popover.claude
            Layout.fillWidth: true
            Layout.margins: popover.px(16)
            spacing: popover.px(8)

            Label { Layout.fillWidth: true; text: "Conversation · Runtime authentication" }
            Repeater {
                model: [
                    { name: "SDK session estimate", value: "$139.6424" },
                    { name: "Input / output tokens", value: "22.383 / 1.561.880" },
                    { name: "Cache read / write", value: "400.672.151 / 3.522.600" },
                    { name: "Query budget", value: "Managed by runtime" }
                ]
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { Layout.fillWidth: true; size: 14; text: modelData.name; color: KetTheme.t.textMuted; elide: Text.ElideRight }
                    Label { size: 14; text: modelData.value; font.features: { "tnum": 1 } }
                }
            }
            Note { text: "Session totals include restored history and subagents. The query budget counts only the current query's spend, so these values are not a budget progress ratio. SDK cost is an estimate." }
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: popover.px(4)
                Layout.bottomMargin: popover.px(4)
                Layout.preferredHeight: 1
                color: popover.track
            }
            Label { Layout.fillWidth: true; text: "Organization · month to date (UTC)" }
            Label { Layout.fillWidth: true; size: 14; text: "Add a separate Admin API key in Settings → Providers."; wrapMode: Text.WordWrap }
            Note { text: "All products and keys in the Admin key's organization, which may differ from the inference key's account. Cost is after discounts, before credits; balance and spending limits are available in Console." }
            RowLayout {
                spacing: popover.px(8)
                Repeater {
                    model: ["Refresh", "Open Anthropic Console"]
                    Basic.Button {
                        id: accountButton
                        required property string modelData
                        implicitWidth: accountText.implicitWidth + popover.px(20)
                        implicitHeight: popover.px(32)
                        hoverEnabled: true
                        contentItem: Label {
                            id: accountText
                            size: 14
                            text: accountButton.modelData
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: popover.px(5)
                            color: accountButton.hovered ? KetTheme.t.surfaceHover : "transparent"
                            border.width: 1
                            border.color: accountButton.activeFocus ? KetTheme.t.accent : popover.track
                        }
                    }
                }
            }
        }

        // Plan limits, shared by every conversation on this provider.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: popover.px(16)
            Layout.rightMargin: popover.px(16)
            Layout.topMargin: popover.px(13)
            Layout.bottomMargin: popover.px(12)
            spacing: popover.px(11)

            Label { size: 14; text: "Plan usage limits"; color: KetTheme.t.textMuted }
            Repeater {
                model: popover.limitRows
                ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: popover.px(6)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: popover.px(8)
                        Label { Layout.fillWidth: true; text: modelData.label; elide: Text.ElideRight }
                        Label {
                            size: 14
                            text: modelData.resets + "  " + modelData.used + "%"
                            color: KetTheme.t.textMuted
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                    Track { percent: modelData.used }
                }
            }
        }

        Rule {}

        Note {
            Layout.leftMargin: popover.px(16)
            Layout.rightMargin: popover.px(16)
            Layout.topMargin: popover.px(11)
            Layout.bottomMargin: popover.px(13)
            text: "Limits are shared across conversations for this provider."
        }
    }
}
