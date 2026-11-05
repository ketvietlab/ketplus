import "ChromeMetrics.js" as Metrics
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Popup {
    id: picker
    objectName: "modelPopup"

    required property var tokens
    required property Item viewport
    required property Item anchorItem
    property var models: []
    property var efforts: []
    property string selectedModel: ""
    property string selectedEffort: ""
    property string defaultEffort: ""
    property string selectedProvider: "codex"
    property bool loading: false
    property bool available: false
    property double configurationRevision: 0
    property double draftRevision: 0
    property var compactWindow: null
    property bool compactSupported: false
    property int modelCapacity: 0
    property bool applying: false
    property string applyError: ""
    property string blockedReason: ""
    property string draftModel: ""
    property string draftEffort: ""
    readonly property bool canCustomizeCompact: compactSupported && ["codex", "claude"].indexOf(selectedProviderTab) >= 0
    readonly property int selectedCapacity: {
        for (let model of providerModels) {
            if (catalogMatches(model, draftModel) && Number(model.modelContextWindow) > 0) return Number(model.modelContextWindow)
        }
        return draftModel === selectedModel && selectedProviderTab === selectedProvider ? modelCapacity : 0
    }
    property var effortLabel: function(value) { return value }

    property string selectedProviderTab: "codex"
    // While a turn runs the conversation keeps its provider; the other tabs say when to switch.
    property string lockedProvider: ""

    readonly property bool hasClaudeModels: {
        for (let i = 0; i < models.length; ++i)
            if (models[i].provider === "claude" && !models[i].hidden) return true
        return false
    }
    readonly property bool hasCursorModels: {
        for (let i = 0; i < models.length; ++i)
            if (models[i].provider === "cursor" && !models[i].hidden) return true
        return false
    }
    readonly property string cursorInstallCommand: Qt.platform.os === "windows"
        ? "irm 'https://cursor.com/install?win32=true' | iex"
        : "curl https://cursor.com/install -fsS | bash"
    readonly property var providerModels: models.filter(function(model) {
        return model.provider === picker.selectedProviderTab && !model.hidden
    })
    readonly property var filteredModels: {
        const query = modelSearch.text.trim().toLocaleLowerCase()
        return providerModels.filter(function(model) {
            return ((model.displayName || "") + " " + model.model + " " + (model.description || ""))
                .toLocaleLowerCase().indexOf(query) !== -1
        })
    }
    function catalogMatches(entry, selected) {
        if (!entry || !selected)
            return false
        if (entry.model === selected || entry.id === selected)
            return true
        const variants = entry.efforts || []
        for (let i = 0; i < variants.length; ++i)
            if (variants[i].model === selected)
                return true
        return false
    }
    readonly property var displayedEfforts: {
        const models = providerModels
        for (let i = 0; i < models.length; ++i)
            if (catalogMatches(models[i], draftModel))
                return models[i].supportedReasoningEfforts || []
        if (models.length > 0)
            return models[0].supportedReasoningEfforts || []
        return []
    }
    readonly property int effortIndex: {
        for (let i = 0; i < displayedEfforts.length; ++i)
            if (displayedEfforts[i].reasoningEffort === draftEffort) return i
        return -1
    }

    signal modelSelected(var model)
    signal effortSelected(string effort)
    signal applyRequested(string model, string effort, string provider, var changes)
    signal refreshRequested()

    parent: Overlay.overlay
    function revealControl(item) {
        const flick = settingsScroll.contentItem
        if (!flick || flick.contentY === undefined) return
        const top = item.mapToItem(settingsBody, 0, 0).y
        const inset = Metrics.px(8, tokens)
        let offset = flick.contentY
        if (top < offset + inset) offset = top - inset
        else if (top + item.height > offset + flick.height - inset)
            offset = top + item.height - flick.height + inset
        flick.contentY = Math.max(0, Math.min(offset, Math.max(0, flick.contentHeight - flick.height)))
    }
    width: Math.min(Metrics.px(500, tokens), viewport.width - Metrics.px(32, tokens))
    height: Math.min(settingsBody.implicitHeight + Metrics.px(116, tokens) + 2, viewport.height - Metrics.px(32, tokens))
    x: Math.round((viewport.width - width) / 2)
    y: Math.round((viewport.height - height) / 2)
    padding: 0
    focus: true
    modal: true
    dim: true
    closePolicy: applying ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        modelSearch.clear()
        draftRevision = configurationRevision
        draftModel = selectedModel
        draftEffort = selectedEffort
        compactControl.reset(compactWindow)
        let tab = selectedProvider || "codex"
        let matchedPreferred = false
        for (let i = 0; i < models.length; ++i) {
            if (models[i].provider === tab && catalogMatches(models[i], selectedModel)) {
                matchedPreferred = true
                break
            }
        }
        if (!matchedPreferred && selectedModel) {
            for (let i = 0; i < models.length; ++i) {
                if (catalogMatches(models[i], selectedModel) && models[i].provider) {
                    tab = models[i].provider
                    break
                }
            }
        }
        if (tab === "claude" && !hasClaudeModels) tab = "codex"
        if (tab === "cursor" && !hasCursorModels) tab = "codex"
        if (lockedProvider) tab = lockedProvider
        selectedProviderTab = tab
        syncCurrentModel()
        modelSearch.forceActiveFocus()
    }
    onApplyErrorChanged: { if (applyError && !applying) draftRevision = configurationRevision }
    onSelectedModelChanged: syncCurrentModel()
    onClosed: anchorItem.forceActiveFocus()

    Overlay.modal: Rectangle { color: picker.tokens.overlay || "#66000000" }
    background: Rectangle {
        radius: Metrics.px(10, tokens)
        color: picker.tokens.surfaceRaised
        border.width: 1
        border.color: picker.tokens.borderStrong
    }

    function chooseDraft(model) {
        if (applying || blockedReason) return
        draftModel = model.model || model.id || ""
        draftEffort = model.defaultReasoningEffort || ""
        syncCurrentModel()
    }

    function chooseCurrentModel() {
        const model = filteredModels[modelList.currentIndex]
        if (available && !loading && model) chooseDraft(model)
    }

    function syncCurrentModel() {
        let selected = -1
        for (let i = 0; i < filteredModels.length; ++i)
            if (catalogMatches(filteredModels[i], draftModel)) selected = i
        modelList.currentIndex = selected >= 0 ? selected : filteredModels.length > 0 ? 0 : -1
    }

    function restoreDefaults() {
        let defaultModel = providerModels.length > 0 ? providerModels[0] : null
        for (let model of providerModels)
            if (model.isDefault) defaultModel = model
        if (defaultModel) chooseDraft(defaultModel)
        else if (defaultEffort) draftEffort = defaultEffort
        compactControl.reset(null)
        modelSearch.clear()
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(58, tokens)
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(14, tokens)
            spacing: Metrics.px(8, tokens)

            Text {
                Layout.fillWidth: true
                text: qsTr("Model & context")
                color: picker.tokens.textMain
                font.family: picker.tokens.uiFont
                font.pixelSize: picker.tokens.title
                font.weight: Font.DemiBold
            }
            Basic.ToolButton {
                id: closeButton
                objectName: "closeModelPickerButton"
                Layout.preferredWidth: Metrics.px(34, tokens)
                Layout.preferredHeight: Metrics.px(34, tokens)
                padding: Metrics.px(9, tokens)
                contentItem: TintedIcon {
                    source: "qrc:/agent/icons/plus.svg"
                    rotation: 45
                    color: picker.tokens.textMuted
                }
                background: Rectangle {
                    radius: Metrics.px(6, tokens)
                    color: closeButton.hovered || closeButton.visualFocus
                           ? picker.tokens.surfaceHover : "transparent"
                }
                enabled: !picker.applying
                onClicked: picker.close()
                Accessible.name: "Close model picker"
                KetToolTip {
                    tokens: picker.tokens
                    visible: closeButton.hovered
                    text: "Close"
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: picker.tokens.border }

        Basic.ScrollView {
            id: settingsScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                id: settingsBody
                width: settingsScroll.availableWidth
                spacing: 0
        Row {
            id: providerGrid
            objectName: "providerTabs"
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(28, tokens)
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            Layout.topMargin: Metrics.px(12, tokens)
            spacing: Metrics.px(22, tokens)

            Repeater {
                model: [
                    {id: "codex", label: "Codex"},
                    {id: "claude", label: "Claude"},
                    {id: "cursor", label: "Cursor"},
                    {id: "ketrouter", label: "K\u00e9tRouter"}
                ]
                Basic.ToolButton {
                    id: providerButton
                    required property var modelData
                    objectName: "providerTab_" + modelData.id
                    width: contentItem.implicitWidth
                    height: Metrics.px(28, tokens)
                    readonly property bool supported: modelData.id === "codex"
                        || (modelData.id === "claude" && picker.hasClaudeModels)
                        || (modelData.id === "cursor" && picker.hasCursorModels)
                    readonly property bool lockedOut: supported && picker.lockedProvider !== "" && picker.lockedProvider !== modelData.id
                    enabled: supported && !lockedOut
                    padding: 0
                    Accessible.name: modelData.label
                    Accessible.description: enabled ? (picker.selectedProviderTab === modelData.id ? "Current provider" : "Select provider") : lockedOut ? "Switch to " + modelData.label + " after this turn" : modelData.id === "cursor" ? "Install Cursor CLI, then retry" : "Not supported yet"
                    contentItem: Text {
                        text: providerButton.modelData.label
                        color: providerButton.enabled ? picker.tokens.tabActiveText : picker.tokens.textMuted
                        opacity: providerButton.enabled ? 1 : 0.62
                        font.family: picker.tokens.uiFont
                        font.pixelSize: picker.tokens.body
                        font.weight: providerButton.enabled && picker.selectedProviderTab === providerButton.modelData.id ? Font.DemiBold : Font.Normal
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Item {
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Metrics.px(2, tokens)
                            visible: providerButton.enabled && picker.selectedProviderTab === providerButton.modelData.id
                            color: picker.tokens.accent
                        }
                    }
                    KetToolTip {
                        tokens: picker.tokens
                        visible: providerButton.hovered
                        text: providerButton.enabled ? providerButton.modelData.label
                              : providerButton.lockedOut ? "Switch to " + providerButton.modelData.label + " after this turn"
                              : providerButton.modelData.id === "cursor"
                                ? "Install Cursor CLI: " + picker.cursorInstallCommand
                              : providerButton.modelData.label + ": not supported yet"
                    }
                    onClicked: {
                        if (!providerButton.enabled) return
                        picker.selectedProviderTab = providerButton.modelData.id
                        if (picker.providerModels.length > 0) picker.chooseDraft(picker.providerModels[0])
                        picker.syncCurrentModel()
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            Layout.topMargin: Metrics.px(7, tokens)
            text: picker.selectedProviderTab === "claude"
                  ? "Claude uses the local Agent SDK sidecar."
                  : picker.selectedProviderTab === "cursor"
                    ? (picker.hasCursorModels
                       ? "Cursor lists models from the installed CLI (`agent models`)."
                       : "Install Cursor CLI: " + picker.cursorInstallCommand)
                  : picker.hasClaudeModels && picker.hasCursorModels
                    ? "Codex is ready. Claude and Cursor are available."
                  : picker.hasClaudeModels
                    ? "Codex is ready. Claude is available."
                  : picker.hasCursorModels
                    ? "Codex is ready. Cursor is available."
                    : "Codex is ready. Other providers are not supported yet."
            color: picker.tokens.textMuted
            font.family: picker.tokens.uiFont
            font.pixelSize: picker.tokens.caption
            elide: Text.ElideRight
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(36, tokens)
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            Layout.topMargin: Metrics.px(14, tokens)
            Layout.bottomMargin: Metrics.px(12, tokens)
            radius: Metrics.px(5, tokens)
            color: picker.tokens.interactiveBackground
            border.width: 1
            border.color: modelSearch.activeFocus ? picker.tokens.focus : picker.tokens.borderStrong

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Metrics.px(10, tokens)
                anchors.rightMargin: Metrics.px(6, tokens)
                spacing: Metrics.px(8, tokens)
                TintedIcon {
                    Layout.preferredWidth: Metrics.px(15, tokens)
                    Layout.preferredHeight: Metrics.px(15, tokens)
                    source: "qrc:/agent/icons/search.svg"
                    color: picker.tokens.textMuted
                }
                Basic.TextField {
                    id: modelSearch
                    objectName: "modelSearch"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    placeholderText: "Search models..."
                    placeholderTextColor: picker.tokens.textMuted
                    color: picker.tokens.textMain
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                    padding: 0
                    selectByMouse: true
                    background: Item {}
                    Accessible.name: "Search Codex models"
                    Keys.onDownPressed: {
                        if (modelList.count > 0) {
                            modelList.currentIndex = Math.max(0, modelList.currentIndex)
                            modelList.forceActiveFocus()
                        }
                    }
                    onAccepted: picker.chooseCurrentModel()
                }
                Basic.ToolButton {
                    id: clearSearch
                    objectName: "clearModelSearchButton"
                    Layout.preferredWidth: Metrics.px(26, tokens)
                    Layout.preferredHeight: Metrics.px(26, tokens)
                    visible: modelSearch.text.length > 0
                    padding: Metrics.px(7, tokens)
                    contentItem: TintedIcon {
                        source: "qrc:/agent/icons/plus.svg"
                        rotation: 45
                        color: picker.tokens.textMuted
                    }
                    background: Rectangle {
                        radius: Metrics.px(5, tokens)
                        color: clearSearch.hovered ? picker.tokens.surfaceHover : "transparent"
                    }
                    onClicked: {
                        modelSearch.clear()
                        modelSearch.forceActiveFocus()
                    }
                    Accessible.name: "Clear model search"
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(Math.max(60, Math.min(180, picker.filteredModels.length * 60)), tokens)
            Layout.minimumHeight: Metrics.px(48, tokens)
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)

            ListView {
                id: modelList
                objectName: "modelPickerList"
                anchors.fill: parent
                clip: true
                spacing: 0
                visible: !picker.loading && picker.available
                model: picker.filteredModels
                currentIndex: -1
                keyNavigationEnabled: true
                keyNavigationWraps: true
                onModelChanged: picker.syncCurrentModel()
                Keys.onReturnPressed: picker.chooseCurrentModel()
                Keys.onEnterPressed: picker.chooseCurrentModel()
                Keys.onSpacePressed: picker.chooseCurrentModel()

                delegate: Basic.ItemDelegate {
                    id: modelRow
                    required property var modelData
                    required property int index
                    objectName: "modelOption_" + modelData.model
                    width: ListView.view.width
                    height: Metrics.px(60, tokens)
                    padding: 0
                    highlighted: picker.catalogMatches(modelData, picker.draftModel)
                    Accessible.name: modelData.displayName || modelData.model

                    contentItem: RowLayout {
                        spacing: Metrics.px(11, tokens)
                        Rectangle {
                            Layout.preferredWidth: Metrics.px(14, tokens)
                            Layout.preferredHeight: Metrics.px(14, tokens)
                            radius: Metrics.px(7, tokens)
                            color: "transparent"
                            border.width: 1
                            border.color: modelRow.highlighted
                                          ? picker.tokens.tabActiveText : picker.tokens.borderStrong
                            Rectangle {
                                anchors.centerIn: parent
                                width: Metrics.px(6, tokens)
                                height: Metrics.px(6, tokens)
                                radius: Metrics.px(3, tokens)
                                visible: modelRow.highlighted
                                color: picker.tokens.tabActiveText
                            }
                        }
                        Column {
                            Layout.fillWidth: true
                            spacing: Metrics.px(3, tokens)
                            Text {
                                width: parent.width
                                text: modelRow.modelData.displayName || modelRow.modelData.model
                                elide: Text.ElideRight
                                color: picker.tokens.textMain
                                font.family: picker.tokens.uiFont
                                font.pixelSize: picker.tokens.body
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: modelRow.modelData.description || modelRow.modelData.model
                                elide: Text.ElideRight
                                color: picker.tokens.textMuted
                                font.family: picker.tokens.uiFont
                                font.pixelSize: picker.tokens.micro
                            }
                        }
                        Rectangle {
                            visible: modelRow.index === 0
                            Layout.preferredWidth: Metrics.px(50, tokens)
                            Layout.preferredHeight: Metrics.px(22, tokens)
                            radius: Metrics.px(4, tokens)
                            color: picker.tokens.interactiveBackground
                            border.color: picker.tokens.border
                            Text {
                                anchors.centerIn: parent
                                text: "Default"
                                color: picker.tokens.textSecondary
                                font.family: picker.tokens.uiFont
                                font.pixelSize: picker.tokens.micro
                            }
                        }
                    }
                    background: Item {
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: picker.tokens.border
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: Metrics.px(5, tokens)
                            color: modelRow.hovered ? picker.tokens.surfaceHover : "transparent"
                            border.color: modelRow.visualFocus || (modelList.activeFocus && modelRow.ListView.isCurrentItem)
                                          ? picker.tokens.focus : "transparent"
                        }
                    }
                    onClicked: {
                        modelList.currentIndex = index
                        picker.chooseDraft(modelData)
                    }
                    KetToolTip {
                        tokens: picker.tokens
                        visible: modelRow.hovered
                        text: modelData.description || modelData.model
                    }
                }
                ScrollBar.vertical: Basic.ScrollBar { policy: ScrollBar.AsNeeded }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width
                spacing: Metrics.px(8, tokens)
                visible: picker.loading || !picker.available || picker.filteredModels.length === 0
                Text {
                    objectName: "modelPickerEmptyState"
                    Layout.fillWidth: true
                    text: !picker.available ? "Codex CLI unavailable" : picker.loading ? "Loading models..."
                          : picker.providerModels.length === 0 ? "Models unavailable" : "No models found"
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: picker.tokens.textMuted
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                }
                Basic.Button {
                    id: retryModels
                    objectName: "retryModelsButton"
                    Layout.alignment: Qt.AlignHCenter
                    visible: picker.available && !picker.loading
                    text: picker.providerModels.length === 0 ? "Retry" : "Clear search"
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                    implicitHeight: Metrics.px(32, tokens)
                    horizontalPadding: Metrics.px(12, tokens)
                    contentItem: Text {
                        text: retryModels.text
                        font: retryModels.font
                        color: picker.tokens.textMain
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: Metrics.px(5, tokens)
                        color: retryModels.hovered ? picker.tokens.surfaceHover : picker.tokens.interactiveBackground
                        border.color: retryModels.visualFocus ? picker.tokens.focus : picker.tokens.borderStrong
                    }
                    onClicked: {
                        modelSearch.clear()
                        if (picker.providerModels.length === 0) picker.refreshRequested()
                        else modelSearch.forceActiveFocus()
                    }
                }
            }
        }

        Rectangle {
            visible: picker.displayedEfforts.length > 1
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: picker.tokens.border
        }

        ColumnLayout {
            visible: picker.displayedEfforts.length > 1
            Layout.fillWidth: true
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            Layout.topMargin: Metrics.px(14, tokens)
            Layout.bottomMargin: Metrics.px(14, tokens)
            spacing: Metrics.px(7, tokens)

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Reasoning effort"
                    color: picker.tokens.textMain
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                    font.weight: Font.DemiBold
                }
                Text {
                    objectName: "selectedEffortLabel"
                    text: picker.draftEffort ? picker.effortLabel(picker.draftEffort) : "Default"
                    color: picker.tokens.tabActiveText
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.caption
                }
            }

            Basic.Slider {
                id: effortSlider
                objectName: "effortSlider"
                Layout.fillWidth: true
                Layout.preferredHeight: Metrics.px(24, tokens)
                from: 0
                to: Math.max(1, picker.displayedEfforts.length - 1)
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: Math.max(0, picker.effortIndex)
                enabled: picker.available && !picker.loading && !picker.applying && !picker.blockedReason && picker.displayedEfforts.length > 1
                onMoved: {
                    const index = Math.round(value)
                    if (index >= 0 && index < picker.displayedEfforts.length)
                        picker.draftEffort = picker.displayedEfforts[index].reasoningEffort
                }
                background: Rectangle {
                    x: effortSlider.leftPadding
                    y: (effortSlider.height - height) / 2
                    width: effortSlider.availableWidth
                    height: Metrics.px(4, tokens)
                    radius: Metrics.px(2, tokens)
                    color: picker.tokens.interactiveActive
                    border.color: effortSlider.visualFocus ? picker.tokens.focus : picker.tokens.borderStrong
                    Rectangle {
                        height: parent.height
                        width: effortSlider.position * parent.width
                        radius: Metrics.px(2, tokens)
                        color: picker.tokens.accent
                        visible: effortSlider.enabled && picker.effortIndex >= 0
                    }
                }
                handle: Rectangle {
                    objectName: "effortHandle"
                    x: effortSlider.leftPadding + effortSlider.visualPosition * (effortSlider.availableWidth - width)
                    y: (effortSlider.height - height) / 2
                    width: Metrics.px(16, tokens)
                    height: Metrics.px(16, tokens)
                    radius: Metrics.px(8, tokens)
                    visible: picker.effortIndex >= 0
                    color: effortSlider.enabled ? picker.tokens.accent : picker.tokens.textDisabled
                    border.color: picker.tokens.surfaceRaised
                    border.width: 2
                }
                Accessible.name: "Reasoning effort"
                Accessible.description: picker.effortLabel(picker.draftEffort)
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Metrics.px(14, tokens)
                Repeater {
                    model: picker.displayedEfforts
                    Text {
                        required property var modelData
                        required property int index
                        objectName: "effortTickLabel_" + index
                        readonly property real tickCenter: picker.displayedEfforts.length <= 1
                            ? parent.width / 2
                            : 8 + index * (parent.width - 16) / (picker.displayedEfforts.length - 1)
                        x: Math.max(0, Math.min(parent.width - width, tickCenter - width / 2))
                        anchors.verticalCenter: parent.verticalCenter
                        text: picker.effortLabel(modelData.reasoningEffort)
                        color: picker.tokens.textMuted
                        font.family: picker.tokens.uiFont
                        font.pixelSize: picker.tokens.micro
                    }
                }
            }
        }

        CompactWindowControl {
            id: compactControl
            Layout.fillWidth: true
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            Layout.topMargin: Metrics.px(16, tokens)
            Layout.bottomMargin: Metrics.px(16, tokens)
            tokens: picker.tokens
            onFocusRequested: (item) => picker.revealControl(item)
            supported: picker.canCustomizeCompact
            capacity: picker.selectedCapacity
            blocked: picker.applying || picker.blockedReason.length > 0
            blockedReason: picker.blockedReason
        }
        Text {
            objectName: "modelConfigurationError"
            Layout.fillWidth: true
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            Layout.bottomMargin: Metrics.px(12, tokens)
            visible: picker.applyError.length > 0
            text: picker.applyError
            wrapMode: Text.Wrap
            color: picker.tokens.danger
            font.family: picker.tokens.uiFont
            font.pixelSize: picker.tokens.caption
            Accessible.role: Accessible.AlertMessage
        }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: picker.tokens.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Metrics.px(58, tokens)
            Layout.leftMargin: Metrics.px(22, tokens)
            Layout.rightMargin: Metrics.px(22, tokens)
            spacing: Metrics.px(8, tokens)
            Item { Layout.fillWidth: true }
            Basic.Button {
                id: resetEffort
                objectName: "resetEffortButton"
                text: "Restore defaults"
                enabled: picker.available && !picker.loading &&
                         (compactControl.requested !== null || (picker.defaultEffort.length > 0 && picker.defaultEffort !== picker.draftEffort) ||
                          (picker.providerModels.length > 0 && picker.providerModels[0].model !== picker.draftModel))
                implicitHeight: Metrics.px(34, tokens)
                horizontalPadding: Metrics.px(12, tokens)
                contentItem: Text {
                    text: resetEffort.text
                    color: resetEffort.enabled ? picker.tokens.textMain : picker.tokens.textDisabled
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Metrics.px(5, tokens)
                    color: resetEffort.hovered ? picker.tokens.surfaceHover : "transparent"
                }
                onClicked: picker.restoreDefaults()
            }
            Basic.Button {
                id: cancelButton
                objectName: "cancelModelButton"
                text: qsTr("Cancel")
                enabled: !picker.applying
                implicitHeight: Metrics.px(34, tokens)
                horizontalPadding: Metrics.px(12, tokens)
                contentItem: Text {
                    text: cancelButton.text
                    color: picker.tokens.textMain
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: Metrics.px(5, tokens); color: cancelButton.hovered ? picker.tokens.surfaceHover : "transparent" }
                onClicked: picker.close()
            }
            Basic.Button {
                id: applyButton
                objectName: "applyModelButton"
                text: picker.applying ? qsTr("Applying…") : picker.applyError ? qsTr("Retry") : qsTr("Apply")
                enabled: picker.available && !picker.loading && !picker.applying && !picker.blockedReason && compactControl.valid && picker.draftModel.length > 0
                implicitHeight: Metrics.px(34, tokens)
                horizontalPadding: Metrics.px(15, tokens)
                contentItem: Text {
                    text: applyButton.text
                    color: "#ffffff"
                    font.family: picker.tokens.uiFont
                    font.pixelSize: picker.tokens.body
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Metrics.px(5, tokens)
                    color: applyButton.hovered ? picker.tokens.accentHover : picker.tokens.accent
                }
                onClicked: picker.applyRequested(picker.draftModel, picker.draftEffort, picker.selectedProviderTab,
                    picker.compactSupported ? {compactWindow: compactControl.requested, expected_revision: picker.draftRevision} : {expected_revision: picker.draftRevision})
            }
        }
    }
}
