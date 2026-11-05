// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// The composer with its lower toolbar: attachments and permissions on the left; the model,
// effort and the Context & limits ring on the right. The ring opens its panel in place.
Item {
    id: bar

    required property var window
    property bool busy: false
    property bool compact: false // narrow columns hide the permission and the effort
    property string placeholder: "Message Codex…"
    property string model: Fixtures.models[0]
    property string effort: "Medium"
    property string permission: "Workspace"

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    implicitHeight: composer.implicitHeight

    KetComposer {
        id: composer
        width: parent.width
        busy: bar.busy
        placeholderText: bar.placeholder
        onSubmitted: text => {
            bar.window.notify("Sent to " + bar.model.split(" · ")[0])
            composer.text = ""
        }
        onStopRequested: bar.window.notify("Stopped the turn")

        leadingTools: [
            KetIconButton {
                id: attach
                controlSize: KetTheme.ControlSize.Small
                iconName: "plus"
                text: "Add context"
                onClicked: attachMenu.popup(attach, 0, -attachMenu.height)
            },
            KetButton {
                id: permissionButton
                visible: !bar.compact
                controlSize: KetTheme.ControlSize.Small
                variant: KetTheme.ButtonVariant.Ghost
                iconName: "shield"
                text: bar.permission
                onClicked: permissionMenu.popup(permissionButton, 0, -permissionMenu.height)
            }
        ]

        trailingTools: [
            KetButton {
                id: modelButton
                controlSize: KetTheme.ControlSize.Small
                variant: KetTheme.ButtonVariant.Ghost
                text: bar.compact ? bar.model.split(" · ")[1] : bar.model
                onClicked: modelMenu.popup(modelButton, 0, -modelMenu.height)
            },
            KetButton {
                id: effortButton
                visible: !bar.compact
                controlSize: KetTheme.ControlSize.Small
                variant: KetTheme.ButtonVariant.Ghost
                text: bar.effort
                onClicked: effortMenu.popup(effortButton, 0, -effortMenu.height)
            },
            KetUsageRing {
                id: ring
                value: 0.62
                onClicked: usage.openAbove(ring)
            }
        ]
    }

    KetMenu {
        id: attachMenu
        KetMenuItem { text: "Attach files…"; iconName: "attachment"; onTriggered: bar.window.notify("Attached design-system.md") }
        KetMenuItem { text: "Add a screenshot"; iconName: "image"; onTriggered: bar.window.notify("Added a screenshot") }
        KetMenuItem { text: "Mention a conversation"; iconName: "chat"; shortcut: "@"; onTriggered: composer.text += "@agent/" }
    }

    KetMenu {
        id: permissionMenu
        Repeater {
            model: [
                { text: "Workspace", description: "Edit files in this worktree; ask before anything else" },
                { text: "Read only", description: "Read and search; never edit or run commands" },
                { text: "Full access", description: "Edit, run commands and use the network without asking" }
            ]
            KetMenuItem {
                required property var modelData
                text: modelData.text
                description: modelData.description
                checkable: true
                checked: bar.permission === modelData.text
                onTriggered: bar.permission = modelData.text
            }
        }
    }

    KetMenu {
        id: modelMenu
        Repeater {
            model: Fixtures.models
            KetMenuItem {
                required property string modelData
                text: modelData
                checkable: true
                checked: bar.model === modelData
                onTriggered: bar.model = modelData
            }
        }
    }

    KetMenu {
        id: effortMenu
        Repeater {
            model: Fixtures.efforts
            KetMenuItem {
                required property string modelData
                text: modelData
                checkable: true
                checked: bar.effort === modelData
                onTriggered: bar.effort = modelData
            }
        }
    }

    // Weekly usage appears only inside Context & limits.
    KetPopover {
        id: usage
        parent: Basic.Overlay.overlay
        title: "Context & limits"
        KetMeter { width: parent.width; label: "Context window"; valueText: "62%"; fraction: 0.62; caption: "124k of 200k tokens" }
        KetPopoverDivider {}
        KetMeter { width: parent.width; label: "5-hour limit"; valueText: "38%"; fraction: 0.38; caption: "Resets at 21:40" }
        KetMeter { width: parent.width; label: "Weekly limit"; valueText: "71%"; fraction: 0.71; caption: "Resets Monday" }
        KetPopoverText { text: "Compact the conversation to free context without losing the summary." }
        footer: KetButton {
            text: "Compact now"
            controlSize: KetTheme.ControlSize.Small
            onClicked: { usage.close(); bar.window.notify("Compacting the conversation") }
        }
    }
}
