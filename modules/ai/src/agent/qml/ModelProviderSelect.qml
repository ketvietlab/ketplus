import Ket
import QtQuick

// The provider of Model & context: a select box with its label above, as the mock's. Four
// providers or more do not fit a row of tabs. A provider that cannot be chosen is listed
// disabled with the reason as its secondary line.
KetFieldRow {
    id: providerSelect

    // provider_display's table, from agentHost.providerChoices: [{id, label, name}].
    property var providers: []
    property string selectedProvider
    property string lockedProvider
    property bool hasClaudeModels: false
    property bool hasCursorModels: false
    property bool hasCopilotModels: false
    property string cursorInstallCommand
    property string copilotInstallCommand
    // Providers with a setup guide (ADR 0110): they can be chosen, so the picker's banner shows.
    property var setupProviders: []
    signal providerChosen(string provider)

    // Each provider with whether it can be chosen now and, when not, why.
    readonly property var choices: providers.map(function(entry) {
        const supported = entry.id === "codex"
            || (entry.id === "claude" && hasClaudeModels)
            || (entry.id === "cursor" && hasCursorModels)
            || (entry.id === "copilot" && hasCopilotModels)
            || setupProviders.indexOf(entry.id) >= 0
        const lockedOut = supported && lockedProvider !== "" && lockedProvider !== entry.id
        const reason = supported ? (lockedOut ? "Switch to " + entry.name + " after this turn" : "")
            : entry.id === "cursor" ? "Install Cursor CLI: " + cursorInstallCommand
            : entry.id === "copilot" ? "Install GitHub Copilot CLI: " + copilotInstallCommand
            : "Not supported yet"
        return {id: entry.id, label: entry.label, enabled: supported && !lockedOut, reason: reason}
    })

    function indexOf(provider) {
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].id === provider) return i
        return -1
    }

    // Chooses `provider` as a click on its entry does; false when it cannot be chosen.
    function choose(provider) {
        const index = indexOf(provider)
        if (index < 0 || !choices[index].enabled) return false
        if (provider !== selectedProvider) providerChosen(provider)
        return true
    }

    stacked: true
    label: "Provider"

    KetSelect {
        id: select
        objectName: "providerSelect"
        model: providerSelect.choices
        textRole: "label"
        currentIndex: providerSelect.indexOf(providerSelect.selectedProvider)
        Accessible.name: "Provider"
        delegate: KetMenuItem {
            required property var modelData
            required property int index
            objectName: "providerChoice_" + modelData.id
            width: ListView.view ? ListView.view.width : implicitWidth
            text: modelData.label
            description: modelData.reason
            enabled: modelData.enabled
            checkable: true
            checked: select.currentIndex === index
            highlighted: select.highlightedIndex === index
        }
        // The keyboard moves over disabled entries too: one of those keeps the provider. The
        // shown entry follows selectedProvider again either way.
        onActivated: function(index) {
            providerSelect.choose(providerSelect.choices[index].id)
            currentIndex = Qt.binding(function() {
                return providerSelect.indexOf(providerSelect.selectedProvider)
            })
        }
    }
}
