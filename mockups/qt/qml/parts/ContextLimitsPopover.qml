// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Context & limits, opened from the composer's ring, as KetPlus desktop's usage popover:
// the context window, the provider's conversation and organization usage (Claude), then the
// plan limits shared across conversations.
KetPopover {
    id: popover

    property string provider: "codex"
    property real contextUsed: 0.16
    readonly property bool claude: provider === "claude"

    signal compactRequested()

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    parent: Basic.Overlay.overlay
    title: "Context & limits"

    KetMeter {
        width: parent.width
        label: "Context window"
        valueText: Math.round(500 * popover.contextUsed) + "k / 500k (" + Math.round(100 * popover.contextUsed) + "%)"
        fraction: popover.contextUsed
        caption: popover.claude ? "Context compaction is managed by the provider"
                                : "Compacts automatically at 90%"
    }
    Item {
        width: parent.width
        visible: !popover.claude
        height: visible ? compact.implicitHeight + popover.px(8) : 0
        KetButton {
            id: compact
            anchors.bottom: parent.bottom
            controlSize: KetTheme.ControlSize.Small
            text: "Compact session"
            onClicked: { popover.close(); popover.compactRequested() }
        }
    }

    KetPopoverDivider { visible: popover.claude }
    KetPopoverText { visible: popover.claude; role: "heading"; text: "Conversation · Runtime authentication" }
    Repeater {
        model: popover.claude ? [
            { name: "SDK session estimate", value: "$139.6424" },
            { name: "Input / output tokens", value: "22,383 / 1,561,880" },
            { name: "Cache read / write", value: "400,672,151 / 3,522,600" },
            { name: "Query budget", value: "Managed by runtime" }
        ] : []
        RowLayout {
            required property var modelData
            width: parent.width
            height: popover.px(25)
            Text {
                Layout.fillWidth: true
                text: modelData.name
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: popover.px(13)
                elide: Text.ElideRight
            }
            Text {
                text: modelData.value
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: popover.px(13)
                font.features: { "tnum": 1 }
            }
        }
    }
    KetPopoverText {
        visible: popover.claude
        topPadding: popover.px(4)
        text: "Session totals include restored history and subagents. The query budget counts only the current query's spend, so these values are not a budget progress ratio. SDK cost is an estimate."
    }

    KetPopoverDivider { visible: popover.claude }
    KetPopoverText { visible: popover.claude; role: "heading"; text: "Organization · month to date (UTC)" }
    KetPopoverText { visible: popover.claude; role: "body"; text: "Add a separate Admin API key in Settings → Providers." }
    KetPopoverText {
        visible: popover.claude
        text: "All products and keys in the Admin key's organization, which may differ from the inference key's account. Cost is after discounts, before credits; balance and spending limits are available in Console."
    }
    Row {
        visible: popover.claude
        height: visible ? implicitHeight + popover.px(4) : 0
        topPadding: popover.px(8)
        spacing: popover.px(8)
        KetButton { controlSize: KetTheme.ControlSize.Small; text: "Refresh" }
        KetButton { controlSize: KetTheme.ControlSize.Small; text: "Open Anthropic Console" }
    }

    KetPopoverDivider {}
    KetPopoverText { text: "Plan usage limits" }
    Repeater {
        model: popover.claude ? [
            { label: "Session · 5 hours", resets: "Resets Fri, 06 Nov  12:10 PM", used: 0.02 },
            { label: "Weekly · all models", resets: "Resets Sun, 08 Nov  7:00 AM", used: 0.85 },
            { label: "Weekly · Fable", resets: "Resets Sun, 08 Nov  7:00 AM", used: 0 }
        ] : [
            { label: "5-hour limit", resets: "Resets 21:40", used: 0.38 },
            { label: "Weekly limit", resets: "Resets Mon, 09 Nov  9:00 AM", used: 0.71 }
        ]
        KetMeter {
            required property var modelData
            width: parent.width
            label: modelData.label
            valueText: modelData.resets + "  " + Math.round(100 * modelData.used) + "%"
            fraction: modelData.used
        }
    }
    KetPopoverDivider {}
    KetPopoverText { text: "Limits are shared across conversations for this provider." }
}
