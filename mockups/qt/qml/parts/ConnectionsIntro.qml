// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Effects
import Ket
import KetPlusMock

// Connections before sign-in: what connecting gives (this Mac's Agent Workspace and
// KetPlus Mobile answering the same question), the three pairing steps and who stays in
// control. The scene shows captures of the two mocks (assets/connections, see README).
Column {
    id: intro

    required property var window
    signal signInRequested()

    // Below this width the scene goes under the copy and the steps stack.
    readonly property bool narrow: width < px(860)
    readonly property string theme: KetTheme.dark ? "dark" : "light"

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }
    function asset(name) { return "qrc:/qt/qml/KetPlusMock/assets/connections/" + name + "-" + theme + ".png" }

    spacing: 0

    // A capture with rounded corners: the mask keeps the picture inside its frame.
    component Capture: Item {
        id: capture
        property alias source: picture.source
        property real radius
        // Under a title bar the top corners stay square.
        property bool squareTop: false

        Image {
            id: picture
            anchors.fill: parent
            visible: false
            fillMode: Image.PreserveAspectCrop
            horizontalAlignment: Image.AlignLeft
            verticalAlignment: Image.AlignTop
            // Decoded at the drawn width, so the large capture scales down smoothly.
            sourceSize.width: Math.ceil(capture.width * Screen.devicePixelRatio)
            smooth: true
        }
        Item {
            id: mask
            anchors.fill: parent
            visible: false
            layer.enabled: true
            layer.smooth: true
            Rectangle {
                anchors.fill: parent
                radius: capture.radius
                topLeftRadius: capture.squareTop ? 0 : capture.radius
                topRightRadius: capture.squareTop ? 0 : capture.radius
            }
        }
        MultiEffect {
            anchors.fill: parent
            source: picture
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1
        }
    }

    // Line heights are fixed, as in CSS: Qt's proportional mode multiplies the font's own
    // line spacing.
    component Muted: Text {
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: intro.px(12)
        wrapMode: Text.Wrap
        lineHeightMode: Text.FixedHeight
        lineHeight: Math.round(intro.px(12) * 1.6)
    }

    Item {
        width: parent.width
        height: intro.px(32)
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Connections"
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: intro.px(22)
            font.weight: Font.DemiBold
            font.letterSpacing: -0.5
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: intro.px(7)
            KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "disconnect"; box: intro.px(14); color: KetTheme.t.textMuted }
            Text { anchors.verticalCenter: parent.verticalCenter; text: "Not connected"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: intro.px(12) }
        }
    }

    Item { visible: intro.window.signInExpired; width: 1; height: intro.px(24) }
    KetBanner {
        visible: intro.window.signInExpired
        width: parent.width
        tone: KetTheme.BannerTone.Warning
        title: "Your login expired"
        message: "Please sign in again. Local work is preserved."
    }

    Item { width: 1; height: intro.px(intro.narrow ? 30 : 48) }

    Grid {
        id: hero
        width: parent.width
        columns: intro.narrow ? 1 : 2
        columnSpacing: intro.px(28)
        rowSpacing: intro.px(32)
        verticalItemAlignment: Grid.AlignVCenter
        readonly property real copyWidth: intro.narrow ? width : (width - columnSpacing) * 0.45

        Column {
            width: hero.copyWidth
            topPadding: intro.px(10)
            bottomPadding: intro.px(10)

            Text {
                text: "KETPLUS CONNECTIONS"
                color: KetTheme.t.accentText
                font.family: KetTheme.fontFamily
                font.pixelSize: intro.px(11)
                font.weight: Font.Medium
                font.letterSpacing: 1.4
            }
            Item { width: 1; height: intro.px(16) }
            Text {
                width: parent.width
                text: "Your workspace.\nWithin reach."
                color: KetTheme.t.textMain
                font.family: KetTheme.fontFamily
                font.pixelSize: intro.px(40)
                font.weight: Font.DemiBold
                font.letterSpacing: -1.5
                lineHeightMode: Text.FixedHeight
                lineHeight: Math.round(intro.px(40) * 1.13)
            }
            Item { width: 1; height: intro.px(20) }
            Text {
                width: Math.min(parent.width, intro.px(intro.narrow ? 480 : 380))
                text: "Continue a conversation from your phone, or connect a headless Linux machine to work in its repositories from this desktop."
                color: KetTheme.t.textSecondary
                font.family: KetTheme.fontFamily
                font.pixelSize: intro.px(14)
                wrapMode: Text.Wrap
                lineHeightMode: Text.FixedHeight
                lineHeight: Math.round(intro.px(14) * 1.7)
            }
            Item { width: 1; height: intro.px(28) }
            KetButton {
                text: "Sign in"
                iconName: "log-in"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: intro.signInRequested()
            }
            Item { width: 1; height: intro.px(11) }
            Muted { width: parent.width; text: "Use your KetSuite account to get started." }
            Item { width: 1; height: intro.px(30) }
            Row {
                width: parent.width
                spacing: intro.px(10)
                KetIcon { y: intro.px(2); name: "display"; box: intro.px(16); color: KetTheme.t.textMuted }
                Muted { width: parent.width - intro.px(26); text: "Your local workspace keeps working.\nSign in when you’re ready to connect a device." }
            }
        }

        // This Mac's Agent Workspace and KetPlus Mobile, both waiting on the same kind of
        // question: the scene the copy promises.
        Rectangle {
            id: scene
            width: intro.narrow ? Math.min(hero.width, intro.px(500)) : hero.width - hero.copyWidth - hero.columnSpacing
            height: intro.px(intro.narrow ? 380 : 400)
            radius: intro.px(16)
            clip: true
            border.color: KetTheme.t.panelBorder
            gradient: Gradient {
                GradientStop { position: 0; color: KetTheme.t.accentSubtle }
                GradientStop { position: 0.7; color: KetTheme.t.pageBg }
            }
            Accessible.role: Accessible.Graphic
            Accessible.name: "KetPlus Agent Workspace on a Mac and KetPlus Mobile on a phone"

            Item {
                id: desktop
                x: intro.px(24)
                y: intro.px(44)
                width: parent.width - intro.px(24 + 55)
                height: intro.px(286)

                KetShadow { anchors.fill: parent; radius: intro.px(9); shadowY: intro.px(14); shadowBlur: intro.px(30); shadowColor: Qt.rgba(0, 0, 0, 0.05) }
                Rectangle {
                    anchors.fill: parent
                    radius: intro.px(9)
                    color: KetTheme.t.panelBg
                }
                Rectangle {
                    id: titleBar
                    width: parent.width
                    height: intro.px(26)
                    topLeftRadius: intro.px(9)
                    topRightRadius: intro.px(9)
                    color: KetTheme.t.pageBg
                    Row {
                        x: intro.px(12)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: intro.px(4)
                        Repeater { model: 3; Rectangle { width: intro.px(6); height: width; radius: width / 2; color: KetTheme.t.borderStrong } }
                    }
                    Row {
                        anchors.centerIn: parent
                        spacing: intro.px(5)
                        KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "display"; box: intro.px(12); color: KetTheme.t.textMuted }
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "This Mac"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: intro.px(11) }
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: KetTheme.t.panelBorder }
                }
                Capture {
                    x: 1
                    y: titleBar.height
                    width: parent.width - 2
                    height: parent.height - titleBar.height - 1
                    radius: intro.px(8)
                    source: intro.asset("desktop-question")
                    squareTop: true
                }
                Rectangle {
                    anchors.fill: parent
                    radius: intro.px(9)
                    color: "transparent"
                    border.color: KetTheme.t.borderStrong
                }
            }

            Item {
                id: phone
                anchors.right: parent.right
                anchors.rightMargin: intro.px(18)
                y: intro.px(intro.narrow ? 14 : 26)
                width: intro.px(intro.narrow ? 150 : 160) + 2 * bezel
                height: (width - 2 * bezel) * 852 / 393 + 2 * bezel
                readonly property real bezel: intro.px(4)

                KetShadow { anchors.fill: parent; radius: intro.px(26); shadowY: intro.px(18); shadowBlur: intro.px(36); shadowColor: Qt.rgba(0, 0, 0, 0.16) }
                // A phone's bezel is near-black in both themes, like the hardware.
                Rectangle {
                    anchors.fill: parent
                    radius: intro.px(26)
                    color: "#111214"
                    border.color: Qt.rgba(1, 1, 1, 0.08)
                }
                Capture {
                    anchors.fill: parent
                    anchors.margins: phone.bezel
                    radius: intro.px(22)
                    source: intro.asset("mobile-question")
                }
            }

            Row {
                x: intro.px(intro.narrow ? 15 : 25)
                anchors.bottom: parent.bottom
                anchors.bottomMargin: intro.px(intro.narrow ? 12 : 23)
                spacing: intro.px(7)
                KetIcon { anchors.verticalCenter: parent.verticalCenter; name: "chats"; box: intro.px(13); color: KetTheme.t.textMuted }
                Text { anchors.verticalCenter: parent.verticalCenter; text: "Your conversations. On every screen."; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: intro.px(11) }
            }
        }
    }

    Item { width: 1; height: intro.px(54) }
    Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
    Item { width: 1; height: intro.px(28) }

    // The note sits on the heading's baseline, or under it when narrow.
    Item {
        width: parent.width
        height: intro.narrow ? stepsTitle.height + intro.px(8) + stepsNote.height : stepsTitle.height
        Text { id: stepsTitle; text: "From this Mac to your phone"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: intro.px(16); font.weight: Font.DemiBold; font.letterSpacing: -0.3 }
        Muted {
            id: stepsNote
            y: intro.narrow ? stepsTitle.height + intro.px(8) : stepsTitle.baselineOffset - baselineOffset
            width: parent.width
            horizontalAlignment: intro.narrow ? Text.AlignLeft : Text.AlignRight
            text: "Three steps. You approve the connection."
        }
    }

    Item { width: 1; height: intro.px(28) }

    Grid {
        id: steps
        width: parent.width
        columns: intro.narrow ? 1 : 3
        columnSpacing: intro.px(28)
        rowSpacing: intro.px(22)
        readonly property real cell: intro.narrow ? width : (width - 2 * columnSpacing) / 3

        Repeater {
            model: [
                { title: "Sign in on this Mac", text: "Use the same KetSuite account on both devices." },
                { title: "Scan with KetPlus Mobile", text: "Open the app on your phone and scan the pairing code." },
                { title: "Confirm, then continue", text: "Match the codes and approve your phone on this Mac." }
            ]
            Row {
                required property var modelData
                required property int index
                width: steps.cell
                spacing: intro.px(12)
                Column {
                    Rectangle { width: number.implicitWidth; height: intro.px(2); color: KetTheme.t.accent }
                    Item { width: 1; height: intro.px(6) }
                    Text { id: number; text: "0" + (index + 1); color: KetTheme.t.textMuted; font.family: KetTheme.monoFontFamily; font.pixelSize: intro.px(13) }
                }
                Column {
                    width: parent.width - number.implicitWidth - parent.spacing
                    spacing: intro.px(7)
                    topPadding: intro.px(8)
                    Text { width: parent.width; text: modelData.title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: intro.px(13); font.weight: Font.DemiBold; wrapMode: Text.Wrap }
                    Muted { width: parent.width; text: modelData.text }
                }
            }
        }
    }

    Item { width: 1; height: intro.px(28) }
    Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
    Item { width: 1; height: intro.px(16) }

    Row {
        width: parent.width
        spacing: intro.px(8)
        KetIcon { y: intro.px(2); name: "shield"; box: intro.px(15); color: KetTheme.t.textMuted }
        Muted { width: parent.width - intro.px(23); text: "You choose which devices can connect. Review or revoke their access here at any time." }
    }
}
