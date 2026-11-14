import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// New-conversation clock. Send now is the default until the user sets a time or a repeat.
Popup {
    id: popup
    objectName: "scheduleFirstMessage"
    property var bridge: null
    property bool more: false
    modal: true
    focus: true
    padding: 16
    width: Math.min(480, parent ? parent.width - 24 : 480)
    onOpened: {
        more = false
        if (bridge && !bridge.scheduleSet)
            bridge.mode = 0
    }

    contentItem: ColumnLayout {
        spacing: 8
        Label {
            text: "Schedule first message"
            font.bold: true
        }
        RadioButton {
            objectName: "scheduleSendNow"
            text: "Send now"
            checked: bridge && bridge.mode === 0
            onClicked: if (bridge) bridge.mode = 0
        }
        RadioButton {
            text: "At a time"
            checked: bridge && bridge.mode === 1
            onClicked: if (bridge) bridge.mode = 1
        }
        GridLayout {
            visible: bridge && bridge.mode === 1
            columns: 2
            columnSpacing: 8
            TextField {
                objectName: "scheduleDate"
                Layout.fillWidth: true
                placeholderText: "YYYY-MM-DD"
                text: bridge ? bridge.date : ""
                onEditingFinished: if (bridge) bridge.date = text
            }
            TextField {
                objectName: "scheduleTime"
                Layout.fillWidth: true
                placeholderText: "HH:mm"
                text: bridge ? bridge.time : ""
                onEditingFinished: if (bridge) bridge.time = text
            }
            TextField {
                objectName: "scheduleZone"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                placeholderText: "Timezone"
                text: bridge ? bridge.zone : ""
                onEditingFinished: if (bridge) bridge.zone = text
            }
        }
        RadioButton {
            text: "Repeat"
            checked: bridge && bridge.mode === 2
            onClicked: if (bridge) bridge.mode = 2
        }
        ColumnLayout {
            visible: bridge && bridge.mode === 2
            spacing: 8
            ComboBox {
                objectName: "scheduleRepeat"
                Layout.fillWidth: true
                model: ["Every day", "Weekdays", "Weekly", "Every N hours"]
                onActivated: {
                    const kinds = ["daily", "weekdays", "weekly", "interval"]
                    if (bridge)
                        bridge.repeatKind = kinds[currentIndex]
                }
            }
            TextField {
                placeholderText: "HH:mm"
                text: bridge ? bridge.time : ""
                onEditingFinished: if (bridge) bridge.time = text
            }
            TextField {
                visible: bridge && bridge.repeatKind === "interval"
                placeholderText: "YYYY-MM-DD"
                text: bridge ? bridge.date : ""
                onEditingFinished: if (bridge) bridge.date = text
            }
            TextField {
                visible: bridge && bridge.repeatKind === "interval"
                placeholderText: "Every N hours"
                text: bridge ? String(bridge.everyHours) : "4"
                onEditingFinished: if (bridge) bridge.everyHours = parseInt(text, 10) || 0
            }
            Flow {
                visible: bridge && bridge.repeatKind === "weekly"
                spacing: 4
                Repeater {
                    model: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
                    CheckBox {
                        required property string modelData
                        required property int index
                        readonly property string key: ["mon", "tue", "wed", "thu", "fri", "sat", "sun"][index]
                        text: modelData
                        checked: bridge && bridge.weekDays.indexOf(key) >= 0
                        onToggled: {
                            if (!bridge)
                                return
                            const days = bridge.weekDays.slice()
                            const at = days.indexOf(key)
                            if (checked && at < 0)
                                days.push(key)
                            if (!checked && at >= 0)
                                days.splice(at, 1)
                            bridge.weekDays = days
                        }
                    }
                }
            }
            CheckBox {
                text: "New conversation each run"
                checked: bridge && bridge.newEachRun
                onToggled: if (bridge) bridge.newEachRun = checked
            }
        }
        Button {
            visible: bridge && bridge.mode > 0
            text: popup.more ? "Less" : "More"
            flat: true
            onClicked: popup.more = !popup.more
        }
        ColumnLayout {
            visible: popup.more && bridge && bridge.mode > 0
            Label { text: "When busy" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Queue until finished", "Skip this run"]
                onActivated: if (bridge) bridge.busyPolicy = currentIndex === 0 ? "queue" : "skip"
            }
            Label { text: "When missed" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Catch up once when the computer wakes", "Skip"]
                onActivated: if (bridge) bridge.missedPolicy = currentIndex === 0 ? "run_once" : "skip"
            }
        }
        Label {
            visible: text.length > 0
            text: bridge ? bridge.lastError : ""
            color: "firebrick"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button {
                text: "Cancel"
                flat: true
                onClicked: popup.close()
            }
            Button {
                objectName: "scheduleApply"
                text: bridge && bridge.mode === 0 ? "Use Send now" : "Set schedule"
                onClicked: {
                    if (bridge && bridge.apply())
                        popup.close()
                }
            }
        }
    }
}
