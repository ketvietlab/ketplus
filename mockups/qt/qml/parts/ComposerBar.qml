// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// The composer as KetPlus desktop lays it out: the card holds only the message and its
// send/stop circle; the row below holds attachments and the mode on the left, then the
// model with its effort and the Context & limits ring on the right. The model button opens
// Model & context; the ring opens its panel in place.
Item {
    id: bar

    required property var window
    property bool busy: false
    property bool compact: false // narrow columns keep only the mode's icon
    property string placeholder: "Message Codex…"
    property string provider: "codex"
    property string model: "GPT-5.6 Luna"
    property string effort: "Medium"
    property string permission: "Workspace"
    property real contextUsed: 0.16
    readonly property var mode: {
        for (const entry of Fixtures.permissionModes)
            if (entry.label === bar.permission)
                return entry
        return Fixtures.permissionModes[1]
    }
    readonly property string providerLabel: {
        for (const entry of Fixtures.providers)
            if (entry.id === bar.provider)
                return entry.label
        return ""
    }

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    // Review evidence: --open=model|mode|limits.
    function openPanel(name) {
        if (name === "model")
            modelDialog.open()
        else if (name === "mode")
            permissionButton.clicked()
        else if (name === "limits")
            limits.openAbove(ring)
    }
    function choosePermission(index) {
        bar.permission = Fixtures.permissionModes[index].label
        permissionMenu.close()
    }

    implicitHeight: composer.implicitHeight + footer.anchors.topMargin + footer.height

    KetComposer {
        id: composer
        width: parent.width
        busy: bar.busy
        placeholderText: bar.placeholder
        onSubmitted: text => {
            bar.window.notify("Sent to " + bar.providerLabel)
            composer.text = ""
        }
        onStopRequested: bar.window.notify("Stopped the turn")
    }

    RowLayout {
        id: footer
        anchors.top: composer.bottom
        anchors.topMargin: bar.px(4)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: bar.px(2)
        anchors.rightMargin: bar.px(2)
        height: bar.px(28)
        spacing: bar.px(2)

        KetIconButton {
            id: attach
            controlSize: KetTheme.ControlSize.Small
            iconWeight: KetTheme.IconWeight.Regular
            glyphSize: composer.recipe.attachIconSize
            iconName: "plus"
            text: "Add images or text files"
            onClicked: attachMenu.popup(attach, 0, -attachMenu.height - bar.px(4))
        }

        ComposerFooterButton {
            id: permissionButton
            iconName: bar.mode.icon
            text: bar.mode.label
            warning: bar.mode.elevated
            showLabel: !bar.compact
            open: permissionMenu.visible
            Accessible.description: "Permissions for the next message. Restricted actions are blocked without approval prompts."
            onClicked: permissionMenu.popup(permissionButton, 0, -permissionMenu.height - bar.px(4))
        }

        Item { Layout.fillWidth: true }

        ComposerFooterButton {
            id: modelButton
            Layout.maximumWidth: Math.max(bar.px(40), bar.width - bar.px(bar.compact ? 120 : 240))
            text: bar.providerLabel + " · " + bar.model + "   " + bar.effort
            open: modelDialog.visible
            Accessible.name: "Choose model and reasoning effort"
            Accessible.description: bar.providerLabel + " · " + bar.model + " · " + bar.effort + " effort"
            onClicked: modelDialog.open()
        }

        KetUsageRing {
            id: ring
            Layout.alignment: Qt.AlignVCenter
            value: bar.contextUsed
            onClicked: limits.openAbove(ring)
        }
    }

    KetMenu {
        id: attachMenu
        KetMenuItem { text: "Attach files…"; iconName: "attachment"; onTriggered: bar.window.notify("Attached design-system.md") }
        KetMenuItem { text: "Add a screenshot"; iconName: "image"; onTriggered: bar.window.notify("Added a screenshot") }
        KetMenuItem { text: "Mention a conversation"; iconName: "chat"; shortcut: "@"; onTriggered: composer.text += "@agent/" }
    }

    KetMenu {
        id: permissionMenu
        width: Math.min(bar.px(340), bar.window.width - bar.px(24))
        KetMenuSection { text: "Mode" }
        Repeater {
            model: Fixtures.permissionModes
            KetMenuItem {
                required property var modelData
                required property int index
                text: modelData.label
                description: modelData.description
                iconName: modelData.icon
                warning: modelData.elevated
                shortcut: String(index + 1)
                checkable: true
                checked: bar.permission === modelData.label
                onTriggered: bar.choosePermission(index)
                // The highlighted item has focus: 1–3 choose from anywhere in the menu.
                Keys.onPressed: event => {
                    const choice = event.key - Qt.Key_1
                    if (choice >= 0 && choice < Fixtures.permissionModes.length) {
                        bar.choosePermission(choice)
                        event.accepted = true
                    }
                }
            }
        }
    }

    ModelContextDialog {
        id: modelDialog
        parent: Basic.Overlay.overlay
        provider: bar.provider
        model: bar.model
        effort: bar.effort
        onChosen: (provider, model, effort) => {
            bar.provider = provider
            bar.model = model
            bar.effort = effort
            bar.window.notify("Applies from the next turn")
        }
    }

    ContextLimitsPopover {
        id: limits
        provider: bar.provider
        contextUsed: bar.contextUsed
        onCompactRequested: bar.window.notify("Compacting the conversation")
    }
}
