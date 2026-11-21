import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic

// Provider page tabs drawn from the kit's route-tab recipe, as KetRouteTabs draws the mock's.
// They stay a Repeater of buttons because each provider tab can be disabled or locked.
Item {
    id: providerTabs

    property string selectedProvider
    property string lockedProvider
    property bool hasClaudeModels: false
    property bool hasCursorModels: false
    property bool hasCopilotModels: false
    property string cursorInstallCommand
    property string copilotInstallCommand
    // Providers with a setup guide (ADR 0110): their tab opens, so the picker's banner shows.
    property var setupProviders: []
    signal providerChosen(string provider)

    readonly property var recipe: KetTheme.routeTabRecipe(KetTheme.dark, KetTheme.zoom, false, false, false, true, false)
    implicitHeight: providerGrid.implicitHeight

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: providerTabs.recipe.rule
    }

    Flow {
        id: providerGrid
        objectName: "providerTabs"
        width: parent.width
        spacing: providerTabs.recipe.spacing

        Repeater {
            model: [
                {id: "codex", label: "Codex"},
                {id: "claude", label: "Claude"},
                {id: "cursor", label: "Cursor"},
                {id: "copilot", label: "GitHub Copilot"},
                {id: "ketrouter", label: "K\u00e9tRouter"}
            ]
            Basic.ToolButton {
                id: providerButton
                required property var modelData
                objectName: "providerTab_" + modelData.id
                readonly property bool supported: modelData.id === "codex"
                    || (modelData.id === "claude" && providerTabs.hasClaudeModels)
                    || (modelData.id === "cursor" && providerTabs.hasCursorModels)
                    || (modelData.id === "copilot" && providerTabs.hasCopilotModels)
                    || providerTabs.setupProviders.indexOf(modelData.id) >= 0
                readonly property bool lockedOut: supported && providerTabs.lockedProvider !== "" && providerTabs.lockedProvider !== modelData.id
                readonly property var recipe: KetTheme.routeTabRecipe(KetTheme.dark, KetTheme.zoom, enabled && providerTabs.selectedProvider === modelData.id, hovered, down, enabled, visualFocus)
                enabled: supported && !lockedOut
                hoverEnabled: true
                padding: 0
                opacity: recipe.opacity
                implicitWidth: tabLabel.implicitWidth
                implicitHeight: recipe.paddingTop + tabLabel.implicitHeight + recipe.paddingBottom + recipe.indicatorHeight
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.label
                Accessible.selected: enabled && providerTabs.selectedProvider === modelData.id
                Accessible.description: enabled ? (providerTabs.selectedProvider === modelData.id ? "Current provider" : "Select provider") : lockedOut ? "Switch to " + modelData.label + " after this turn" : modelData.id === "cursor" ? "Install Cursor CLI, then retry" : modelData.id === "copilot" ? "Install GitHub Copilot CLI on the daemon's machine, then retry" : "Not supported yet"
                contentItem: Item {
                    Text {
                        id: tabLabel
                        y: providerButton.recipe.paddingTop
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: providerButton.modelData.label
                        color: providerButton.recipe.text
                        font.family: KetTheme.fontFamily
                        font.pixelSize: providerButton.recipe.fontPixels
                    }
                }
                background: Item {
                    Rectangle {
                        anchors.fill: parent
                        visible: providerButton.recipe.focusRing.a > 0
                        radius: 3
                        color: "transparent"
                        border.width: 2
                        border.color: providerButton.recipe.focusRing
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: providerButton.recipe.indicatorHeight
                        color: providerButton.recipe.indicator
                    }
                }
                KetToolTip {
                    objectName: "ketToolTip"
                    visible: providerButton.hovered
                    text: providerButton.enabled ? providerButton.modelData.label
                          : providerButton.lockedOut ? "Switch to " + providerButton.modelData.label + " after this turn"
                          : providerButton.modelData.id === "cursor"
                            ? "Install Cursor CLI: " + providerTabs.cursorInstallCommand
                          : providerButton.modelData.id === "copilot"
                            ? "Install GitHub Copilot CLI: " + providerTabs.copilotInstallCommand
                          : providerButton.modelData.label + ": not supported yet"
                }
                onClicked: {
                    if (!providerButton.enabled) return
                    providerTabs.providerChosen(providerButton.modelData.id)
                }
            }
        }
    }
}
