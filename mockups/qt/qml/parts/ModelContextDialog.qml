// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock

// Model & context, as KetPlus desktop's model picker: provider tabs, a model search and list,
// the reasoning effort and the auto-compact threshold. Choices are a draft until Apply.
KetDialog {
    id: dialog

    property string provider: "codex"
    // The app window, for each provider's setup state (Fixtures.providerSetup).
    property var window: null
    readonly property var draftSetup: {
        const id = window ? window.providerIssues[draftProvider] || "" : ""
        const setup = Fixtures.providerSetup[id]
        return setup && setup.blocking ? setup : null
    }
    property string model: ""
    property string effort: "Medium"
    property string draftProvider: provider
    property string draftModel: model
    property int draftEffort: 1
    property int draftCompact: 0
    readonly property var draftModels: Fixtures.providerModels[draftProvider] || []
    readonly property var shownModels: {
        const query = search.text.trim().toLowerCase()
        return draftModels.filter(entry => query === ""
            || (entry.name + " " + entry.description).toLowerCase().indexOf(query) >= 0)
    }

    signal chosen(string provider, string model, string effort)

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function providerIndex(id) {
        for (let i = 0; i < Fixtures.providers.length; ++i)
            if (Fixtures.providers[i].id === id)
                return i
        return 0
    }
    function chooseProvider(index) {
        draftProvider = Fixtures.providers[index].id
        draftModel = draftModels.length > 0 ? draftModels[0].name : ""
    }
    function restoreDefaults() {
        const defaults = Fixtures.providerModels[draftProvider] || []
        draftModel = defaults.length > 0 ? defaults[0].name : ""
        draftEffort = 1
        draftCompact = 0
    }

    title: "Model & context"
    onAboutToShow: {
        search.text = ""
        draftProvider = provider
        const listed = (Fixtures.providerModels[provider] || []).find(
            entry => entry.name === model || entry.short === model)
        draftModel = listed ? listed.name : model
        draftEffort = Math.max(0, Fixtures.efforts.indexOf(effort))
    }

    KetRouteTabs {
        Layout.fillWidth: true
        tabs: Fixtures.providers.map(entry => entry.label)
        currentIndex: dialog.providerIndex(dialog.draftProvider)
        onCurrentIndexChanged: if (Fixtures.providers[currentIndex].id !== dialog.draftProvider)
                                   dialog.chooseProvider(currentIndex)
    }

    Text {
        Layout.fillWidth: true
        topPadding: dialog.px(10)
        bottomPadding: dialog.px(12)
        text: dialog.draftSetup ? dialog.draftSetup.title + ". The conversation shows how to set it up; you can still choose a model now."
                                : Fixtures.providers[dialog.providerIndex(dialog.draftProvider)].caption
        color: dialog.draftSetup ? KetTheme.t.danger : KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: dialog.px(13)
        wrapMode: Text.Wrap
    }

    KetTextField {
        id: search
        Layout.fillWidth: true
        visible: dialog.draftModels.length > 0
        leadingIcon: "search"
        placeholderText: "Search models…"
    }

    Column {
        Layout.fillWidth: true
        topPadding: dialog.px(6)

        Repeater {
            model: dialog.shownModels

            Basic.ItemDelegate {
                id: row
                required property var modelData
                required property int index
                readonly property bool chosen: dialog.draftModel === modelData.name

                width: parent.width
                height: dialog.px(60)
                padding: 0
                hoverEnabled: true
                Accessible.name: modelData.name + ", " + modelData.description
                onClicked: dialog.draftModel = modelData.name

                background: Rectangle {
                    radius: dialog.px(6)
                    color: row.hovered ? KetTheme.t.surfaceHover : "transparent"
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        visible: row.index < dialog.shownModels.length - 1
                        color: KetTheme.t.panelBorder
                    }
                }

                contentItem: RowLayout {
                    spacing: dialog.px(10)

                    KetRadio {
                        Layout.leftMargin: dialog.px(2)
                        checked: row.chosen
                        focusPolicy: Qt.NoFocus
                        onClicked: dialog.draftModel = row.modelData.name
                        Accessible.ignored: true
                    }
                    Column {
                        Layout.fillWidth: true
                        spacing: dialog.px(3)
                        Text {
                            width: parent.width
                            text: row.modelData.name
                            color: KetTheme.t.textMain
                            font.family: KetTheme.fontFamily
                            font.pixelSize: dialog.px(15)
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: row.modelData.description
                            color: KetTheme.t.textMuted
                            font.family: KetTheme.fontFamily
                            font.pixelSize: dialog.px(13)
                            elide: Text.ElideRight
                        }
                    }
                    KetBadge {
                        Layout.rightMargin: dialog.px(2)
                        visible: row.modelData.isDefault === true
                        text: "Default"
                    }
                }
            }
        }

        KetEmptyState {
            width: parent.width
            visible: dialog.shownModels.length === 0
            iconName: dialog.draftModels.length === 0 ? "network" : "search"
            title: dialog.draftModels.length === 0 ? "Not available yet" : "No matching models"
            message: dialog.draftModels.length === 0
                         ? "KétRouter will list routed models here."
                         : "Try another name or clear the search."
        }
    }

    Rectangle { Layout.fillWidth: true; Layout.topMargin: dialog.px(6); implicitHeight: 1; color: KetTheme.t.panelBorder }

    KetStepSlider {
        Layout.fillWidth: true
        Layout.topMargin: dialog.px(16)
        Layout.bottomMargin: dialog.px(8)
        label: "Reasoning effort"
        steps: Fixtures.efforts
        currentIndex: dialog.draftEffort
        onCurrentIndexChanged: dialog.draftEffort = currentIndex
    }

    Item {
        Layout.fillWidth: true
        implicitHeight: compactHeading.implicitHeight + dialog.px(20)
        Text {
            id: compactHeading
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            text: "Auto-compact"
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: dialog.px(15)
            font.weight: Font.DemiBold
        }
        Text {
            anchors.right: parent.right
            anchors.baseline: compactHeading.baseline
            text: "500,000 model capacity"
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: dialog.px(13)
        }
    }

    Text {
        Layout.fillWidth: true
        topPadding: dialog.px(6)
        bottomPadding: dialog.px(10)
        text: "Summarize earlier messages near this threshold. The model keeps its full context capacity."
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: dialog.px(13)
        wrapMode: Text.Wrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: dialog.px(4)
        spacing: dialog.px(6)

        Repeater {
            model: Fixtures.compactChoices

            Basic.AbstractButton {
                id: choice
                required property var modelData
                required property int index
                readonly property bool selected: dialog.draftCompact === index

                Layout.fillWidth: true
                implicitHeight: dialog.px(34)
                enabled: modelData.tokens <= 500000
                hoverEnabled: true
                text: modelData.label
                Accessible.role: Accessible.RadioButton
                Accessible.checked: selected
                onClicked: dialog.draftCompact = index

                background: Rectangle {
                    radius: dialog.px(5)
                    color: choice.selected ? KetTheme.t.accentSubtle
                         : choice.hovered ? KetTheme.t.surfaceHover : "transparent"
                    border.width: 1
                    border.color: choice.visualFocus || choice.selected ? KetTheme.t.accent
                                                                        : KetTheme.t.panelBorder
                }
                contentItem: Text {
                    text: choice.text
                    color: choice.enabled ? KetTheme.t.textMain : KetTheme.t.textDisabled
                    font.family: KetTheme.fontFamily
                    font.pixelSize: dialog.px(14)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    KetTextField {
        Layout.fillWidth: true
        Layout.topMargin: dialog.px(8)
        visible: Fixtures.compactChoices[dialog.draftCompact].tokens < 0
        placeholderText: "Tokens, from 100,000 to 500,000"
    }

    Text {
        Layout.fillWidth: true
        topPadding: dialog.px(8)
        text: "This conversation · applies from the next turn"
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: dialog.px(13)
    }

    actions: [
        KetButton {
            text: "Restore defaults"
            variant: KetTheme.ButtonVariant.Ghost
            onClicked: dialog.restoreDefaults()
        },
        KetButton {
            text: "Cancel"
            variant: KetTheme.ButtonVariant.Ghost
            onClicked: dialog.reject()
        },
        KetButton {
            text: "Apply"
            variant: KetTheme.ButtonVariant.Primary
            enabled: dialog.draftModel !== ""
            onClicked: {
                const shown = Fixtures.providerModels[dialog.draftProvider].find(
                    entry => entry.name === dialog.draftModel)
                dialog.chosen(dialog.draftProvider, shown && shown.short ? shown.short : dialog.draftModel,
                               Fixtures.efforts[dialog.draftEffort])
                dialog.accept()
            }
        }
    ]
}
