import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

ColumnLayout {
    id: control
    objectName: "compactWindowControl"

    property bool supported: false
    property bool blocked: false
    property string blockedReason: ""
    property int capacity: 0
    property int preset: 0
    property bool custom: false
    property string customText: ""
    signal focusRequested(var item)
    readonly property int maximum: capacity > 0 ? Math.min(1000000, capacity) : 1000000
    readonly property var requested: {
        if (!supported || (!custom && preset === 0)) return null
        if (!custom) return preset
        const value = customText.trim()
        return /^[0-9]+$/.test(value) ? Number(value) : -1
    }
    readonly property bool valid: requested === null ||
        (Number.isInteger(requested) && requested >= 100000 && requested <= maximum)
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Auto-compact")
    spacing: KetTheme.px(8, KetTheme.zoom)

    function moveChoice(index, direction) {
        for (let step = 1; step <= choiceRepeater.count; ++step) {
            const next = (index + direction * step + choiceRepeater.count) % choiceRepeater.count
            const button = choiceRepeater.itemAt(next)
            if (button && button.enabled) { button.clicked(); button.forceActiveFocus(); return }
        }
    }

    function reset(value) {
        preset = Number(value) > 0 ? Number(value) : 0
        custom = preset > 0 && [272000, 500000, 1000000].indexOf(preset) < 0
        customText = preset > 0 ? String(preset) : ""
    }

    RowLayout {
        Layout.fillWidth: true
        Text {
            text: qsTr("Auto-compact")
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
            font.weight: Font.DemiBold
        }
        Text {
            Layout.fillWidth: true
            text: control.capacity > 0
                ? qsTr("%1 model capacity").arg(Number(control.capacity).toLocaleString(Qt.locale("en_US"), "f", 0))
                : qsTr("Model capacity not reported")
            horizontalAlignment: Text.AlignRight
            wrapMode: Text.Wrap
            color: KetTheme.t.textMuted
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
        }
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Summarize earlier messages near this threshold. The model keeps its full context capacity.")
        wrapMode: Text.Wrap
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
    }
    GridLayout {
        Layout.fillWidth: true
        columns: width >= KetTheme.px(400, KetTheme.zoom) ? 5 : 3
        columnSpacing: KetTheme.px(8, KetTheme.zoom)
        rowSpacing: KetTheme.px(8, KetTheme.zoom)
        visible: control.supported
        Repeater {
            id: choiceRepeater
            model: [
                {value: 0, label: qsTr("Auto")}, {value: 272000, label: "272k"},
                {value: 500000, label: "500k"}, {value: 1000000, label: "1M"},
                {value: -1, label: qsTr("Custom")}
            ]
            Basic.Button {
                id: choice
                required property int index
                required property var modelData
                objectName: "compactWindow_" + modelData.value
                onActiveFocusChanged: if (activeFocus) control.focusRequested(choice)
                Layout.fillWidth: true
                Layout.preferredHeight: KetTheme.px(36, KetTheme.zoom)
                readonly property bool selected: modelData.value === -1 ? control.custom
                    : !control.custom && control.preset === modelData.value
                enabled: !control.blocked && (modelData.value < 0 || modelData.value <= control.maximum)
                padding: KetTheme.px(6, KetTheme.zoom)
                contentItem: Text {
                    text: choice.modelData.label
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: choice.enabled ? KetTheme.t.textMain : KetTheme.t.textDisabled
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
                    font.weight: choice.selected ? Font.DemiBold : Font.Normal
                }
                background: Rectangle {
                    radius: KetTheme.px(5, KetTheme.zoom)
                    color: choice.selected ? KetTheme.t.accentSubtle
                        : choice.hovered ? KetTheme.t.surfaceHover : KetTheme.t.surfaceRaised
                    border.color: choice.visualFocus ? KetTheme.t.colorFocus
                        : choice.selected ? KetTheme.t.accent : KetTheme.t.panelBorder
                    border.width: 1
                }
                Keys.onRightPressed: control.moveChoice(index, 1)
                Keys.onLeftPressed: control.moveChoice(index, -1)
                Keys.onDownPressed: control.moveChoice(index, 1)
                Keys.onUpPressed: control.moveChoice(index, -1)
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                Accessible.checked: selected
                onClicked: {
                    if (modelData.value === -1) { control.custom = true; customInput.forceActiveFocus() }
                    else { control.preset = modelData.value; control.custom = false }
                }
                KetToolTip { objectName: "ketToolTip"; visible: choice.hovered; text: choice.modelData.label }
            }
        }
    }
    Basic.TextField {
        id: customInput
        objectName: "compactWindowInput"
        onActiveFocusChanged: if (activeFocus) control.focusRequested(customInput)
        Layout.fillWidth: true
        Layout.preferredHeight: KetTheme.px(36, KetTheme.zoom)
        visible: control.supported && control.custom
        enabled: !control.blocked
        text: control.customText
        onTextEdited: control.customText = text
        placeholderText: qsTr("100000–1000000 tokens")
        inputMethodHints: Qt.ImhDigitsOnly
        selectByMouse: true
        color: KetTheme.t.textMain
        placeholderTextColor: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.buttonRecipe(KetTheme.dark, KetTheme.zoom, KetTheme.ButtonVariant.Secondary, KetTheme.ControlSize.Medium, false, false, true, false).fontPixels
        padding: KetTheme.px(8, KetTheme.zoom)
        Accessible.name: qsTr("Compact window in tokens")
        background: Rectangle {
            radius: KetTheme.px(5, KetTheme.zoom)
            color: KetTheme.t.surfaceRaised
            border.width: 1
            border.color: customInput.activeFocus ? KetTheme.t.colorFocus : KetTheme.t.panelBorder
        }
    }
    Text {
        objectName: "compactWindowValidation"
        Layout.fillWidth: true
        visible: control.supported && !control.valid
        text: qsTr("Enter a whole token count from 100,000 to 1,000,000, within the reported model capacity.")
        wrapMode: Text.Wrap
        color: KetTheme.t.danger
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
        Accessible.role: Accessible.AlertMessage
    }
    Text {
        Layout.fillWidth: true
        visible: !control.supported
        text: qsTr("Provider default. This runtime does not support a custom compact window.")
        wrapMode: Text.Wrap
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
    }
    Text {
        Layout.fillWidth: true
        text: control.blockedReason || qsTr("This conversation · applies from the next turn")
        wrapMode: Text.Wrap
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: KetTheme.assistantMessageRecipe(KetTheme.dark, KetTheme.zoom).timePixels
    }
}
