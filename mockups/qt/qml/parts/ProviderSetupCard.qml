// SPDX-License-Identifier: MPL-2.0
import QtQuick
import Ket
import KetPlusMock

// The guide for a provider that cannot work (Fixtures.providerSetup): what happened in plain
// words, the provider's own message, numbered steps with the exact commands, then one primary
// action. It sits above the composer when the provider cannot start a turn, closes the turn a
// provider ended, and repeats in Settings → Providers.
//
// A command is copied, or typed into a terminal beside the conversation for the user to run
// with Return; KetPlus never runs it. Check again and Retry walk the mock's live states:
// checking, then still failing once when the state has `still`, then fixed.
Rectangle {
    id: card

    required property string setupId
    required property var window
    // In Settings → Providers the page shows the provider's name and badge, the primary action
    // checks the provider again (Retry belongs to the conversation), and only links to other
    // pages stay.
    property bool inSettings: false
    readonly property var setup: Fixtures.providerSetup[setupId] || ({})
    readonly property bool danger: setup.tone === "danger"
    readonly property string primaryAction: {
        const action = setup.primaryAction || ""
        if (!inSettings)
            return action
        return action.startsWith("settings/") && action !== "settings/providers" ? action : ""
    }
    readonly property bool navigates: primaryAction !== "" && primaryAction !== "retry"
    readonly property bool checks: setup.blocking || inSettings
    // idle | checking | still (the check failed again) | settled (a retried turn was sent)
    property string phase: "idle"
    property bool checkedOnce: false

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    // Primary and secondary actions; an empty action checks again (or retries a turn).
    function act(action) {
        if (!action || action === "retry") {
            phase = "checking"
            step.restart()
        } else if (action.startsWith("settings/")) {
            window.showScreen(action)
        } else if (action === "model" || action === "limits") {
            window.openComposerPanel(action)
        } else if (action.startsWith("switch:")) {
            window.switchProvider(action.slice(7))
        } else if (action.startsWith("new:")) {
            window.newConversation(action.slice(4))
        }
    }

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight + px(28)
    radius: px(8)
    color: KetTheme.t.panelBg
    border.color: phase === "settled" ? KetTheme.t.panelBorder
                : danger ? KetTheme.t.dangerBorder : KetTheme.t.warningBorder
    Accessible.role: Accessible.Grouping
    Accessible.name: title.text

    // The mock's provider check: a moment, then still failing once if the state says so.
    Timer {
        id: step
        interval: 1200
        onTriggered: {
            if (card.setup.still && !card.checkedOnce) {
                card.checkedOnce = true
                card.phase = "still"
                return
            }
            if (card.checks) {
                card.phase = "idle"
                card.window.notify(card.setup.ready || card.setup.name + " is ready")
            } else {
                card.phase = "settled"
                card.window.notify("Sent your last message to " + card.setup.name + " again")
            }
            card.window.setProviderIssue(card.setup.provider, "")
        }
    }

    Column {
        id: column
        x: card.px(16)
        y: card.px(14)
        width: parent.width - 2 * x
        spacing: card.px(12)

        Row {
            visible: !card.inSettings
            width: parent.width
            spacing: card.px(10)
            KetIcon {
                id: icon
                anchors.verticalCenter: parent.verticalCenter
                box: card.px(16)
                name: card.phase === "settled" ? "success" : card.danger ? "error" : "warning"
                color: card.phase === "settled" ? KetTheme.t.success : card.danger ? KetTheme.t.danger : KetTheme.t.warning
            }
            Text {
                id: title
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - icon.width - badge.width - 2 * parent.spacing)
                elide: Text.ElideRight
                text: card.phase === "settled" ? card.setup.name + " is working on your message again" : card.setup.title || ""
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: card.px(14)
                font.weight: Font.Medium
            }
            KetBadge {
                id: badge
                visible: card.phase !== "settled"
                anchors.verticalCenter: parent.verticalCenter
                text: card.setup.badge || ""
                tone: card.danger ? KetTheme.BadgeTone.Danger : KetTheme.BadgeTone.Warning
            }
        }

        // Once a retried turn is on its way, only the record of what failed stays.
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: card.phase === "settled" ? "This turn ended with: " + card.setup.title + "."
                : card.inSettings && card.setup.summary ? card.setup.summary : card.setup.reason || ""
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: card.px(13)
        }

        // The provider's own words, so a search or a support request finds the same text.
        Row {
            visible: !!card.setup.error && card.phase !== "settled"
            width: parent.width
            spacing: card.px(8)
            Text {
                id: said
                text: (card.setup.source || card.setup.name) + " said"
                color: KetTheme.t.textMuted
                font.family: KetTheme.fontFamily
                font.pixelSize: card.px(12)
            }
            Text {
                width: parent.width - said.width - parent.spacing
                wrapMode: Text.Wrap
                text: card.setup.error || ""
                color: KetTheme.t.textSecondary
                font.family: KetTheme.monoFontFamily
                font.pixelSize: card.px(12)
            }
        }

        Column {
            visible: card.phase !== "settled"
            width: parent.width
            spacing: card.px(10)
            Repeater {
                model: (card.setup.steps || []).filter(entry => !(card.inSettings && entry.turn))
                Row {
                    id: stepRow
                    required property var modelData
                    required property int index
                    width: parent.width
                    spacing: card.px(10)
                    Rectangle {
                        width: card.px(20)
                        height: width
                        radius: width / 2
                        color: KetTheme.t.panelBgSubtle
                        border.color: KetTheme.t.panelBorder
                        Text {
                            anchors.centerIn: parent
                            text: stepRow.index + 1
                            color: KetTheme.t.textSecondary
                            font.family: KetTheme.fontFamily
                            font.pixelSize: card.px(12)
                            font.weight: Font.Medium
                        }
                    }
                    Column {
                        width: parent.width - card.px(30)
                        spacing: card.px(2)
                        Text {
                            width: parent.width
                            topPadding: card.px(1)
                            wrapMode: Text.Wrap
                            text: stepRow.modelData.text
                            color: KetTheme.t.textMain
                            font.family: KetTheme.fontFamily
                            font.pixelSize: card.px(13)
                        }
                        KetCodeBlock {
                            visible: !!stepRow.modelData.command
                            width: parent.width
                            code: stepRow.modelData.command || ""
                            language: "shell"
                            onCopiedToClipboard: card.window.notify("Copied " + code)
                        }
                        KetButton {
                            visible: !!stepRow.modelData.command
                            text: "Type in Terminal"
                            iconName: "terminal"
                            variant: KetTheme.ButtonVariant.Ghost
                            controlSize: KetTheme.ControlSize.Small
                            Accessible.description: "Opens a terminal beside the conversation with the command typed; press Return to run it"
                            onClicked: card.window.notify("Opened a terminal with " + stepRow.modelData.command
                                                          + " typed. Press Return to run it.")
                        }
                    }
                }
            }
        }

        Text {
            visible: card.phase === "still"
            width: parent.width
            wrapMode: Text.Wrap
            text: card.setup.still || ""
            color: KetTheme.t.danger
            font.family: KetTheme.fontFamily
            font.pixelSize: card.px(13)
        }

        Row {
            visible: card.phase !== "settled"
            spacing: card.px(10)
            KetButton {
                text: card.phase === "checking" ? (card.checks ? "Checking…" : "Retrying…")
                    : card.inSettings && !card.navigates ? "Check again" : card.setup.primary || "Check again"
                variant: KetTheme.ButtonVariant.Primary
                enabled: card.phase !== "checking"
                onClicked: card.act(card.primaryAction)
            }
            KetActivityIndicator {
                visible: card.phase === "checking"
                anchors.verticalCenter: parent.verticalCenter
            }
            KetButton {
                visible: !!card.setup.secondary && !card.inSettings
                text: card.setup.secondary || ""
                variant: KetTheme.ButtonVariant.Ghost
                enabled: card.phase !== "checking"
                onClicked: card.act(card.setup.secondaryAction)
            }
        }
    }
}
