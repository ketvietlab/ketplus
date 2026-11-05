import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var workspace
    property var session: ({})
    property var account: ({})
    signal refreshRequested()
    spacing: Metrics.px(8, workspace.t)
    readonly property var report: account.report || ({})
    function dollars(value) {
        return typeof value === "number" && isFinite(value) && value >= 0
            ? "$" + value.toFixed(4) : "Not reported"
    }
    function tokens(value) {
        return typeof value === "number" && isFinite(value) && value >= 0
            ? value.toLocaleString(Qt.locale(), "f", 0) : "Not reported"
    }
    function timestamp(value) {
        const date = new Date(value)
        return value && !isNaN(date.getTime()) ? Qt.formatDateTime(date, "dd MMM h:mm AP") : "Not reported"
    }
    Text {
        Layout.fillWidth: true
        text: "Conversation · " + (root.session.mode === "api_key" ? "API key"
            : root.session.mode === "runtime" ? "Runtime authentication" : "Awaiting usage")
        color: workspace.t.textMain
        font.family: workspace.t.uiFont
        font.pixelSize: workspace.t.reading
        wrapMode: Text.WordWrap
    }
    Repeater {
        model: [
            {name: "SDK session estimate", value: root.dollars(root.session.estimatedCostUsd), id: "claudeSessionCost"},
            {name: "Input / output tokens", value: root.tokens(root.session.inputTokens) + " / " + root.tokens(root.session.outputTokens), id: "claudeSessionTokens"},
            {name: "Cache read / write", value: root.tokens(root.session.cacheReadInputTokens) + " / " + root.tokens(root.session.cacheCreationInputTokens), id: "claudeCacheTokens"},
            {name: "Query budget", value: !root.session.mode ? "Not reported" : root.session.mode !== "api_key" ? "Managed by runtime"
                : root.session.budgetUsd > 0 ? root.dollars(root.session.budgetUsd) : "No query limit", id: "claudeQueryBudget"}
        ]
        delegate: RowLayout {
            required property var modelData
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: modelData.name
                color: workspace.t.textMuted
                font.family: workspace.t.uiFont
                font.pixelSize: workspace.t.body
            }
            Text {
                objectName: modelData.id
                text: modelData.value
                color: workspace.t.textMain
                font.family: workspace.t.uiFont
                font.pixelSize: workspace.t.body
            }
        }
    }
    Text {
        Layout.fillWidth: true
        text: "Session totals include restored history and subagents. The query budget counts only the current query's spend, so these values are not a budget progress ratio. SDK cost is an estimate."
        color: workspace.t.textMuted
        font.family: workspace.t.uiFont
        font.pixelSize: workspace.t.caption
        wrapMode: Text.WordWrap
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: Metrics.px(4, workspace.t)
        Layout.bottomMargin: Metrics.px(4, workspace.t)
        Layout.preferredHeight: 1
        color: workspace.t.border
    }
    Text {
        text: "Organization · month to date (UTC)"
        color: workspace.t.textMain
        font.family: workspace.t.uiFont
        font.pixelSize: workspace.t.reading
    }
    Text {
        objectName: "claudeAccountSummary"
        Layout.fillWidth: true
        text: root.account.loading ? "Loading organization usage…" : root.account.error ||
            (!root.account.configured ? "Add a separate Admin API key in Settings → Providers."
            : typeof root.report.costUsd !== "number" ? "No organization report yet. Refresh to load it."
            : root.dollars(root.report.costUsd) + " · " + root.tokens(root.report.totalTokens) + " tokens")
        color: root.account.error ? workspace.t.warning : workspace.t.textMain
        font.family: workspace.t.uiFont
        font.pixelSize: workspace.t.body
        wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        visible: typeof root.report.costUsd === "number"
        text: "Period: " + (root.report.startingAt || "").slice(0, 10) + " → " + (root.report.endingAt || "").slice(0, 10)
            + " UTC · Data updated " + root.timestamp(root.report.dataRefreshedAt)
            + (root.account.stale ? " · Stale; refresh needed" : "")
        color: workspace.t.textMuted
        font.family: workspace.t.uiFont
        font.pixelSize: workspace.t.caption
        wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        text: "All products and keys in the Admin key's organization, which may differ from the inference key's account. Cost is after discounts, before credits; balance and spending limits are available in Console."
        color: workspace.t.textMuted
        font.family: workspace.t.uiFont
        font.pixelSize: workspace.t.caption
        wrapMode: Text.WordWrap
    }
    component UsageButton: Basic.Button {
        id: button
        padding: Metrics.px(8, workspace.t)
        contentItem: Text {
            text: button.text
            color: button.enabled ? workspace.t.textMain : workspace.t.textDisabled
            font.family: workspace.t.uiFont
            font.pixelSize: workspace.t.body
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Metrics.px(4, workspace.t)
            color: button.hovered ? workspace.t.surfaceHover : "transparent"
            border.width: 1
            border.color: button.activeFocus ? workspace.t.accent : workspace.t.border
        }
    }
    RowLayout {
        UsageButton {
            objectName: "claudeAccountRefresh"
            text: "Refresh"
            enabled: !root.account.loading
            onClicked: root.refreshRequested()
        }
        UsageButton {
            objectName: "claudeAccountConsole"
            text: "Open Anthropic Console"
            onClicked: Qt.openUrlExternally("https://platform.claude.com/usage")
        }
    }
}
