// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock

// Connections: this Mac, paired phones and trusted Linux machines (SSH or the KetPlus
// Relay), and the account. Pairing and trust are deterministic prototype states.
Item {
    id: screen

    required property var window
    property var phones: [{ name: "iPhone 16", detail: "KetPlus App · Paired" }]
    property var machines: [{ name: "build-node-01", detail: "Ubuntu 24.04 LTS · Ready for remote work", via: "Relay", online: true }]
    property bool historyOpen: false
    property int linuxMode: 0

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    component SectionTitle: Item {
        property string title
        property string count
        width: parent.width
        height: screen.px(28)
        Text { anchors.verticalCenter: parent.verticalCenter; text: title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(17); font.weight: Font.Medium }
        Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: count; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
    }

    component DeviceCard: Rectangle {
        default property alias trailing: trailingRow.data
        property string iconName
        property string name
        property string detail
        property var badges: []
        width: parent.width
        height: screen.px(94)
        radius: screen.px(7)
        color: KetTheme.t.panelBg
        border.color: KetTheme.t.panelBorder
        Rectangle {
            x: screen.px(22)
            anchors.verticalCenter: parent.verticalCenter
            width: screen.px(40); height: width; radius: screen.px(7)
            color: KetTheme.t.panelBgSubtle
            border.color: KetTheme.t.panelBorder
            KetIcon { anchors.centerIn: parent; name: iconName; box: screen.px(18); color: KetTheme.t.textSecondary }
        }
        Column {
            x: screen.px(80)
            anchors.verticalCenter: parent.verticalCenter
            spacing: screen.px(6)
            Row {
                spacing: screen.px(8)
                Text { anchors.verticalCenter: parent.verticalCenter; text: name; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14); font.weight: Font.Medium }
                Repeater { model: badges; KetBadge { required property var modelData; text: modelData.text; tone: modelData.tone } }
            }
            Text { text: detail; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
        }
        Row { id: trailingRow; anchors.right: parent.right; anchors.rightMargin: screen.px(22); anchors.verticalCenter: parent.verticalCenter; spacing: screen.px(8) }
    }

    Basic.ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            x: Math.max(screen.px(42), (scroll.availableWidth - width) / 2)
            y: screen.px(38)
            width: Math.min(scroll.availableWidth - screen.px(84), screen.px(1000))
            spacing: screen.px(18)

            PageHeading {
                width: parent.width
                title: "Connections"
                description: "Phones that access this Mac and Linux machines this desktop can use."
                KetButton {
                    id: addButton
                    text: "Add"
                    iconName: "plus"
                    variant: KetTheme.ButtonVariant.Primary
                    onClicked: addMenu.popup(addButton, addButton.width - addMenu.width, addButton.height + screen.px(4))
                }
            }

            Item { width: 1; height: screen.px(28) }

            Row {
                spacing: screen.px(38)
                Rectangle {
                    width: screen.px(84); height: width; radius: screen.px(10)
                    color: KetTheme.t.accentSubtle
                    border.color: KetTheme.t.accentBorder
                    KetIcon { anchors.centerIn: parent; name: "display"; box: screen.px(40); color: KetTheme.t.accentText }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: screen.px(10)
                    Text { text: "This Mac"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(22); font.weight: Font.Medium }
                    Text { text: "Ready for your connected devices"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14) }
                    KetBadge { text: "Connected"; tone: KetTheme.BadgeTone.Success }
                }
            }

            Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }

            SectionTitle { title: "Phones & tablets"; count: screen.phones.length + " paired" }
            KetEmptyState {
                visible: screen.phones.length === 0
                width: parent.width
                iconName: "smartphone"
                title: "No phone paired"
                message: "Pair the KetPlus App to follow and steer conversations from your phone."
                actionText: "Pair a phone"
                onActionTriggered: pairDialog.open()
            }
            Repeater {
                model: screen.phones
                DeviceCard {
                    required property var modelData
                    required property int index
                    iconName: "smartphone"
                    name: modelData.name
                    detail: modelData.detail
                    KetButton { text: "Details"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.window.notify("Last seen 2 minutes ago · iOS 26") }
                    KetButton {
                        text: "Revoke access"
                        iconName: "wifi-off"
                        variant: KetTheme.ButtonVariant.GhostDanger
                        onClicked: {
                            screen.phones = screen.phones.filter((p, i) => i !== index)
                            screen.window.notify("Revoked " + modelData.name)
                        }
                    }
                }
            }

            SectionTitle { title: "Linux machines"; count: screen.machines.length + " trusted" }
            Repeater {
                model: screen.machines
                DeviceCard {
                    required property var modelData
                    required property int index
                    iconName: "terminal"
                    name: modelData.name
                    detail: modelData.detail
                    badges: [{ text: modelData.online ? "Connected" : "Offline", tone: modelData.online ? KetTheme.BadgeTone.Success : KetTheme.BadgeTone.Neutral },
                             { text: modelData.via, tone: KetTheme.BadgeTone.Neutral }]
                    KetButton { text: "Details"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.window.notify("Fingerprint SHA256:9f2c…a41e verified") }
                    KetButton {
                        text: modelData.online ? "Disconnect" : "Connect"
                        iconName: "disconnect"
                        variant: KetTheme.ButtonVariant.Ghost
                        onClicked: {
                            const next = screen.machines.slice()
                            next[index] = Object.assign({}, modelData, { online: !modelData.online })
                            screen.machines = next
                        }
                    }
                }
            }
            Row {
                spacing: screen.px(8)
                KetIcon { name: "shield"; box: screen.px(13); color: KetTheme.t.textMuted; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "Linux machines connect over your SSH access (free) or through the KetPlus Relay (Pro). Verify the host fingerprint or pairing code before trusting one."; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
            }

            Item {
                width: parent.width
                height: screen.px(64)
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: screen.px(12)
                    KetAvatar { name: "An Nguyen"; anchors.verticalCenter: parent.verticalCenter }
                    Column {
                        Text { text: "Demo account"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                        Text { text: "Personal workspace"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                    }
                }
                KetButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: "Sign out"; iconName: "log-out"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.window.notify("Signed out of the demo account") }
            }

            KetButton {
                text: (screen.historyOpen ? "▾  " : "▸  ") + "About your history"
                variant: KetTheme.ButtonVariant.Ghost
                controlSize: KetTheme.ControlSize.Small
                onClicked: screen.historyOpen = !screen.historyOpen
            }
            Text {
                visible: screen.historyOpen
                width: parent.width
                wrapMode: Text.Wrap
                text: "Conversations, transcripts and files stay on this Mac. A paired phone reads them through an end-to-end encrypted session; the relay sees only ciphertext."
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: screen.px(13)
                lineHeight: 1.4
            }
            Item { width: 1; height: screen.px(40) }
        }
    }

    KetMenu {
        id: addMenu
        KetMenuItem { text: "Pair a phone or tablet"; description: "Scan a code with the KetPlus App"; iconName: "smartphone"; onTriggered: pairDialog.open() }
        KetMenuItem { text: "Connect a Linux machine"; description: "Over SSH or the KetPlus Relay"; iconName: "terminal"; onTriggered: linuxDialog.open() }
    }

    KetDialog {
        id: pairDialog
        title: "Pair a phone"
        Row {
            spacing: screen.px(20)
            Grid {
                columns: 9
                Repeater {
                    model: 81
                    Rectangle {
                        required property int index
                        width: screen.px(14); height: width
                        color: ((index * 7919) % 13 < 6 || [0, 1, 2, 9, 18, 6, 7, 8, 17, 26, 54, 63, 72, 73, 74].includes(index)) ? KetTheme.t.textMain : KetTheme.t.panelBg
                    }
                }
            }
            Column {
                spacing: screen.px(10)
                Text { text: "Open KetPlus on your phone and scan the code,\nor type the pairing code."; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); lineHeight: 1.4 }
                Text { text: "K7Q2 · 9XMD"; color: KetTheme.t.textMain; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(22) }
                KetBadge { text: "Waiting for your phone…"; tone: KetTheme.BadgeTone.Info }
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: pairDialog.reject() },
            KetButton {
                text: "Simulate pairing"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    screen.phones = screen.phones.concat([{ name: "iPad Air", detail: "KetPlus App · Paired just now" }])
                    pairDialog.accept()
                    screen.window.notify("iPad Air paired")
                }
            }
        ]
    }

    KetDialog {
        id: linuxDialog
        title: "Connect a Linux machine"
        KetSegmentedControl {
            options: ["SSH (free)", "KetPlus Relay (Pro)"]
            currentIndex: screen.linuxMode
            onActivated: index => screen.linuxMode = index
        }
        KetFieldRow {
            width: parent.width
            label: screen.linuxMode === 0 ? "Host" : "Pairing code"
            help: screen.linuxMode === 0 ? "Uses your ~/.ssh/config and agent. KetPlus installs its daemon in ~/.ketplus."
                                         : "Run ketplusctl pair on the machine and type the code it shows."
            KetTextField { width: parent.width; placeholderText: screen.linuxMode === 0 ? "build-node-02.local" : "ABCD-1234" }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: linuxDialog.reject() },
            KetButton {
                text: "Verify and trust"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    screen.machines = screen.machines.concat([{ name: "build-node-02", detail: "Debian 13 · Ready for remote work", via: screen.linuxMode === 0 ? "SSH" : "Relay", online: true }])
                    linuxDialog.accept()
                    screen.window.notify("build-node-02 trusted")
                }
            }
        ]
    }
}
