import Ket
import QtQuick
import Ket as Kit

// The guide for a provider that cannot work (#170, ADR 0110; mockups/qt ProviderSetupCard):
// what happened in plain words, the provider's own message, numbered steps with the exact
// commands, then one primary action. Above the composer it says why the provider cannot start
// a turn; in the transcript it closes the turn the provider ended. `setup` is a
// provider_setup::guide map.
//
// A command is copied, or typed into a terminal beside the conversation for the user to run
// with Return; KetPlus never runs it. Check again asks the daemon to probe again; when the
// same guide comes back, the guide's `still` text says what to try next.
Rectangle {
    id: card
    objectName: "agentProviderSetupCard"

    required property var shell
    required property var setup
    // Above the composer: a Hide button; the model button opens the guide again.
    property bool canHide: false
    // In the transcript: the failed row, for Retry.
    property string messageKey: ""
    // Inside the guide dialog (Model & context → Show setup steps), which carries the title and
    // the frame: no turn steps, and the primary action checks again unless it opens a page.
    property bool inDialog: false
    signal hideRequested()

    readonly property bool danger: setup.tone === "danger"
    readonly property string setupId: setup.id || ""
    // idle | checking | still (the check found the same problem) | settled (a retried turn was sent)
    property string phase: "idle"
    readonly property bool turnCard: messageKey !== ""
    // Retry sends the failed turn's message again; a turn that cannot be resent checks again.
    readonly property bool retryable: turnCard && !!shell.retryableFailure && shell.retryableFailure.key === messageKey

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function act(action) {
        if (!action) {
            phase = "checking"
            checkTimeout.restart()
            agentHost.recheckProviders()
        } else if (action === "retry") {
            phase = "settled"
            shell.retryFailedTurn(messageKey)
        } else if (action.startsWith("settings/")) {
            agentHost.requestSettings(action.slice(9))
        } else if (action === "model" || action.startsWith("switch:")) {
            shell.openModelPicker()
        } else if (action === "limits") {
            if (!shell.limitsPopover.visible)
                shell.limitsPopover.toggleFromAnchor()
        } else if (action.startsWith("new:")) {
            agentHost.requestNewSession()
        }
    }

    // The daemon answered the check: the guide is gone (this card with it), or it is the same.
    Connections {
        target: agentHost
        enabled: card.phase === "checking"
        function onProviderRuntimeChanged() {
            checkTimeout.stop()
            card.phase = card.setup.still ? "still" : "idle"
        }
    }
    Timer {
        id: checkTimeout
        interval: 15000
        onTriggered: card.phase = "idle"
    }

    implicitHeight: column.implicitHeight + (inDialog ? 0 : px(28))
    radius: px(8)
    color: inDialog ? "transparent" : KetTheme.t.panelBg
    border.width: inDialog ? 0 : 1
    border.color: phase === "settled" ? KetTheme.t.panelBorder
                : danger ? KetTheme.t.dangerBorder : KetTheme.t.warningBorder
    Accessible.role: Accessible.Grouping
    Accessible.name: title.text

    Column {
        id: column
        x: card.inDialog ? 0 : card.px(16)
        y: card.inDialog ? 0 : card.px(14)
        width: parent.width - 2 * x
        spacing: card.px(12)

        Row {
            visible: !card.inDialog
            width: parent.width
            spacing: card.px(10)
            Kit.KetIcon {
                id: icon
                anchors.verticalCenter: parent.verticalCenter
                box: card.px(16)
                name: card.phase === "settled" ? "success" : card.danger ? "error" : "warning"
                color: card.phase === "settled" ? KetTheme.t.success : card.danger ? KetTheme.t.danger : KetTheme.t.warning
            }
            Text {
                id: title
                objectName: "agentProviderSetupTitle"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - icon.width - badge.width - 2 * parent.spacing
                                               - (hide.visible ? hide.width + parent.spacing : 0))
                elide: Text.ElideRight
                text: card.phase === "settled" ? card.setup.name + " is working on your message again" : card.setup.title || ""
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: card.px(14)
                font.weight: Font.Medium
            }
            Kit.KetBadge {
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
            textFormat: Text.PlainText
            text: card.phase === "settled" ? "This turn ended with: " + card.setup.title + "."
                : card.inDialog && card.setup.summary ? card.setup.summary : card.setup.reason || ""
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
                objectName: "agentProviderSetupError"
                width: parent.width - said.width - parent.spacing
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
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
                // Retry and the like belong to a turn; above the composer there is none yet.
                model: (card.setup.steps || []).filter(entry => card.turnCard || !entry.turn)
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
                        width: parent.width - card.px(20) - parent.spacing
                        spacing: card.px(2)
                        Text {
                            width: parent.width
                            topPadding: card.px(1)
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: stepRow.modelData.text
                            color: KetTheme.t.textMain
                            font.family: KetTheme.fontFamily
                            font.pixelSize: card.px(13)
                        }
                        Kit.KetCodeBlock {
                            objectName: "agentProviderSetupCommand"
                            visible: !!stepRow.modelData.command
                            width: parent.width
                            code: stepRow.modelData.command || ""
                            language: "shell"
                        }
                        Kit.KetButton {
                            objectName: "agentProviderSetupTypeInTerminal"
                            visible: !!stepRow.modelData.command
                            text: "Type in Terminal"
                            iconName: "terminal"
                            variant: KetTheme.ButtonVariant.Ghost
                            controlSize: KetTheme.ControlSize.Small
                            Accessible.description: "Opens a terminal beside the conversation with the command typed; press Return to run it"
                            onClicked: agentHost.typeInTerminal(stepRow.modelData.command)
                        }
                    }
                }
            }
        }

        Text {
            objectName: "agentProviderSetupStill"
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
            Kit.KetButton {
                objectName: "agentProviderSetupPrimary"
                // Retry needs the failed turn; without one the card checks again.
                readonly property string setupAction: {
                    const action = card.setup.primaryAction || ""
                    if (card.inDialog)
                        return action.startsWith("settings/") && action !== "settings/providers" ? action : ""
                    return action === "retry" && !card.retryable ? "" : action
                }
                text: card.phase === "checking" ? (setupAction === "" ? "Checking…" : "Retrying…")
                    : setupAction === "" ? "Check again" : card.setup.primary || "Check again"
                variant: KetTheme.ButtonVariant.Primary
                enabled: card.phase !== "checking"
                onClicked: card.act(setupAction)
            }
            Kit.KetActivityIndicator {
                visible: card.phase === "checking"
                anchors.verticalCenter: parent.verticalCenter
            }
            Kit.KetButton {
                objectName: "agentProviderSetupSecondary"
                visible: !!card.setup.secondary && !card.inDialog && (card.retryable || card.setup.secondaryAction !== "retry")
                text: card.setup.secondary || ""
                variant: KetTheme.ButtonVariant.Ghost
                enabled: card.phase !== "checking"
                onClicked: card.act(card.setup.secondaryAction)
            }
        }
    }

    // Hide, top right; the model button opens the guide again.
    Kit.KetIconButton {
        id: hide
        objectName: "agentProviderSetupHide"
        visible: card.canHide
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: card.px(8)
        anchors.rightMargin: card.px(8)
        iconName: "close"
        controlSize: KetTheme.ControlSize.Small
        Accessible.name: "Hide setup steps"
        onClicked: card.hideRequested()
    }
}
