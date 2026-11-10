// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import Ket
import KetPlusMock
import KetPlusMock.Native

// The KetPlus Pro window: the sidebar and the current screen, with the app-wide overlays
// (Quick search, the repository picker, dialogs and toasts).
Basic.ApplicationWindow {
    id: app

    // Screens, in sidebar order. A state follows a slash: "workspace/welcome".
    readonly property var screenIds: [
        "workspace/plan-ready", "workspace/plan-planning", "workspace/plan-running", "workspace/plan-continuing", "workspace/plan-blocked", "workspace/plan-paused", "workspace/plan-limit", "workspace/plan-complete",
        "workspace/content-tabs", "workspace/content-terminal", "workspace/content-file",
        "workspace", "workspace/welcome", "workspace/running", "workspace/question",
        "workspace/error", "workspace/atlas", "workspace/details", "workspace/report",
        "workspace/report-running", "workspace/report-details", "workspace/claude",
        "workspace/codex", "workspace/cursor", "workspace/turn-report", "editor", "editor/terminal",
        "editor/markdown", "editor/markdown-source", "editor/samples",
        "changes", "worktrees", "teams", "teams/task", "teams/large", "teams/empty",
        "connections", "connections/no-activity", "connections/signed-out",
        "connections/expired", "connections/sign-in", "settings", "settings/typography", "settings/account",
        "settings/browser", "settings/computer", "settings/providers", "settings/archived",
        "settings/updates", "settings/storage", "workspace/storage-warning", "atlas", "mobile-picker", "workspace/compact",
        // Provider setup (Fixtures.providerSetup): the provider cannot start a turn, or ended one.
        "workspace/setup-cli-missing", "workspace/setup-cli-outdated", "workspace/setup-signed-out",
        "workspace/setup-claude-signed-out", "workspace/setup-sidecar-missing", "workspace/setup-expired",
        "workspace/setup-key-rejected", "workspace/setup-limit", "workspace/setup-capacity",
        "workspace/setup-plan-blocked", "settings/providers-setup"
    ]
    readonly property string planningState: workspace.planState
    function planStatus(number, fallback) { return workspace.planStatus(number, fallback) }
    property string screen: "workspace"
    // The terminal below the editor (editor/terminal).
    property bool editorTerminal: false
    // A file the editor opens (editor/markdown), and whether Markdown shows its source.
    property string editorFile: ""
    property bool editorSource: false
    property string settingsTab: "appearance"
    // The KetSuite account that Connections needs; signed out shows its welcome
    // (connections/signed-out), and an expired login also says why (connections/expired).
    property bool signedIn: true
    property bool signInExpired: false
    property var conversation: Fixtures.repositories[0].conversations[0]
    property string repository: Fixtures.repositories[0].name
    property string machine: Fixtures.machines[0]
    property bool updateReady: true
    // The journal is over its size limit (settings/storage, workspace/storage-warning); the
    // sidebar warns until it is compacted.
    property bool storageWarning: false
    // Each provider's setup state (a Fixtures.providerSetup key), or "" when it is ready.
    property var providerIssues: ({ codex: "", claude: "", cursor: "" })
    // Providers whose guide the user hid above the composer; a new setup state shows it again.
    property var hiddenSetups: ({})
    // Paired devices have done something on this Mac (connections); none yet in
    // connections/no-activity.
    property bool remoteActivity: true
    property string tagFilter
    // Teams (concept): folders in the sidebar; a team's name opens its page (teams).
    readonly property var teamList: teams.teams
    readonly property string currentTeam: teams.team ? teams.team.id : ""
    function openTeam(id) {
        teams.showTeam(id)
        screen = "teams"
    }
    // The sidebar's team actions. Each shows the team's page behind its dialog.
    function newTeam(repositoryName) {
        screen = "teams"
        teams.startNewTeam(repositoryName)
    }
    function addTeamMember(id, number) {
        screen = "teams"
        teams.startAddMember(id, number)
    }
    function deleteTeam(id) {
        screen = "teams"
        teams.startDelete(id)
    }
    function removeTeamMember(id, number) {
        screen = "teams"
        teams.startRemove(id, number)
    }
    function teamOf(number) { return teamList.find(t => t.members.some(m => m.number === number)) || null }
    property bool compact: false
    property size wideSize: Qt.size(1440, 900)

    function setCompact(on) {
        if (on === compact)
            return
        if (on) {
            wideSize = Qt.size(width, height)
            workspace.detailsOpen = false
            screen = "workspace"
        }
        compact = on
        width = on ? KetTheme.px(480, KetTheme.zoom) : wideSize.width
        height = on ? Math.min(height, KetTheme.px(760, KetTheme.zoom)) : wideSize.height
    }
    property var archived: [
        { number: "1022", title: "Spike the relay handshake", repository: "ketviet", when: "Oct 28" },
        { number: "0871", title: "Old onboarding copy", repository: "ketplus-desktop", when: "Oct 12" }
    ]

    function setProviderIssue(provider, state) {
        const next = Object.assign({}, providerIssues)
        next[provider] = state
        providerIssues = next
        showSetup(provider)
    }

    function hideSetup(provider) {
        const next = Object.assign({}, hiddenSetups)
        next[provider] = true
        hiddenSetups = next
    }

    function showSetup(provider) {
        if (!hiddenSetups[provider])
            return
        const next = Object.assign({}, hiddenSetups)
        delete next[provider]
        hiddenSetups = next
    }

    // The guide of a provider's setup state in a dialog, from anywhere (Model & context).
    function openSetupGuide(provider) {
        const state = providerIssues[provider] || ""
        if (!Fixtures.providerSetup[state])
            return
        setupDialog.provider = provider
        setupDialog.setupId = state
        setupDialog.open()
    }

    // A provider's setup state in the Agent Workspace. One that cannot start a turn opens a new
    // conversation with that provider, its guide above the composer; one that ended a turn
    // opens a conversation whose last turn failed with it.
    function openSetup(state) {
        const setup = Fixtures.providerSetup[state]
        if (!setup)
            return
        if (!setup.transient)
            setProviderIssue(setup.provider, state)
        const number = setup.blocking ? "1204" : "7310"
        for (const r of Fixtures.repositories) {
            const base = r.conversations.find(c => c.number === number)
            if (base) {
                openConversation(r.name, Object.assign({}, base, {
                    title: setup.topic || base.title, provider: setup.name, providerId: setup.provider, model: setup.model,
                    effort: setup.provider === "cursor" ? "Default" : "Medium",
                    transcript: setup.blocking ? "welcome" : "setup", setup: state }))
                return
            }
        }
    }

    // "Use Codex instead": the new conversation's provider changes before anything was sent.
    function switchProvider(provider) {
        const entry = Fixtures.providers.find(p => p.id === provider)
        conversation = Object.assign({}, conversation, {
            provider: entry ? entry.label : provider, providerId: provider,
            model: Fixtures.providerModels[provider][0].name,
            effort: provider === "cursor" ? "Default" : "Medium" })
        notify("This conversation now uses " + (entry ? entry.label : provider))
    }

    // Model & context → Apply: the conversation's provider, model and effort.
    function chooseModel(provider, model, effort) {
        const entry = Fixtures.providers.find(p => p.id === provider)
        conversation = Object.assign({}, conversation, {
            provider: entry ? entry.label : provider, providerId: provider, model: model, effort: effort })
        notify("Applies from the next turn")
    }

    function newConversation(provider) {
        for (const r of Fixtures.repositories) {
            const base = r.conversations.find(c => c.transcript === "welcome")
            if (base) {
                openConversation(r.name, base)
                switchProvider(provider)
                return
            }
        }
    }

    function openComposerPanel(name) {
        screen = "workspace"
        Qt.callLater(() => workspace.openComposerPanel(name))
    }

    function showScreen(id) {
        const parts = String(id).split("/")
        const name = parts[0]
        const state = parts[1] || ""
        if (name === "workspace" && ["content-tabs", "content-terminal", "content-file"].includes(state)) {
            setCompact(false)
            const repo = Fixtures.repositories[0]
            openConversation(repo.name, repo.conversations.find(c => c.number === "4821"))
            workspace.openContentDemo(state.slice(8))
            return
        }
        if (name === "workspace" && state.startsWith("plan-")) {
            setCompact(false)
            const repo = Fixtures.repositories[0]
            openConversation(repo.name, repo.conversations.find(c => c.number === "4821"))
            workspace.openPlan(state.slice(5))
            return
        }
        if (name === "workspace" && state.startsWith("setup-")) {
            setCompact(false)
            workspace.detailsOpen = false
            openSetup(state.slice(6))
            return
        }
        if (id === "settings/providers-setup") {
            // Settings → Providers with two providers to fix.
            setProviderIssue("cursor", "cli-missing")
            setProviderIssue("claude", "expired")
            settingsTab = "providers"
            screen = "settings"
            return
        }
        if (state === "compact") {
            setCompact(true)
            return
        }
        setCompact(false)
        if (state === "storage-warning" || id === "settings/storage")
            storageWarning = true
        if (name === "workspace") {
            const kind = state === "details" ? "activity" : state === "report-details" ? "report" : state
            if (kind) {
                for (const r of Fixtures.repositories) {
                    const match = r.conversations.find(c => c.transcript === kind)
                    if (match) {
                        openConversation(r.name, match)
                        break
                    }
                }
            }
            workspace.detailsOpen = state === "details" || state === "report-details"
            if (state === "details") workspace.openContent("changes")
            if (state === "report-details") workspace.openContent("report")
        }
        if (name === "settings")
            settingsTab = state || "appearance"
        if (name === "connections") {
            remoteActivity = state !== "no-activity"
            signedIn = !["signed-out", "expired", "sign-in"].includes(state)
            signInExpired = state === "expired"
            if (state === "sign-in")
                Qt.callLater(connections.openSignIn)
        }
        if (name === "teams")
            teams.showState(state)
        if (name === "editor") {
            editorTerminal = state === "terminal"
            editorFile = state.startsWith("markdown") ? "licensing.md"
                       : state === "samples" ? "design-system.md" : ""
            editorSource = state === "markdown-source"
        }
        screen = name
    }

    function openConversation(repositoryName, item) {
        repository = repositoryName
        conversation = item
        screen = "workspace"
    }

    function openEveryOverlay() {
        spotlight.open()
        spotlight.close()
        picker.open()
        picker.close()
        renameDialog.open()
        renameDialog.close()
        setupDialog.setupId = "cli-missing"
        setupDialog.open()
        setupDialog.close()
        deleteDialog.open()
        deleteDialog.close()
        connections.openActivity("iPhone 16")
        connections.closeActivity()
        teams.openEveryOverlay()
        toast.show("Smoke test")
    }

    function notify(message) { toast.show(message) }

    // The image viewer over the whole window, as the app opens a capture or an image file.
    function openImages(images, index) {
        imageViewer.items = images
        imageViewer.openAt(index)
    }

    // Settings › Agent models opens the model picker in the Agent Workspace, as the app does.
    function openAgentModels() {
        showScreen("workspace")
        Qt.callLater(() => workspace.openComposerPanel("model"))
    }

    width: 1440
    height: 900
    minimumWidth: compact ? 360 : 1024
    minimumHeight: 640
    visible: true
    title: "KétPlus Pro · Mock"
    color: KetTheme.t.pageBg
    font.family: KetTheme.fontFamily

    Component.onCompleted: {
        showScreen(Mock.initialScreen + (Mock.initialState ? "/" + Mock.initialState : ""))
        if (Mock.initialPanel === "images")
            Qt.callLater(() => openImages(Fixtures.captures, 1))
        else if (Mock.initialPanel === "picker")
            Qt.callLater(() => picker.open())
        else if (Mock.initialPanel === "setup")
            Qt.callLater(() => openSetupGuide(conversation.providerId || "claude"))
        else if (["activity", "device", "device-focus"].includes(Mock.initialPanel) && screen === "connections") {
            Qt.callLater(() => connections.openActivity(Mock.initialPanel === "activity" ? "" : "iPhone 16"))
            if (Mock.initialPanel === "device-focus")
                Qt.callLater(() => Qt.callLater(() => connections.focusActivityRow(3)))
        }
        else if (Mock.initialPanel === "toast")
            // The app-wide toast, for the side by side with the app.
            Qt.callLater(() => notify("Saved the plan"))
        else if (["repo-menu", "team-menu", "row-menu"].includes(Mock.initialPanel))
            Qt.callLater(() => sidebar.openMenu(Mock.initialPanel))
        else if (screen === "teams" && Mock.initialPanel)
            Qt.callLater(() => teams.openDialog(Mock.initialPanel))
        else if (["pair", "linux", "signin"].includes(Mock.initialPanel) && screen === "connections")
            Qt.callLater(() => connections.openDialog(Mock.initialPanel))
        else if (Mock.initialPanel && screen === "workspace")
            Qt.callLater(() => workspace.openComposerPanel(Mock.initialPanel))
    }

    Shortcut {
        sequences: ["Ctrl+K", "Meta+K"]
        onActivated: spotlight.open()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+M", "Meta+Shift+M"]
        onActivated: app.setCompact(!app.compact)
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: app.showScreen("settings")
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            id: sidebar
            Layout.fillHeight: true
            Layout.preferredWidth: KetTheme.px(240, KetTheme.zoom) // 283 less 15%
            visible: app.screen !== "mobile-picker" && !app.compact
            window: app
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            visible: sidebar.visible
            color: KetTheme.t.sidebarBorder
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: ["workspace", "editor", "changes", "worktrees", "connections", "settings",
                           "atlas", "mobile-picker", "teams"].indexOf(app.screen)

            WorkspaceScreen { id: workspace; window: app }
            EditorScreen { window: app }
            SourceControlScreen { window: app }
            WorktreesScreen { window: app }
            ConnectionsScreen { id: connections; window: app }
            SettingsScreen { window: app }
            AtlasBuilderScreen { window: app }
            MobilePickerScreen { window: app }
            TeamsScreen { id: teams; window: app }
        }
    }

    KetCommandPalette {
        id: spotlight
        groups: {
            const query = spotlight.query.toLowerCase()
            if (!query)
                return Fixtures.spotlightGroups
            return Fixtures.spotlightGroups.map(group => ({
                title: group.title,
                items: group.items.filter(item => (item.title + " " + item.detail)
                                                      .toLowerCase().includes(query))
            }))
        }
        placeholderText: "Search files, conversations, Atlases and commands"
        onActivated: item => {
            app.showScreen(item.screen)
            if (item.id.startsWith("chat:")) {
                const number = item.id.slice(5)
                for (const r of Fixtures.repositories)
                    for (const c of r.conversations)
                        if (c.number === number)
                            app.openConversation(r.name, c)
            }
        }
    }

    RepositoryPicker {
        id: picker
        window: app
    }

    // A provider's setup guide (Model & context → Show setup steps). It closes once the
    // provider is fixed.
    KetDialog {
        id: setupDialog
        property string provider
        property string setupId
        readonly property var setup: Fixtures.providerSetup[setupId] || ({})
        readonly property bool resolved: provider !== "" && !app.providerIssues[provider]
        onResolvedChanged: if (resolved && visible) close()
        title: setup.title || ""
        ProviderSetupCard {
            Layout.fillWidth: true
            setupId: setupDialog.setupId
            window: app
            inDialog: true
        }
        actions: [
            KetButton { text: "Close"; variant: KetTheme.ButtonVariant.Ghost; onClicked: setupDialog.reject() }
        ]
    }

    KetDialog {
        id: renameDialog
        title: "Rename conversation"
        KetFieldRow {
            width: parent.width
            label: "Name"
            KetTextField {
                id: renameField
                width: parent.width
                text: app.conversation.title
            }
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: renameDialog.reject() },
            KetButton {
                text: "Rename"
                variant: KetTheme.ButtonVariant.Primary
                onClicked: {
                    renameDialog.accept()
                    app.notify("Renamed to “" + renameField.text + "”")
                }
            }
        ]
    }

    KetDialog {
        id: deleteDialog
        title: "Delete conversation?"
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "@agent/" + app.conversation.number + " and its transcript are removed from this "
                  + "Mac. Its worktree and files stay. Archive keeps a conversation you may need again."
            color: KetTheme.t.textSecondary
            font.family: KetTheme.fontFamily
            font.pixelSize: KetTheme.px(KetTheme.t.textMd, KetTheme.zoom)
            lineHeight: 1.4
        }
        actions: [
            KetButton { text: "Cancel"; variant: KetTheme.ButtonVariant.Ghost; onClicked: deleteDialog.reject() },
            KetButton { text: "Archive instead"; onClicked: { deleteDialog.reject(); app.notify("Conversation archived") } },
            KetButton {
                text: "Delete"
                variant: KetTheme.ButtonVariant.Danger
                onClicked: { deleteDialog.accept(); app.notify("Conversation deleted") }
            }
        ]
    }

    property alias renameDialog: renameDialog
    property alias deleteDialog: deleteDialog
    property alias spotlight: spotlight
    property alias picker: picker

    KetLightbox {
        id: imageViewer
        tools: KetTheme.LightboxTool.ZoomOut | KetTheme.LightboxTool.ZoomLevel
               | KetTheme.LightboxTool.ZoomIn | KetTheme.LightboxTool.Rotate
               | KetTheme.LightboxTool.Copy | KetTheme.LightboxTool.Download
               | KetTheme.LightboxTool.Print | KetTheme.LightboxTool.Fullscreen
        onToolTriggered: tool => {
            if (tool === KetTheme.LightboxTool.Copy)
                app.notify("Copied the image")
            else if (tool === KetTheme.LightboxTool.Download)
                app.notify("Saved " + (item ? item.caption.split(" · ").pop() : "the image")
                           + " to Downloads")
            else if (tool === KetTheme.LightboxTool.Print && item)
                Mock.printImage(item.source, rotationAngle, item.caption)
            else if (tool === KetTheme.LightboxTool.Fullscreen)
                app.visibility = app.visibility === Window.FullScreen ? Window.Windowed
                                                                      : Window.FullScreen
        }
    }

    KetToast { id: toast }
}
