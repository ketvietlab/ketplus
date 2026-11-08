import Ket
import QtQuick
import Ket as Kit
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Popup {
    id: picker
    objectName: "modelPopup"
    readonly property var look: Kit.KetTheme.dialogRecipe(Kit.KetTheme.dark, Kit.KetTheme.zoom)

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
    // Row titles and supporting text follow the mock's Model & context dialog.
    readonly property real titlePixels: KetTheme.px(15, KetTheme.zoom)
    readonly property real bodyPixels: KetTheme.px(13, KetTheme.zoom)

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
        const inset = KetTheme.px(8, KetTheme.zoom)
        let offset = flick.contentY
        if (top < offset + inset) offset = top - inset
        else if (top + item.height > offset + flick.height - inset)
            offset = top + item.height - flick.height + inset
        flick.contentY = Math.max(0, Math.min(offset, Math.max(0, flick.contentHeight - flick.height)))
    }
    width: Math.min(KetTheme.px(500, KetTheme.zoom), viewport.width - KetTheme.px(32, KetTheme.zoom))
    height: Math.min(settingsBody.implicitHeight + KetTheme.px(116, KetTheme.zoom) + 2, viewport.height - KetTheme.px(32, KetTheme.zoom))
    x: (viewport.width - width) / 2
    y: (viewport.height - height) / 2
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

    Overlay.modal: Rectangle { color: picker.look.backdrop }
    background: Rectangle {
        radius: picker.look.radius
        color: picker.look.background
        border.width: 1
        border.color: picker.look.border
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
            Layout.preferredHeight: KetTheme.px(58, KetTheme.zoom)
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(14, KetTheme.zoom)
            spacing: KetTheme.px(8, KetTheme.zoom)

            Text {
                Layout.fillWidth: true
                text: qsTr("Model & context")
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: picker.look.titlePixels
                font.weight: picker.look.titleWeight
            }
            Basic.ToolButton {
                id: closeButton
                objectName: "closeModelPickerButton"
                Layout.preferredWidth: KetTheme.px(34, KetTheme.zoom)
                Layout.preferredHeight: KetTheme.px(34, KetTheme.zoom)
                padding: KetTheme.px(9, KetTheme.zoom)
                contentItem: Item {
                    implicitWidth: KetTheme.px(16, KetTheme.zoom)
                    implicitHeight: implicitWidth
                    KetIcon {
                        anchors.centerIn: parent
                        box: Math.min(parent.width, parent.height)
                        name: KetTheme.iconForLucide("plus")
                        rotation: 45
                        color: KetTheme.t.textMuted
                    }
                }
                background: Rectangle {
                    radius: KetTheme.px(6, KetTheme.zoom)
                    color: closeButton.hovered || closeButton.visualFocus
                           ? KetTheme.t.surfaceHover : "transparent"
                }
                enabled: !picker.applying
                onClicked: picker.close()
                Accessible.name: "Close model picker"
                KetToolTip {
                    objectName: "ketToolTip"
                    visible: closeButton.hovered
                    text: "Close"
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: KetTheme.t.panelBorder }

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
        ModelProviderTabs {
            Layout.fillWidth: true
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.topMargin: KetTheme.px(22, KetTheme.zoom)
            selectedProvider: picker.selectedProviderTab
            lockedProvider: picker.lockedProvider
            hasClaudeModels: picker.hasClaudeModels
            hasCursorModels: picker.hasCursorModels
            cursorInstallCommand: picker.cursorInstallCommand
            onProviderChosen: (provider) => {
                picker.selectedProviderTab = provider
                if (picker.providerModels.length > 0) picker.chooseDraft(picker.providerModels[0])
                picker.syncCurrentModel()
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.topMargin: KetTheme.px(10, KetTheme.zoom)
            Layout.bottomMargin: KetTheme.px(2, KetTheme.zoom)
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
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: picker.bodyPixels
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: picker.look.paddingInline
            Layout.rightMargin: picker.look.paddingInline
            Layout.topMargin: KetTheme.px(14, KetTheme.zoom)
            Layout.bottomMargin: KetTheme.px(12, KetTheme.zoom)
            Kit.KetTextField {
                id: modelSearch
                objectName: "modelSearch"
                Layout.fillWidth: true
                leadingIcon: Kit.KetTheme.iconForLucide("search")
                placeholderText: "Search models…"
                Accessible.name: "Search models"
                Keys.onDownPressed: {
                    if (modelList.count > 0) {
                        modelList.currentIndex = Math.max(0, modelList.currentIndex)
                        modelList.forceActiveFocus()
                    }
                }
                onAccepted: picker.chooseCurrentModel()
            }
            Kit.KetIconButton {
                objectName: "clearModelSearchButton"
                iconName: Kit.KetTheme.iconForLucide("x")
                text: "Clear model search"
                visible: modelSearch.text.length > 0
                onClicked: { modelSearch.clear(); modelSearch.forceActiveFocus() }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: KetTheme.px(Math.max(60, Math.min(180, picker.filteredModels.length * 60)), KetTheme.zoom)
            Layout.minimumHeight: KetTheme.px(48, KetTheme.zoom)
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)

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
                    height: KetTheme.px(60, KetTheme.zoom)
                    padding: 0
                    highlighted: picker.catalogMatches(modelData, picker.draftModel)
                    Accessible.name: modelData.displayName || modelData.model

                    contentItem: RowLayout {
                        spacing: KetTheme.px(10, KetTheme.zoom)
                        KetRadio {
                            Layout.leftMargin: KetTheme.px(2, KetTheme.zoom)
                            // The row owns the choice; a click must not break this binding.
                            checkable: false
                            checked: modelRow.highlighted
                            focusPolicy: Qt.NoFocus
                            onClicked: modelRow.clicked()
                            Accessible.ignored: true
                        }
                        Column {
                            Layout.fillWidth: true
                            spacing: KetTheme.px(3, KetTheme.zoom)
                            Text {
                                width: parent.width
                                text: modelRow.modelData.displayName || modelRow.modelData.model
                                elide: Text.ElideRight
                                color: KetTheme.t.textMain
                                font.family: KetTheme.fontFamily
                                font.pixelSize: picker.titlePixels
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                text: modelRow.modelData.description || modelRow.modelData.model
                                elide: Text.ElideRight
                                color: KetTheme.t.textMuted
                                font.family: KetTheme.fontFamily
                                font.pixelSize: picker.bodyPixels
                            }
                        }
                        Rectangle {
                            visible: modelRow.index === 0
                            Layout.preferredWidth: KetTheme.px(50, KetTheme.zoom)
                            Layout.preferredHeight: KetTheme.px(22, KetTheme.zoom)
                            radius: KetTheme.px(4, KetTheme.zoom)
                            color: KetTheme.t.interactiveBg
                            border.color: KetTheme.t.panelBorder
                            Text {
                                anchors.centerIn: parent
                                text: "Default"
                                color: KetTheme.t.textSecondary
                                font.family: KetTheme.fontFamily
                                font.pixelSize: KetTheme.dayDividerRecipe(KetTheme.dark, KetTheme.zoom).fontPixels
                            }
                        }
                    }
                    background: Item {
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            visible: modelRow.index < modelList.count - 1
                            color: KetTheme.t.panelBorder
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: KetTheme.px(5, KetTheme.zoom)
                            color: modelRow.hovered ? KetTheme.t.surfaceHover : "transparent"
                            border.color: modelRow.visualFocus || (modelList.activeFocus && modelRow.ListView.isCurrentItem)
                                          ? KetTheme.t.colorFocus : "transparent"
                        }
                    }
                    onClicked: {
                        modelList.currentIndex = index
                        picker.chooseDraft(modelData)
                    }
                    KetToolTip {
                        objectName: "ketToolTip"
                        visible: modelRow.hovered
                        text: modelData.description || modelData.model
                    }
                }
                ScrollBar.vertical: Basic.ScrollBar { policy: ScrollBar.AsNeeded }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width
                spacing: KetTheme.px(8, KetTheme.zoom)
                visible: picker.loading || !picker.available || picker.filteredModels.length === 0
                Text {
                    objectName: "modelPickerEmptyState"
                    Layout.fillWidth: true
                    text: !picker.available ? "Codex CLI unavailable" : picker.loading ? "Loading models..."
                          : picker.providerModels.length === 0 ? "Models unavailable" : "No models found"
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: KetTheme.t.textMuted
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                }
                Basic.Button {
                    id: retryModels
                    objectName: "retryModelsButton"
                    Layout.alignment: Qt.AlignHCenter
                    visible: picker.available && !picker.loading
                    text: picker.providerModels.length === 0 ? "Retry" : "Clear search"
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                    implicitHeight: KetTheme.px(32, KetTheme.zoom)
                    horizontalPadding: KetTheme.px(12, KetTheme.zoom)
                    contentItem: Text {
                        text: retryModels.text
                        font: retryModels.font
                        color: KetTheme.t.textMain
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: KetTheme.px(5, KetTheme.zoom)
                        color: retryModels.hovered ? KetTheme.t.surfaceHover : KetTheme.t.interactiveBg
                        border.color: retryModels.visualFocus ? KetTheme.t.colorFocus : KetTheme.t.borderStrong
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
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.topMargin: KetTheme.px(6, KetTheme.zoom)
            Layout.preferredHeight: 1
            color: KetTheme.t.panelBorder
        }

        ColumnLayout {
            visible: picker.displayedEfforts.length > 1
            Layout.fillWidth: true
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.topMargin: KetTheme.px(14, KetTheme.zoom)
            Layout.bottomMargin: KetTheme.px(14, KetTheme.zoom)
            spacing: KetTheme.px(7, KetTheme.zoom)

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Reasoning effort"
                    color: effortSlider.recipe.label
                    font.family: KetTheme.fontFamily
                    font.pixelSize: effortSlider.recipe.labelPixels
                    font.weight: effortSlider.recipe.labelWeight
                }
                Text {
                    objectName: "selectedEffortLabel"
                    text: picker.draftEffort ? picker.effortLabel(picker.draftEffort) : "Default"
                    color: effortSlider.recipe.value
                    font.family: KetTheme.fontFamily
                    font.pixelSize: effortSlider.recipe.labelPixels
                }
            }

            Basic.Slider {
                id: effortSlider
                objectName: "effortSlider"
                Layout.fillWidth: true
                Layout.preferredHeight: KetTheme.px(24, KetTheme.zoom)
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
                // Drawn from ket::sliderRecipe like KetSlider; the handle hides until an effort is chosen.
                readonly property var recipe: KetTheme.sliderRecipe(KetTheme.dark, KetTheme.zoom, hovered, pressed, enabled, visualFocus)
                hoverEnabled: true
                padding: 0
                opacity: recipe.opacity
                background: Item {
                    x: effortSlider.leftPadding + effortSlider.recipe.handleSize / 2
                    width: effortSlider.availableWidth - effortSlider.recipe.handleSize
                    height: effortSlider.height
                    Rectangle {
                        y: (parent.height - height) / 2
                        width: parent.width
                        height: effortSlider.recipe.trackHeight
                        radius: height / 2
                        color: effortSlider.recipe.track
                    }
                    Rectangle {
                        y: (parent.height - height) / 2
                        width: effortSlider.visualPosition * parent.width
                        height: effortSlider.recipe.trackHeight
                        radius: height / 2
                        color: effortSlider.recipe.fill
                        visible: picker.effortIndex >= 0
                    }
                }
                handle: Item {
                    objectName: "effortHandle"
                    x: effortSlider.leftPadding + effortSlider.visualPosition * (effortSlider.availableWidth - width)
                    y: (effortSlider.height - height) / 2
                    width: effortSlider.recipe.handleSize
                    height: width
                    visible: picker.effortIndex >= 0
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -2
                        visible: effortSlider.recipe.focusRing.a > 0
                        radius: width / 2
                        color: "transparent"
                        border.width: 2
                        border.color: effortSlider.recipe.focusRing
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: effortSlider.recipe.handle
                        border.width: effortSlider.recipe.handleBorderWidth
                        border.color: effortSlider.recipe.handleBorder
                    }
                }
                Accessible.name: "Reasoning effort"
                Accessible.description: picker.effortLabel(picker.draftEffort)
            }

            // Only the range ends are named, as KetStepSlider does; the header shows the current step.
            Item {
                readonly property var recipe: KetTheme.sliderRecipe(KetTheme.dark, KetTheme.zoom, false, false, true, false)
                Layout.fillWidth: true
                Layout.preferredHeight: effortMinLabel.implicitHeight
                Text {
                    id: effortMinLabel
                    objectName: "effortMinLabel"
                    text: picker.displayedEfforts.length > 1 ? picker.effortLabel(picker.displayedEfforts[0].reasoningEffort) : ""
                    color: parent.recipe.rangeLabel
                    font.family: KetTheme.fontFamily
                    font.pixelSize: parent.recipe.rangePixels
                }
                Text {
                    objectName: "effortMaxLabel"
                    anchors.right: parent.right
                    text: picker.displayedEfforts.length > 1 ? picker.effortLabel(picker.displayedEfforts[picker.displayedEfforts.length - 1].reasoningEffort) : ""
                    color: parent.recipe.rangeLabel
                    font.family: KetTheme.fontFamily
                    font.pixelSize: parent.recipe.rangePixels
                }
            }
        }

        CompactWindowControl {
            id: compactControl
            Layout.fillWidth: true
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.topMargin: KetTheme.px(16, KetTheme.zoom)
            Layout.bottomMargin: KetTheme.px(16, KetTheme.zoom)

            onFocusRequested: (item) => picker.revealControl(item)
            supported: picker.canCustomizeCompact
            capacity: picker.selectedCapacity
            blocked: picker.applying || picker.blockedReason.length > 0
            blockedReason: picker.blockedReason
        }
        Text {
            objectName: "modelConfigurationError"
            Layout.fillWidth: true
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.bottomMargin: KetTheme.px(12, KetTheme.zoom)
            visible: picker.applyError.length > 0
            text: picker.applyError
            wrapMode: Text.Wrap
            color: KetTheme.t.danger
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
            Accessible.role: Accessible.AlertMessage
        }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: KetTheme.t.panelBorder }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: KetTheme.px(58, KetTheme.zoom)
            Layout.leftMargin: KetTheme.px(22, KetTheme.zoom)
            Layout.rightMargin: KetTheme.px(22, KetTheme.zoom)
            spacing: KetTheme.px(8, KetTheme.zoom)
            Item { Layout.fillWidth: true }
            Basic.Button {
                id: resetEffort
                objectName: "resetEffortButton"
                text: "Restore defaults"
                enabled: picker.available && !picker.loading &&
                         (compactControl.requested !== null || (picker.defaultEffort.length > 0 && picker.defaultEffort !== picker.draftEffort) ||
                          (picker.providerModels.length > 0 && picker.providerModels[0].model !== picker.draftModel))
                implicitHeight: KetTheme.px(34, KetTheme.zoom)
                horizontalPadding: KetTheme.px(12, KetTheme.zoom)
                contentItem: Text {
                    text: resetEffort.text
                    color: resetEffort.enabled ? KetTheme.t.textMain : KetTheme.t.textDisabled
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: KetTheme.px(5, KetTheme.zoom)
                    color: resetEffort.hovered ? KetTheme.t.surfaceHover : "transparent"
                }
                onClicked: picker.restoreDefaults()
            }
            Basic.Button {
                id: cancelButton
                objectName: "cancelModelButton"
                text: qsTr("Cancel")
                enabled: !picker.applying
                implicitHeight: KetTheme.px(34, KetTheme.zoom)
                horizontalPadding: KetTheme.px(12, KetTheme.zoom)
                contentItem: Text {
                    text: cancelButton.text
                    color: KetTheme.t.textMain
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: KetTheme.px(5, KetTheme.zoom); color: cancelButton.hovered ? KetTheme.t.surfaceHover : "transparent" }
                onClicked: picker.close()
            }
            Basic.Button {
                id: applyButton
                objectName: "applyModelButton"
                text: picker.applying ? qsTr("Applying…") : picker.applyError ? qsTr("Retry") : qsTr("Apply")
                enabled: picker.available && !picker.loading && !picker.applying && !picker.blockedReason && compactControl.valid && picker.draftModel.length > 0
                implicitHeight: KetTheme.px(34, KetTheme.zoom)
                horizontalPadding: KetTheme.px(15, KetTheme.zoom)
                contentItem: Text {
                    text: applyButton.text
                    color: KetTheme.t.actionPrimaryInk
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: KetTheme.px(5, KetTheme.zoom)
                    color: applyButton.pressed ? KetTheme.t.actionPrimaryActive
                           : applyButton.hovered ? KetTheme.t.actionPrimaryHover : KetTheme.t.actionPrimaryBg
                }
                onClicked: picker.applyRequested(picker.draftModel, picker.draftEffort, picker.selectedProviderTab,
                    picker.compactSupported ? {compactWindow: compactControl.requested, expected_revision: picker.draftRevision} : {expected_revision: picker.draftRevision})
            }
        }
    }
}
