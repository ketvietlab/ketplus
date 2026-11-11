// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import Ket
import KetPlusMock
import KetPlusMock.Native

// Settings, in the order of apps/desktop SettingsWorkspacePage: Appearance (live theme and
// zoom), Typography, Account & devices, Agent models (opens the model picker in the Agent
// Workspace), Browser, Computer use, Providers, Archived and Updates.
Item {
    id: screen

    required property var window
    // The pages this edition has, by id; null shows them all (KétPlus Pro). KétPlus has
    // Appearance and Typography only.
    property var tabIds: null
    property string shortcut: "⌘ ,"
    readonly property var tabs: allTabs.filter(t => !tabIds || tabIds.includes(t.id))
    readonly property var allTabs: [
        { id: "appearance", text: "Appearance", icon: "palette" },
        { id: "typography", text: "Typography", icon: "text-size" },
        { id: "account", text: "Account & devices", icon: "display" },
        { id: "agent-models", text: "Agent models", icon: "sliders" },
        { id: "browser", text: "Browser", icon: "globe" },
        { id: "computer", text: "Computer use", icon: "display" },
        { id: "providers", text: "Providers", icon: "package" },
        { id: "archived", text: "Archived", icon: "archive" },
        { id: "storage", text: "Storage", icon: "layers" },
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
    // A settings row: label and help on the left, the control right-aligned and at most
    // 200 px wide.
    component SettingRow: Item {
        id: settingRow
        property string label
        property string help
        default property alias control: slot.data
        readonly property var recipe: KetTheme.fieldRowRecipe(KetTheme.dark, KetTheme.zoom)
        width: parent.width
        implicitHeight: Math.max(texts.implicitHeight, slot.implicitHeight)

        Column {
            id: texts
            anchors.left: parent.left
            anchors.right: slot.left
            anchors.rightMargin: settingRow.recipe.gap
            topPadding: settingRow.recipe.labelTop
            spacing: settingRow.recipe.helpGap
            Text {
                width: parent.width
                text: settingRow.label
                wrapMode: Text.WordWrap
                color: settingRow.recipe.label
                font.family: KetTheme.fontFamily
                font.pixelSize: settingRow.recipe.labelPixels
            }
            Text {
                width: parent.width
                visible: text !== ""
                text: settingRow.help
                wrapMode: Text.WordWrap
                color: settingRow.recipe.help
                font.family: KetTheme.fontFamily
                font.pixelSize: settingRow.recipe.helpPixels
            }
        }
        Item {
            id: slot
            anchors.right: parent.right
            width: Math.min(screen.px(200), settingRow.width)
            implicitHeight: children.length ? children[0].implicitHeight : 0
        }
    }
    // A status line of a provider: a fixed label column, then the value.
    component InfoRow: Row {
        property string label
        property string value
        width: parent.width
        Text {
            width: screen.px(180)
            // A long label wraps in its column, as the app's form label does.
            rightPadding: screen.px(12)
            wrapMode: Text.Wrap
            text: parent.label
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: screen.px(14)
        }
        Text {
            width: parent.width - screen.px(180)
            text: parent.value
            wrapMode: Text.Wrap
            color: KetTheme.t.textMain
            font.family: KetTheme.fontFamily
            font.pixelSize: screen.px(14)
        }
    }
    component Help: Text {
        width: parent.width
        wrapMode: Text.Wrap
        color: KetTheme.t.textMuted
        font.family: KetTheme.fontFamily
        font.pixelSize: screen.px(13)
    }
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
                KetKbd { text: screen.shortcut }
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
                            onClicked: modelData.id === "agent-models" ? screen.window.openAgentModels()
                                       : screen.window.showScreen(modelData.id === "appearance" ? "settings" : "settings/" + modelData.id)
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
                                            browser: browser, computer: computer, providers: providers,
                                            archived: archived, storage: storage,
                                            updates: updates })[screen.current.id]
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
            SettingRow { label: "Interface font"; help: "KetPlus uses KÉT Sans throughout the interface."; KetSelect { width: parent.width; model: ["KÉT Sans"] } }
            SettingRow { label: "Interface font size"; KetSelect { width: parent.width; model: ["13 px", "14 px", "15 px", "16 px"]; currentIndex: 2 } }
            SettingRow { label: "Conversation width"; help: "Applies to Agent Workspace. Atlas keeps its compact reading column."; KetSelect { width: parent.width; model: ["Default · 760 px", "Wide · 864 px", "Extra wide · 1200 px", "Full width"] } }
            SettingRow { label: "Editor font"; KetSelect { width: parent.width; model: ["KÉT Sans Mono", "Menlo", "Monospace"] } }
            SettingRow { label: "Editor font size"; KetSelect { width: parent.width; model: ["12 px", "13 px", "14 px", "15 px", "16 px"]; currentIndex: 2 } }
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
                    Text { text: "const workspace = \"develop\";\nagent.continue(workspace);"; color: KetTheme.t.textMain; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(14) }
                    Text { text: "➜ ketplus-desktop git:(develop)"; color: KetTheme.t.textMuted; font.family: KetTheme.monoFontFamily; font.pixelSize: screen.px(14) }
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
                    Text { text: "an · KétPlus Pro"; color: KetTheme.t.textSecondary; font.family: KetTheme.fontFamily; font.pixelSize: screen.px(13) }
                }
            }
            // No relay usage meter: the relay reports no usage (ADR 0109).
            Row {
                spacing: screen.px(8)
                KetButton { text: "Manage connections"; iconName: "network"; onClicked: screen.window.showScreen("connections") }
                KetButton { text: "Sign out"; iconName: "log-out"; variant: KetTheme.ButtonVariant.Ghost; onClicked: screen.window.notify("Signed out of the demo account") }
            }
        }
    }

    Component {
        id: browser
        Column {
            spacing: screen.px(14)
            Row {
                spacing: screen.px(12)
                Body { width: implicitWidth; anchors.verticalCenter: parent.verticalCenter; text: "Ready · bundled browser runtime" }
                KetButton { text: "Check again"; controlSize: KetTheme.ControlSize.Small; onClicked: screen.window.notify("Browser runtime is ready") }
            }
            KetChoice { width: parent.width; checked: true; text: "Let agents use a browser"; description: "Chromium in its own profile; pages, captures and visual comparison" }
            Divider {}
            SubHeading { text: "Websites" }
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; text: "All websites" }
            KetChoice { width: parent.width; kind: KetTheme.ToggleKind.Radio; checked: true; text: "Only these origins" }
            SettingRow { label: "Allowed origins"; help: "Separate origins with commas. localhost is always allowed."; KetTextField { width: parent.width; text: "https://ketviet.vn" } }
            SettingRow { label: "Source checkout path"; help: "Only needed for a source checkout"; KetTextField { width: parent.width; placeholderText: "~/dev/ketplus" } }
        }
    }

    Component {
        id: computer
        Column {
            spacing: screen.px(14)
            Body { text: "Let agents see and control native Mac apps through KetPlus Computer, for tasks no command or the KetPlus browser can do." }
            KetChoice { width: parent.width; checked: true; text: "Allow agents to use this computer" }
            Divider {}
            SubHeading { text: "Work display" }
            SettingRow { label: "Work display"; help: "The agent works on this display: approved apps move there and screenshots show it. With a single display, it uses the main one."; KetSelect { width: parent.width; model: ["Secondary display", "Main display"] } }
            Divider {}
            SubHeading { text: "Blocked apps" }
            SettingRow { label: "Blocked app bundle IDs"; help: "Bundle IDs separated by commas. KetPlus, Keychain Access and password managers are always blocked. Terminals and IDEs accept clicks only, and browsers other than Safari are view-only."; KetTextField { width: parent.width; placeholderText: "com.example.Banking, com.example.Mail" } }
            Divider {}
            SubHeading { text: "Safety" }
            Help { text: "Screenshots show only the apps you approved; other apps and KetPlus stay hidden. Agents work in the background without taking your pointer. Press Stop on the banner, or Esc in an approved app, to stop: the agent loses its approvals and must ask again. Turns started from your phone cannot control this Mac." }
            Divider {}
            SubHeading { text: "macOS permissions" }
            Help { text: "KetPlus Computer asks for Screen Recording and Accessibility the first time an agent needs them. Grant both to KetPlus Computer, not to KetPlus." }
            KetButton { text: "Open Privacy & Security"; iconName: "shield"; onClicked: screen.window.notify("Opens System Settings › Privacy & Security") }
            Help { text: "KetPlus Computer is installed with this app." }
        }
    }

    Component {
        id: providers
        Column {
            spacing: screen.px(14)
            Body { text: "Codex and Cursor run from the CLIs installed on this computer; Claude runs from the Agent SDK bundled with KetPlus. KetPlus supports CLI releases from about the last three months." }
            Row {
                readonly property int attention: Object.values(screen.window.providerIssues).filter(id => !!id).length
                spacing: screen.px(12)
                Body {
                    width: implicitWidth
                    anchors.verticalCenter: parent.verticalCenter
                    color: KetTheme.t.textMain
                    text: parent.attention === 0 ? "All providers are ready."
                        : parent.attention === 1 ? "1 provider needs attention." : parent.attention + " providers need attention."
                }
                KetButton { text: "Check again"; controlSize: KetTheme.ControlSize.Small; onClicked: screen.window.notify("Provider versions checked") }
            }
            // Each provider: its status badge and details, then the setup guide when it has a
            // setup state (Fixtures.providerSetup), the same guide the conversation shows.
            Repeater {
                model: Fixtures.providerStatus
                Column {
                    id: providerSection
                    required property var modelData
                    readonly property string setupId: screen.window.providerIssues[modelData.id] || ""
                    readonly property var setup: Fixtures.providerSetup[setupId] || null
                    readonly property string tone: setup ? setup.tone : modelData.tone
                    width: parent.width
                    spacing: screen.px(8)
                    topPadding: screen.px(6)
                    Row {
                        spacing: screen.px(10)
                        SubHeading { anchors.verticalCenter: parent.verticalCenter; text: providerSection.modelData.name }
                        KetBadge {
                            anchors.verticalCenter: parent.verticalCenter
                            text: providerSection.setup ? providerSection.setup.badge : providerSection.modelData.badge
                            tone: providerSection.tone === "danger" ? KetTheme.BadgeTone.Danger
                                : providerSection.tone === "warning" ? KetTheme.BadgeTone.Warning : KetTheme.BadgeTone.Success
                        }
                    }
                    Repeater {
                        model: providerSection.setup ? providerSection.setup.rows : providerSection.modelData.rows
                        InfoRow { required property var modelData; label: modelData[0]; value: modelData[1] }
                    }
                    Help { visible: !providerSection.setup && text !== ""; text: providerSection.modelData.note }
                    Loader {
                        active: !!providerSection.setup
                        visible: active
                        width: parent.width
                        sourceComponent: ProviderSetupCard {
                            setupId: providerSection.setupId
                            window: screen.window
                            inSettings: true
                        }
                    }
                }
            }
            Divider {}
            SubHeading { text: "Claude API access" }
            Help { text: "Use your own Anthropic API key on this Mac. Paired phones use the same configuration. Changes apply to new queries after current work finishes." }
            SettingRow { label: "Authentication"; KetSelect { width: parent.width; model: ["Existing Claude runtime authentication", "My Anthropic API key"] } }
            SettingRow { label: "API key"; KetTextField { width: parent.width; placeholderText: "Enter a new key; leave blank to keep the saved key" } }
            SettingRow { label: "Query budget"; KetSelect { width: parent.width; model: ["No query limit", "50.00 USD", "100.00 USD", "195.00 USD"]; currentIndex: 3 } }
            Help { text: "Stored in macOS Keychain. Usage is billed to the key's Anthropic account. The query budget uses an estimate and can be exceeded; check Anthropic Console for actual charges." }
            Flow {
                width: parent.width
                spacing: screen.px(8)
                KetButton { text: "Save Claude settings"; variant: KetTheme.ButtonVariant.Primary; onClicked: screen.window.notify("Claude settings saved") }
                KetButton { text: "Check connection"; onClicked: screen.window.notify("Anthropic accepted the saved key. No model inference was run.") }
                KetButton { text: "Remove key…"; variant: KetTheme.ButtonVariant.GhostDanger }
                KetButton { text: "Refresh"; variant: KetTheme.ButtonVariant.Ghost }
            }
            Body { color: KetTheme.t.textMain; text: "Key saved. New queries use existing Claude runtime authentication." }
            Help { text: "Last observed credential: runtime login or other provider credential. Latest SDK session estimate: 163.68 USD." }
            Divider {}
            SubHeading { text: "Claude organization usage" }
            Help { text: "Optional Admin API key for organization-wide monthly usage and cost. Stored separately in macOS Keychain; never sent to the agent. This key may belong to a different organization from your inference key. Reports include all organization products and keys." }
            SettingRow { label: "Admin API key"; KetTextField { width: parent.width; placeholderText: "sk-ant-admin01-…" } }
            Flow {
                width: parent.width
                spacing: screen.px(8)
                KetButton { text: "Save Admin key"; onClicked: screen.window.notify("Admin key saved") }
                KetButton { text: "Remove Admin key…"; variant: KetTheme.ButtonVariant.GhostDanger }
                KetButton { text: "Refresh organization usage"; variant: KetTheme.ButtonVariant.Ghost }
            }
            Help { text: "Organization month-to-date: 412.80 USD · 38.2M tokens. Updated 10:42." }
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
                // Release notes stay in Sparkle's own window.
                detail: "Downloaded and verified. Restart to finish installing; running turns finish first."
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
            // One release channel per build (staging and production are separate apps).
            KetCheckbox { text: "Check for updates automatically"; checked: true }
            KetCheckbox { text: "Download updates automatically"; checked: true }
            KetButton { text: "Check for updates"; iconName: "refresh"; onClicked: screen.window.notify("KétPlus Pro is up to date") }
        }
    }

    // The daemon journal: what it holds, how long full detail is kept, the size limit and
    // its warning, and compaction. See ketvietlab/ketplus issue "Journal grows without bound".
    Component {
        id: storage
        Column {
            id: storagePage
            readonly property var info: Fixtures.journalStorage
            property bool compacted: false
            readonly property real totalGb: compacted ? info.totalGb - info.parts[3].gb : info.totalGb
            spacing: screen.px(20)

            function compact() {
                compacted = true
                screen.window.storageWarning = false
                screen.window.notify("Journal compacted · " + info.totalGb.toFixed(1) + " GB → "
                                     + totalGb.toFixed(1) + " GB")
            }

            Body { text: "KetPlus keeps every conversation in a local journal so transcripts, remote devices and restarts can replay them. Choose how long it keeps full detail and how large it may grow." }
            InfoRow { label: "SQLite files (including write-ahead log)"; value: "9.1 GB" }
            InfoRow { label: "Disk space available"; value: "31.0 GB" }


            KetBanner {
                width: parent.width
                visible: !storagePage.compacted
                tone: KetTheme.BannerTone.Warning
                title: "The journal is over its size limit"
                message: storagePage.info.totalGb.toFixed(1) + " GB on disk, limit " + storagePage.info.limitGb
                         + " GB. " + storagePage.info.parts[3].gb.toFixed(1)
                         + " GB of it is free space that compacting returns to macOS."
                actionText: "Compact now"
                onActionTriggered: storagePage.compact()
            }

            KetMeter {
                width: parent.width
                label: "Journal"
                valueText: storagePage.totalGb.toFixed(1) + " GB of " + storagePage.info.limitGb + " GB"
                fraction: Math.min(1, storagePage.totalGb / storagePage.info.limitGb)
                caption: "~/Library/Application Support/Ketsuite/KetPlus/daemon · full detail since "
                         + storagePage.info.oldest
            }

            Column {
                width: parent.width
                spacing: screen.px(10)
                Repeater {
                    model: storagePage.info.parts
                    KetMeter {
                        required property var modelData
                        required property int index
                        width: parent.width
                        label: modelData.label
                        readonly property real gb: storagePage.compacted && index === 3 ? 0 : modelData.gb
                        valueText: gb.toFixed(1) + " GB"
                        fraction: gb / storagePage.info.totalGb
                        caption: modelData.detail
                    }
                }
            }

            Divider {}
            SubHeading { text: "Retention" }
            SettingRow {
                label: "Keep full detail for"
                help: "While idle, trim supported older tool details. Messages, running turns and the latest 20 turns stay protected. Changes save immediately."
                KetSelect { width: parent.width; model: ["7 days", "30 days", "90 days", "Forever"]; currentIndex: 1 }
            }
            SettingRow {
                label: "Database size target"
                help: "Save explicitly to enable earlier detail trimming while idle. This soft SQLite-page target excludes media; protected content can remain above it. Forever and automatic-off preserve detail."
                KetSelect { width: parent.width; model: ["1 GB", "2 GB", "5 GB", "10 GB", "No limit"]; currentIndex: 2 }
            }
            Help { text: "Warning threshold: 80%. Save database target explicitly; opening Settings never enables it." }
            SettingRow {
                label: "Generated images"
                help: "After the detail period, keep the full image, a 512 px preview, or only its caption."
                KetSelect { width: parent.width; model: ["Keep full image", "Keep preview", "Caption only"]; currentIndex: 1 }
            }
            SettingRow {
                label: "Archived conversations"
                help: "Delete the history of a conversation this long after it is archived."
                KetSelect { width: parent.width; model: ["30 days", "90 days", "1 year", "Never"]; currentIndex: 1 }
            }
            KetCheckbox { text: "Warn me when the journal passes " + storagePage.info.warnPercent + "% of its limit"; checked: true }
            KetCheckbox { text: "Trim older detail automatically while idle"; checked: true }
            Help { text: "The native Storage page currently supports the detail period and automatic detail trimming. The database target requires an explicit save; images, archive expiry and remaining controls are a design preview." }

            Divider {}
            SubHeading { text: "Largest conversations" }
            Column {
                width: parent.width
                spacing: screen.px(8)
                Repeater {
                    model: storagePage.info.largest
                    InfoRow {
                        required property var modelData
                        label: "@agent/" + modelData.number
                        value: modelData.title + " · " + modelData.provider + " · " + modelData.size
                               + " · " + modelData.note
                    }
                }
            }

            Divider {}
            InfoRow { label: "Last compaction"; value: storagePage.compacted ? "Just now" : storagePage.info.lastCompaction }
            Row {
                spacing: screen.px(10)
                KetButton { text: "Compact now"; variant: KetTheme.ButtonVariant.Primary; enabled: !storagePage.compacted; onClicked: storagePage.compact() }
                KetButton { text: "Export conversations…"; iconName: "download"; onClicked: screen.window.notify("Exported 88 conversations") }
            }
            Help { text: "Compacting rewrites the journal while KetPlus is idle and needs free disk space about the size of the kept data. Running turns keep working." }
        }
    }
}
