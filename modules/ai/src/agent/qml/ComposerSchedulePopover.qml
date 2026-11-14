import Ket
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

// New-conversation clock. Send now is the default until the user sets a time or a repeat.
KetPopover {
    id: popup
    objectName: "scheduleFirstMessage"
    property var bridge: null
    property bool more: false
    title: "Schedule first message"
    width: Math.min(KetTheme.px(480, KetTheme.zoom), parent ? parent.width - KetTheme.px(24, KetTheme.zoom) : KetTheme.px(480, KetTheme.zoom))
    padding: KetTheme.px(16, KetTheme.zoom)
    modal: true
    focus: true
    closePolicy: Basic.Popup.CloseOnEscape | Basic.Popup.CloseOnPressOutside

    function bindIndex(control, read) {
        control.currentIndex = Qt.binding(read)
    }

    onOpened: {
        more = false
        if (bridge && !bridge.scheduleSet)
            bridge.mode = 0
        bindIndex(repeatPicker, () => bridge ? bridge.repeatIndex : 0)
        bindIndex(atZone, () => bridge ? bridge.zoneIndex : 0)
        bindIndex(repeatZone, () => bridge ? bridge.zoneIndex : 0)
        bindIndex(busyPicker, () => bridge ? bridge.busyIndex : 0)
        bindIndex(missedPicker, () => bridge ? bridge.missedIndex : 0)
    }

    Flickable {
        id: scroll
        width: parent.width
        height: Math.min(contentHeight, Math.max(KetTheme.px(180, KetTheme.zoom), (popup.parent ? popup.parent.height : 900) - KetTheme.px(210, KetTheme.zoom)))
        contentHeight: fields.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Basic.ScrollBar.vertical: Basic.ScrollBar {}

        Column {
            id: fields
            width: scroll.width
            spacing: KetTheme.px(10, KetTheme.zoom)

            KetRadio {
                objectName: "scheduleSendNow"
                text: "Send now"
                checked: bridge && bridge.mode === 0
                onClicked: if (bridge) bridge.mode = 0
            }
            KetRadio {
                text: "At a time"
                checked: bridge && bridge.mode === 1
                onClicked: if (bridge) bridge.mode = 1
            }
            Column {
                visible: bridge && bridge.mode === 1
                width: parent.width
                spacing: KetTheme.px(8, KetTheme.zoom)
                Flow {
                    width: parent.width
                    spacing: KetTheme.px(6, KetTheme.zoom)
                    KetButton {
                        text: "Tonight at 22:00"
                        onClicked: if (bridge) bridge.applyPreset("tonight")
                    }
                    KetButton {
                        text: "Tomorrow at 08:00"
                        onClicked: if (bridge) bridge.applyPreset("tomorrow")
                    }
                    KetButton {
                        text: "Custom"
                        onClicked: dateField.forceActiveFocus()
                    }
                }
                KetFieldRow {
                    width: parent.width
                    label: "Date"
                    help: "YYYY-MM-DD"
                    KetTextField {
                        id: dateField
                        objectName: "scheduleDate"
                        width: parent.width
                        text: bridge ? bridge.date : ""
                        onEditingFinished: if (bridge) bridge.date = text
                    }
                }
                KetFieldRow {
                    width: parent.width
                    label: "Time"
                    KetTextField {
                        objectName: "scheduleTime"
                        width: parent.width
                        text: bridge ? bridge.time : ""
                        onEditingFinished: if (bridge) bridge.time = text
                    }
                }
                KetFieldRow {
                    width: parent.width
                    label: "Time zone"
                    KetSelect {
                        id: atZone
                        objectName: "scheduleZone"
                        width: parent.width
                        model: bridge ? bridge.zones : []
                        onActivated: index => { if (bridge) bridge.zoneIndex = index }
                    }
                }
                Text {
                    width: parent.width
                    text: bridge ? bridge.atTimeHint : ""
                    wrapMode: Text.Wrap
                    color: KetTheme.t.textSecondary
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.px(12, KetTheme.zoom)
                }
            }
            KetRadio {
                text: "Repeat"
                checked: bridge && bridge.mode === 2
                onClicked: if (bridge) bridge.mode = 2
            }
            Column {
                visible: bridge && bridge.mode === 2
                width: parent.width
                spacing: KetTheme.px(8, KetTheme.zoom)
                KetFieldRow {
                    width: parent.width
                    label: "Repeat"
                    KetSelect {
                        id: repeatPicker
                        objectName: "scheduleRepeat"
                        width: parent.width
                        model: ["Every day", "Weekdays", "Weekly", "Every N hours"]
                        onActivated: index => { if (bridge) bridge.repeatIndex = index }
                    }
                }
                Flow {
                    visible: bridge && bridge.repeatKind === "weekly"
                    width: parent.width
                    spacing: KetTheme.px(4, KetTheme.zoom)
                    Repeater {
                        model: [
                            { key: "mon", label: "Mon" },
                            { key: "tue", label: "Tue" },
                            { key: "wed", label: "Wed" },
                            { key: "thu", label: "Thu" },
                            { key: "fri", label: "Fri" },
                            { key: "sat", label: "Sat" },
                            { key: "sun", label: "Sun" }
                        ]
                        KetButton {
                            required property var modelData
                            text: modelData.label
                            variant: bridge && bridge.weekDays.indexOf(modelData.key) >= 0
                                      ? KetTheme.ButtonVariant.Primary
                                      : KetTheme.ButtonVariant.Secondary
                            onClicked: {
                                if (!bridge)
                                    return
                                const days = bridge.weekDays.slice()
                                const at = days.indexOf(modelData.key)
                                if (at < 0)
                                    days.push(modelData.key)
                                else
                                    days.splice(at, 1)
                                bridge.weekDays = days
                            }
                        }
                    }
                }
                KetFieldRow {
                    width: parent.width
                    visible: bridge && bridge.repeatKind === "interval"
                    label: "Every N hours"
                    KetTextField {
                        width: parent.width
                        text: bridge ? String(bridge.everyHours) : "4"
                        onEditingFinished: if (bridge) bridge.everyHours = parseInt(text, 10) || 0
                    }
                }
                KetFieldRow {
                    width: parent.width
                    visible: bridge && bridge.repeatKind === "interval"
                    label: "Anchor date"
                    KetTextField {
                        width: parent.width
                        text: bridge ? bridge.date : ""
                        onEditingFinished: if (bridge) bridge.date = text
                    }
                }
                KetFieldRow {
                    width: parent.width
                    label: "Time"
                    KetTextField {
                        width: parent.width
                        text: bridge ? bridge.time : ""
                        onEditingFinished: if (bridge) bridge.time = text
                    }
                }
                KetFieldRow {
                    width: parent.width
                    label: "Time zone"
                    KetSelect {
                        id: repeatZone
                        width: parent.width
                        model: bridge ? bridge.zones : []
                        onActivated: index => { if (bridge) bridge.zoneIndex = index }
                    }
                }
                KetCheckbox {
                    text: "New conversation each run"
                    checked: bridge && bridge.newEachRun
                    onToggled: if (bridge) bridge.newEachRun = checked
                }
                Text {
                    width: parent.width
                    text: bridge ? bridge.liveSummary : ""
                    wrapMode: Text.Wrap
                    color: KetTheme.t.accentText
                    font.family: KetTheme.fontFamily
                    font.pixelSize: KetTheme.px(12, KetTheme.zoom)
                }
            }
            KetButton {
                visible: bridge && bridge.mode > 0
                text: popup.more ? "Less" : "More"
                variant: KetTheme.ButtonVariant.Ghost
                onClicked: popup.more = !popup.more
            }
            Column {
                visible: popup.more && bridge && bridge.mode > 0
                width: parent.width
                spacing: KetTheme.px(8, KetTheme.zoom)
                KetFieldRow {
                    width: parent.width
                    label: "When busy"
                    KetSelect {
                        id: busyPicker
                        width: parent.width
                        model: ["Queue until finished", "Skip this run"]
                        onActivated: index => { if (bridge) bridge.busyIndex = index }
                    }
                }
                KetFieldRow {
                    width: parent.width
                    label: "When missed"
                    KetSelect {
                        id: missedPicker
                        width: parent.width
                        model: ["Catch up once when the computer wakes", "Skip"]
                        onActivated: index => { if (bridge) bridge.missedIndex = index }
                    }
                }
            }
            Text {
                visible: text.length > 0
                width: parent.width
                text: bridge ? bridge.lastError : ""
                wrapMode: Text.Wrap
                color: KetTheme.t.danger
                font.family: KetTheme.fontFamily
                font.pixelSize: KetTheme.px(13, KetTheme.zoom)
            }
        }
    }

    footer: [
        KetButton {
            text: "Cancel"
            variant: KetTheme.ButtonVariant.Ghost
            onClicked: popup.close()
        },
        KetButton {
            objectName: "scheduleApply"
            text: bridge && bridge.mode === 0 ? "Use Send now" : "Set schedule"
            variant: KetTheme.ButtonVariant.Primary
            onClicked: {
                if (bridge && bridge.apply())
                    popup.close()
            }
        }
    ]
}
