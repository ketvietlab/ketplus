// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock
import KetPlusMock.Native

// Settings: Appearance (live theme and zoom), Typography, Account & devices, Providers &
// browser, Archived and Updates.
Item {
    id: screen

    required property var window
    readonly property var tabs: [
        { id: "appearance", text: "Appearance", icon: "palette" },
        { id: "typography", text: "Typography", icon: "text-size" },
        { id: "account", text: "Account & devices", icon: "display" },
        { id: "providers", text: "Providers & browser", icon: "sliders" },
        { id: "archived", text: "Archived", icon: "archive" },
        { id: "updates", text: "Updates", icon: "download" }
    ]
    readonly property var current: tabs.find(t => t.id === window.settingsTab) || tabs[0]

    function px(value) { return KetTheme.px(value, KetTheme.zoom) }

    component SubHeading: Text {
        color: KetTheme.t.textMain
        font.family: KetTheme.fontFamily
        font.pixelSize: screen.px(15)
        font.weight: Font.Medium
    }
    component Divider: Rectangle { width: parent.width; height: 1; color: KetTheme.t.panelBorder }
    component Body: Text {
        width: parent.width
        wrapMode: Text.Wrap
        color: KetTheme.t.textSecondary
        font.family: KetTheme.fontFamily
        font.pixelSize: screen.px(14)
    }

    Basic.ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth

        Column {
            x: screen.px(42)
            y: screen.px(38)
            width: scroll.availableWidth - 2 * x
            spacing: screen.px(32)

            PageHeading {
                width: parent.width
                eyebrow: "MAKE IT YOUR OWN"
                title: "Settings"
                description: "One consistent look across your entire workspace."
                KetKbd { text: "⌘ ," }
            }

            Row {
                width: parent.width
                spacing: screen.px(28)

                Column {
                    width: screen.px(184)
                    spacing: screen.px(4)
                    Repeater {
                        model: screen.tabs
                        KetNavItem {
                            required property var modelData
                            width: parent.width
                            text: modelData.text
                            iconName: modelData.icon
                            checkable: false
                            checked: screen.current.id === modelData.id
                            onClicked: screen.window.showScreen(modelData.id === "appearance" ? "settings" : "settings/" + modelData.id)
                        }
                    }
                }

                Rectangle { width: 1; height: content.height; color: KetTheme.t.panelBorder }

                Column {
                    id: content
                    width: Math.min(parent.width - screen.px(242), screen.px(730))
                    spacing: screen.px(20)

                    Text { text: screen.current.text; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(22); font.weight: Font.Medium }

                    Loader {
                        width: parent.width
                        sourceComponent: ({ appearance: appearance, typography: typography, account: account,
                                            providers: providers, archived: archived, updates: updates })[screen.current.id]
                    }
                }
            }
            Item { width: 1; height: screen.px(20) }
        }
    }

    Component {
        id: appearance
        Column {
            spacing: screen.px(20)
            Body { text: "Choose a theme that suits your lighting and helps you focus." }
            Basic.ButtonGroup { id: themes }
            Row {
                width: parent.width
                spacing: screen.px(14)
                KetThemeCard {
                    width: screen.px(264)
                    text: "Light"
                    checked: !Mock.dark
                    Basic.ButtonGroup.group: themes
                    onClicked: Mock.setDark(false)
                }
                KetThemeCard {
                    width: screen.px(264)
                    text: "Dark"
                    previewDark: true
                    checked: Mock.dark
                    Basic.ButtonGroup.group: themes
                    onClicked: Mock.setDark(true)
                }
            }
            KetCheckbox {
                text: "Follow system appearance"
                checked: Mock.followSystem
                onToggled: Mock.setFollowSystem(checked)
            }
            Divider {}
            SubHeading { text: "Accent color" }
            Item {
                width: parent.width
                height: screen.px(40)
                Row {
                    spacing: screen.px(10)
                    anchors.verticalCenter: parent.verticalCenter
                    KetAccentSwatch { anchors.verticalCenter: parent.verticalCenter }
                    Column {
                        Text { text: "Indigo"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13); font.weight: Font.Medium }
                        Text { text: "The signature color of Két Design System"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                    }
                }
                KetIcon { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; name: "check"; box: screen.px(14); color: KetTheme.t.accentText }
            }
            Divider {}
            SubHeading { text: "Interface zoom" }
            KetSegmentedControl {
                options: ["90%", "100%", "110%", "125%"]
                currentIndex: [0.9, 1, 1.1, 1.25].indexOf(Mock.zoom)
                onActivated: index => Mock.setZoom([0.9, 1, 1.1, 1.25][index])
            }
            Divider {}
            SubHeading { text: "Workspace preview" }
            Rectangle {
                width: parent.width
                height: preview.implicitHeight
                radius: screen.px(7)
                color: KetTheme.t.panelBg
                border.color: KetTheme.t.panelBorder
                clip: true
                Column {
                    id: preview
                    width: parent.width
                    KetTabBar {
                        width: parent.width
                        KetTabButton { text: "NavigationRail.qml"; iconName: "file-code"; width: implicitWidth }
                        KetTabButton { text: "README.md"; width: implicitWidth }
                    }
                    KetCodeBlock {
                        width: parent.width
                        language: "qml"
                        code: "// Focus on what you are creating.\nproperty string workspace: \"develop\"\ncolor: theme.accentSubtle"
                    }
                    Item {
                        width: parent.width
                        height: screen.px(44)
                        Row {
                            x: screen.px(16)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: screen.px(8)
                            Image { source: "qrc:/qt/qml/KetPlusMock/assets/ketplus-mark.svg"; width: screen.px(18); height: width; sourceSize: Qt.size(width * 2, height * 2) }
                            Text { text: "Ready for your next idea."; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                        }
                        KetBadge { anchors.right: parent.right; anchors.rightMargin: screen.px(16); anchors.verticalCenter: parent.verticalCenter; text: "Completed"; tone: KetTheme.BadgeTone.Success }
                    }
                }
            }
            Body { text: "Applies to the interface, editor, diffs, Markdown, and terminal."; font.pixelSize: screen.px(13) }
            Row {
                spacing: screen.px(8)
                KetButton { text: "Restore defaults"; variant: KetTheme.ButtonVariant.Ghost; onClicked: { Mock.setDark(false); Mock.setZoom(1); screen.window.notify("Appearance restored") } }
                KetButton { text: "Save changes"; iconName: "check"; variant: KetTheme.ButtonVariant.Primary; onClicked: screen.window.notify("Settings saved") }
            }
        }
    }

    Component {
        id: typography
        Column {
            spacing: screen.px(18)
            Body { text: "Adjust readability across your workspace." }
            KetFieldRow { width: parent.width; label: "Interface font"; help: "KetPlus uses KÉT Sans throughout the interface."; KetSelect { width: parent.width; model: ["KÉT Sans"] } }
            KetFieldRow { width: parent.width; label: "Interface font size"; KetSelect { width: parent.width; model: ["13 px", "14 px", "15 px", "16 px"]; currentIndex: 2 } }
            KetFieldRow { width: parent.width; label: "Conversation width"; help: "Applies to Agent Workspace. Atlas keeps its compact reading column."; KetSelect { width: parent.width; model: ["Default · 760 px", "Wide · 864 px", "Extra wide · 1200 px", "Full width"] } }
            KetFieldRow { width: parent.width; label: "Editor font"; KetSelect { width: parent.width; model: ["KÉT Sans Mono", "Menlo", "Monospace"] } }
            KetFieldRow { width: parent.width; label: "Editor font size"; KetSelect { width: parent.width; model: ["12 px", "13 px", "14 px"]; currentIndex: 1 } }
            Rectangle {
                width: parent.width
                height: sample.implicitHeight + screen.px(32)
                radius: screen.px(7)
                color: KetTheme.t.panelBgSubtle
                border.color: KetTheme.t.panelBorder
                Column {
                    id: sample
                    x: screen.px(16); y: screen.px(16)
                    spacing: screen.px(8)
                    Text { text: "PREVIEW"; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(11); font.letterSpacing: 1 }
                    Text { text: "A space for what you are creating."; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(15) }
                    Text { text: "const workspace = \"develop\";\nagent.continue(workspace);"; color: KetTheme.t.textMain; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(13) }
                    Text { text: "➜ ketplus-desktop git:(develop)"; color: KetTheme.t.textMuted; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(13) }
                }
            }
        }
    }

    Component {
        id: account
        Column {
            spacing: screen.px(18)
            Body { text: "Your KetPlus account, and the phones and Linux machines that use this Mac." }
            Row {
                spacing: screen.px(12)
                KetAvatar { name: "An Nguyen"; diameter: 40 }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "An Nguyen"; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(15); font.weight: Font.Medium }
                    Text { text: "Personal workspace · KétPlus Pro"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                }
            }
            KetMeter { width: parent.width; label: "KetPlus Relay this month"; valueText: "3.1 GB of 20 GB"; fraction: 0.155; caption: "Pro includes the relay for phones and Linux machines" }
            Row {
                spacing: screen.px(8)
                KetButton { text: "Manage connections"; iconName: "network"; onClicked: screen.window.showScreen("connections") }
                KetButton { text: "Sign out"; iconName: "log-out"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.window.notify("Signed out of the demo account") }
            }
        }
    }

    Component {
        id: providers
        Column {
            spacing: screen.px(14)
            Body { text: "Agents KetPlus can start, and the browser they use." }
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; checked: true; text: "Codex"; description: "Signed in with ChatGPT · GPT-5.6 Luna by default" }
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; text: "Claude Code"; description: "Signed in with Claude · Opus 5.5 by default" }
            KetFieldRow { width: parent.width; label: "Default effort"; KetSelect { width: parent.width; model: Fixtures.efforts; currentIndex: 1 } }
            Divider {}
            SubHeading { text: "Browser" }
            KetChoice { width: parent.width; checked: true; text: "Let Agents use the built-in browser"; description: "Chromium in its own profile; pages, captures and visual comparison" }
            KetChoice { width: parent.width; text: "Allow any origin"; description: "Otherwise only localhost and origins you approve" }
        }
    }

    Component {
        id: archived
        Column {
            spacing: screen.px(14)
            Body { text: "Archived conversations keep their transcript and worktree. Delete removes them from this Mac." }
            KetEmptyState {
                visible: screen.window.archived.length === 0
                width: parent.width
                iconName: "archive"
                title: "Nothing archived"
                message: "Archive a conversation from its context menu."
            }
            Repeater {
                model: screen.window.archived
                Rectangle {
                    required property var modelData
                    required property int index
                    width: parent.width
                    height: screen.px(64)
                    radius: screen.px(7)
                    color: KetTheme.t.panelBg
                    border.color: KetTheme.t.panelBorder
                    Column {
                        x: screen.px(16)
                        anchors.verticalCenter: parent.verticalCenter
                        Text { text: modelData.title; color: KetTheme.t.textMain; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(14) }
                        Text { text: "@agent/" + modelData.number + " · " + modelData.repository + " · archived " + modelData.when; color: KetTheme.t.textMuted; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(12) }
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: screen.px(12)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: screen.px(6)
                        KetButton {
                            text: "Restore"
                            iconName: "restore"
                            controlSize: KetTheme.ControlSize.Small
                            onClicked: {
                                screen.window.archived = screen.window.archived.filter((a, i) => i !== index)
                                screen.window.notify("Restored to the sidebar")
                            }
                        }
                        KetButton { text: "Delete…"; variant: KetTheme.ButtonVariant.GhostDanger; controlSize: KetTheme.ControlSize.Small; onClicked: screen.window.deleteDialog.open() }
                    }
                }
            }
        }
    }

    Component {
        id: updates
        Column {
            spacing: screen.px(14)
            Body { text: "KetPlus checks for updates every day and installs them when you restart." }
            KetUpdateCard {
                width: parent.width
                visible: screen.window.updateReady
                title: "KétPlus Pro 1.8.0"
                ready: true
                badges: KetBadge { text: "Ready"; tone: KetTheme.BadgeTone.Success }
                detail: "Downloaded and verified. Restart to finish installing; running turns finish first."
                notes: ["Quick search finds conversations and Atlases", "Background tasks keep running after a restart",
                        "Compact window remembers its size per display"]
                actions: [
                    KetButton { text: "Restart now"; variant: KetTheme.ButtonVariant.Primary; onClicked: { screen.window.updateReady = false; screen.window.notify("KétPlus Pro 1.8.0 installed") } },
                    KetButton { text: "Later"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.window.notify("Reminder in 4 hours") }
                ]
            }
            KetUpdateCard {
                width: parent.width
                visible: !screen.window.updateReady
                title: "KétPlus Pro 1.8.0"
                badges: KetBadge { text: "Up to date"; tone: KetTheme.BadgeTone.Neutral }
                detail: "Checked just now."
            }
            KetFieldRow { width: parent.width; label: "Channel"; KetSelect { width: parent.width; model: ["Stable", "Beta"] } }
            KetCheckbox { text: "Download updates automatically"; checked: true }
            KetButton { text: "Check for updates"; iconName: "refresh"; onClicked: screen.window.notify("KétPlus Pro is up to date") }
        }
    }
}
