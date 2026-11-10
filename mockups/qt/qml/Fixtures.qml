// SPDX-License-Identifier: MPL-2.0
pragma Singleton
import QtQuick
import Ket
import KetPlusMock.Native

// Deterministic sample data for every screen. Nothing here talks to a daemon, Git or a
// provider; the mock only presents these values.
QtObject {
    // Mermaid samples (mermaid/*.mmd), rendered ahead of time by tools/render_mermaid.py.
    readonly property var mermaid: ({
        sharedClock: Mock.resourceText(":/mermaid/shared-clock.mmd").trim(),
        agentTurn: Mock.resourceText(":/mermaid/agent-turn.mmd").trim()
    })
    readonly property var mermaidNames: ({ sharedClock: "shared-clock", agentTurn: "agent-turn" })

    // The rendering of a Mermaid fence in this theme; empty for a diagram the mock lacks.
    function mermaidImage(source, dark) {
        const text = source.trim()
        for (const key in mermaid) {
            if (mermaid[key] === text)
                return "qrc:/qt/qml/KetPlusMock/assets/mermaid/" + mermaidNames[key]
                    + (dark ? "-dark" : "-light") + "@2x.png"
        }
        return ""
    }

    // The browser captures an activity shows; the gallery opens them in the image viewer.
    readonly property var captures: ["sidebar", "editor", "settings"].map(name => ({
        source: "qrc:/qt/qml/KetPlusMock/assets/screenshot-" + name + ".png",
        caption: "Browser capture · " + name + ".png" }))
    // Tag colours are user data, so they are literals rather than tokens.
    readonly property var tagColors: ({
        UI: "#4f6bed", Performance: "#c2780a", Review: "#7c5cd6", Atlas: "#3f8f7a",
        Account: "#2f8f4e", Bug: "#d1453b", Docs: "#5b6b7f"
    })

    function tag(label) { return { label: label, color: tagColors[label] || "#5b6b7f" } }

    readonly property var repositories: [
        {
            name: "ketplus-desktop",
            conversations: [
                { number: "4821", title: "Prepare the release", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("Review")], worktree: "release/1.4", transcript: "turn-report", hasPlan: true, provider: "Codex",
                  providerId: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                { number: "4822", title: "Prepare the hotfix", status: KetTheme.ConversationStatus.NeedsInput,
                  tags: [tag("Review")], worktree: "fix/hotfix", transcript: "short", hasPlan: true,
                  provider: "Codex", providerId: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                { number: "2035", title: "Refine navigation", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("UI")], worktree: "develop", transcript: "activity" },
                // A finished turn and a running one, both closed by the work report (Fixtures.turnReport).
                { number: "8213", title: "Finish the repository picker", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("UI")], worktree: "develop", transcript: "report" },
                { number: "8215", title: "Follow up on the new UI", status: KetTheme.ConversationStatus.Working,
                  tags: [tag("UI")], worktree: "develop", transcript: "report-running" },
                { number: "9410", title: "Review the startup flow", status: KetTheme.ConversationStatus.Working,
                  tags: [tag("Performance"), tag("Review")], worktree: "develop", transcript: "running" },
                { number: "3372", title: "Atlas: Onboarding experience", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("Atlas")], worktree: "develop", transcript: "atlas" },
                { number: "9079", title: "Check this worktree", status: KetTheme.ConversationStatus.Completed,
                  tags: [], worktree: "feature-onboarding", transcript: "short" },
                // The turn finished with a request for approval: "Waiting for you", not Completed.
                { number: "7542", title: "Check this worktree", status: KetTheme.ConversationStatus.AwaitingReply,
                  tags: [], worktree: "fix-scroll-anchor", transcript: "short" },
                { number: "3366", title: "Review feature onboarding", status: KetTheme.ConversationStatus.NeedsInput,
                  tags: [tag("Review")], worktree: "feature-onboarding", transcript: "question" },
                { number: "4583", title: "Review fix scroll anchor", status: KetTheme.ConversationStatus.Working,
                  tags: [tag("Review")], worktree: "fix-scroll-anchor", transcript: "running" },
                { number: "6120", title: "Profile the editor startup", status: KetTheme.ConversationStatus.Error,
                  tags: [tag("Performance"), tag("Bug")], worktree: "develop", transcript: "error" },
                // A turn the provider ended: the guide in the transcript (workspace/setup-expired…).
                { number: "7310", title: "Speed up the cold start", status: KetTheme.ConversationStatus.Error,
                  tags: [tag("Performance")], worktree: "develop", transcript: "setup", setup: "expired",
                  provider: "Claude", providerId: "claude", model: "Opus 5.5", effort: "Medium" },
                { number: "5904", title: "Draft release notes", status: KetTheme.ConversationStatus.Stopped,
                  tags: [tag("Docs")], worktree: "develop", transcript: "short" }
            ]
        },
        {
            name: "ketviet",
            conversations: [
                { number: "7992", title: "Refine navigation", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("Account"), tag("UI")], worktree: "main", transcript: "activity" },
                { number: "8503", title: "Review the startup flow", status: KetTheme.ConversationStatus.Working,
                  tags: [tag("Account"), tag("Bug"), tag("Review")], worktree: "main", transcript: "running" },
                // Every content type of one provider's journal (workspace/claude, codex, cursor).
                { number: "6120", title: "Fix the choppy sidebar spinner", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("Performance")], worktree: "main", transcript: "claude", provider: "Claude",
                  providerId: "claude", model: "Opus 5.5", effort: "Medium" },
                { number: "6188", title: "Journal size warning", status: KetTheme.ConversationStatus.Stopped,
                  tags: [tag("Bug")], worktree: "feat/journal-storage", transcript: "codex", provider: "Codex",
                  providerId: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                { number: "6203", title: "Trace the staging tenant flow", status: KetTheme.ConversationStatus.Error,
                  tags: [], worktree: "main", transcript: "cursor", provider: "Cursor",
                  providerId: "cursor", model: "Composer 2", effort: "Default" }
            ]
        },
        {
            name: "ketplus-app",
            conversations: [
                { number: "1187", title: "Question sheet", status: KetTheme.ConversationStatus.Completed,
                  tags: [], worktree: "develop", transcript: "short" },
                { number: "1204", title: "iPhone connection", status: KetTheme.ConversationStatus.NotStarted,
                  tags: [], worktree: "develop", transcript: "welcome" }
            ]
        }
    ]

    // The conversation's work report: what the agent reported through the `report` tool,
    // merged by key over four turns. The app counts it; cancelled items are not counted.
    readonly property var turnReport: ({
        title: "Repository picker and new UI follow-ups",
        turn: 4,
        activity: "Editing ExecutionWorktreeDialog.cpp",
        groups: [
            { title: "Pull requests", items: [
                { key: "pr-399", title: "#399 Interface zoom separate from font size", status: "done", change: "",
                  owner: "Agent", evidence: "Merged into feat/new-ui · review by @agent/7542 passed",
                  verified: ["Tests", "Build"] },
                { key: "pr-401", title: "#401 Mermaid labels meet AA contrast", status: "done", change: "",
                  owner: "Agent", evidence: "The 4.45:1 label now reaches 4.6:1 · sweep regression added · merged",
                  verified: ["Code", "Tests", "Build", "Capture"] }
            ] },
            { title: "Repository picker · #164", items: [
                { key: "picker-daemon", title: "The daemon lists another repository's worktrees", status: "done",
                  change: "new", owner: "Agent",
                  evidence: "Lists them without switching or enrolling the repository · daemon_worktree_context 2/2 passed",
                  verified: ["Code", "Tests"] },
                { key: "picker-dialog", title: "The dialog follows the mock flow", status: "active", change: "updated",
                  owner: "Agent", evidence: "Choose a repository, then one of its worktrees, or use the repository or create a worktree",
                  remaining: "Worktree counts, the window's call site and the dialog tests",
                  next: "Finish the dialog and run its tests", verified: ["Code"] },
                { key: "picker-adr", title: "ADR 0105 and the daemon version", status: "waiting", change: "",
                  owner: "Agent", next: "Write them once the dialog lands" }
            ] },
            { title: "Decisions", items: [
                { key: "issue-402", title: "#402 Retire the compatibility stylesheet", status: "waiting", change: "new",
                  owner: "You", remaining: "Now, or folded into #137–#139", next: "Choose when to do it" },
                { key: "update-notice", title: "Toasts and the update notice", status: "blocked", change: "",
                  owner: "You", remaining: "The notice needs approved wording", next: "Approve the notice wording" }
            ] },
            // Only cancelled items: the group reads Cancelled and adds nothing to the counts.
            { title: "Dropped", items: [
                { key: "legacy-picker", title: "Keep the old picker behind a setting", status: "cancelled", change: "",
                  owner: "Agent", evidence: "Replaced by the mock flow in turn 3" }
            ] }
        ]
    })

    readonly property var machines: ["This Mac", "Linux · build-node-01"]

    // Agent Workspace: models and permission modes in the composer toolbar.
    readonly property var models: ["Codex · GPT-5.6 Luna", "Codex · GPT-5.6 Sol", "Claude · Opus 5.5",
                                   "Claude · Sonnet 5.5"]
    readonly property var efforts: ["Light", "Medium", "High", "Extra high", "Maximum"]
    readonly property var permissions: ["Workspace", "Read only", "Full access"]

    // The composer's Mode menu, as KetPlus desktop lists it; keys 1–3 choose.
    readonly property var permissionModes: [
        { label: "Read only", icon: "lock", elevated: false,
          description: "Inspect files; stays in plan mode" },
        { label: "Workspace", icon: "folder", elevated: false,
          description: "Edit project files; asks before commands" },
        { label: "Full access", icon: "shield", elevated: true,
          description: "Skip permission prompts" }
    ]

    // Model & context: providers, their captions and models. KétRouter is not available yet.
    readonly property var providers: [
        { id: "codex", label: "Codex", caption: "Codex is ready. Claude and Cursor are available." },
        { id: "claude", label: "Claude", caption: "Claude uses the local Agent SDK sidecar." },
        { id: "cursor", label: "Cursor", caption: "Cursor lists models from the installed CLI (`agent models`)." },
        { id: "ketrouter", label: "KétRouter", caption: "KétRouter is not available yet." }
    ]
    readonly property var providerModels: ({
        codex: [
            { name: "GPT-5.6 Luna", description: "Fast frontier model for everyday coding", isDefault: true },
            { name: "GPT-5.6 Sol", description: "Deeper reasoning for long, multi-step tasks" },
            { name: "GPT-5.6 Mini", description: "Quick edits and small questions" }
        ],
        claude: [
            { name: "Default (recommended)", short: "Sonnet 5.5", description: "Sonnet 5.5 · Efficient for routine tasks", isDefault: true },
            { name: "Opus 5.5", description: "For complex work and everyday tasks" },
            { name: "Fable 5.1", description: "For your toughest challenges" }
        ],
        cursor: [
            { name: "Auto", description: "Cursor picks a model for each request", isDefault: true },
            { name: "Composer 2", description: "Cursor's own coding model" }
        ],
        ketrouter: []
    })
    // Auto-compact thresholds; 1M exceeds the 500k capacity, so it is unavailable.
    readonly property var compactChoices: [
        { label: "Auto", tokens: 0 }, { label: "272k", tokens: 272000 },
        { label: "500k", tokens: 500000 }, { label: "1M", tokens: 1000000 }, { label: "Custom", tokens: -1 }
    ]

    readonly property var suggestions: [
        { title: "Explain this repository", caption: "Architecture, modules and entry points",
          icon: "layers" },
        { title: "Review my changes", caption: "3 files changed on develop", icon: "compare" },
        { title: "Design a new screen", caption: "Start an Atlas from a prompt", icon: "palette" },
        { title: "Fix the failing tests", caption: "ctest reports 2 failures", icon: "terminal" }
    ]

    readonly property string activityAnswer:
        "## Transcript activity is explicit and easier to scan.\n\n"
        + "The conversation now keeps the result separate from the work performed:\n\n"
        + "- **Exact file and command actions** remain visible and expandable.\n"
        + "- **Images, browser visits, searches, and app usage** have dedicated activity treatments.\n"
        + "- **Unknown future events** fall back to a safe generic row instead of disappearing.\n\n"
        + "Intermediate reasoning and progress narration are not included in the transcript."

    readonly property string runningAnswer:
        "I'm tracing the startup path from `main()` to the first painted frame. So far the "
        + "workspace index and the provider handshake run in sequence; they can overlap.\n\n"
        + "```cpp\nauto index = workspace.indexAsync();\nauto session = providers.connect();\n"
        + "co_await whenAll(index, session);\n```"

    readonly property var changedFiles: [
        { name: "NavigationRail.qml", path: "modules/ai/src/agent/qml", added: 18, removed: 6 },
        { name: "WorktreeItem.qml", path: "modules/ai/src/agent/qml", added: 9, removed: 3 },
        { name: "design-system.md", path: "docs", added: 1, removed: 0 }
    ]

    readonly property var stagedFiles: [
        { name: "ThemeManager.cpp", path: "apps/desktop/src/ui", added: 4, removed: 2 }
    ]

    readonly property string sampleDiff:
        "@@ -18,16 +18,28 @@ WorktreeItem\n"
        + "     WorktreeItem {\n         id: worktreeRow\n         width: parent.width\n"
        + "-        color: \"transparent\"\n-        radius: 2\n"
        + "+        color: selected ? theme.accentSubtle\n+                        : \"transparent\"\n"
        + "+        radius: theme.radiusSmall\n \n         Column {\n             spacing: 4\n"
        + "             Text {\n                 text: worktree.branch\n"
        + "-                color: theme.textSecondary\n"
        + "+                color: selected ? theme.accent\n+                               : theme.textPrimary\n"
        + "             }\n"
        + "+            Text {\n+                visible: worktree.isRunning\n+                text: qsTr(\"Agent working\")\n"
        + "+                color: theme.success\n+                font.pixelSize: theme.textSmall\n+            }\n"
        + "         }\n     }\n"

    // Files & Editor.
    readonly property var explorer: [
        { name: "ketplus-desktop", folder: true, depth: 0, open: true, root: true },
        { name: "apps", folder: true, depth: 1, open: true },
        { name: "desktop", folder: true, depth: 2, open: true },
        { name: "src", folder: true, depth: 3, open: false },
        { name: "modules", folder: true, depth: 1, open: true },
        { name: "ai", folder: true, depth: 2, open: true },
        { name: "src / agent / qml", folder: true, depth: 3, open: true },
        { name: "NavigationRail.qml", depth: 4, status: "M", path: "modules/ai/src/agent/qml" },
        { name: "WorktreeItem.qml", depth: 4, status: "M", path: "modules/ai/src/agent/qml" },
        { name: "atlas", folder: true, depth: 2, open: false },
        { name: "docs", folder: true, depth: 1, open: true },
        { name: "architecture.md", depth: 2, path: "docs" },
        { name: "design-system.md", depth: 2, path: "docs" },
        { name: "licensing.md", depth: 2, path: "docs" },
        { name: "README.md", depth: 1, path: "" },
        { name: "CMakeLists.txt", depth: 1, path: "" }
    ]

    readonly property string navigationRail:
        "// NavigationRail.qml\nimport QtQuick\nimport QtQuick.Controls\n\nItem {\n"
        + "    id: navigationRail\n\n    property string activeWorktree: \"develop\"\n"
        + "    property var conversations: []\n\n    // Keep workspace context visible.\n"
        + "    width: theme.sidebarWidth\n\n    Column {\n        anchors.fill: parent\n"
        + "        spacing: theme.spacingSmall\n\n        RepositoryPicker {\n"
        + "            repository: workspace.repository\n"
        + "            onActivated: workspace.openPicker()\n        }\n\n        WorktreeList {\n"
        + "            model: workspace.worktrees\n            currentPath: activeWorktree\n"
        + "            onSelected: path => {\n                agent.selectWorktree(path)\n"
        + "            }\n        }\n\n        ConversationList {\n"
        + "            model: navigationRail.conversations\n        }\n    }\n}\n"

    readonly property string designSystem:
        "# Design system\n\nKetPlus uses **Két Design System** through the Két Qt kit.\n\n"
        + "- Colours come only from tokens.\n- Product metrics live in `ket::Profile`.\n"
        + "- Every component has a Qt Widgets and a Qt Quick twin.\n\n"
        + "## Code samples\n\nSyntax colours follow the fence's language.\n\n"
        + "```qml\n// A row in the worktree list.\nItem {\n    id: row\n    property bool selected: false\n"
        + "    height: 32\n    Text { text: qsTr(\"Agent working\"); color: theme.success }\n}\n```\n\n"
        + "```cpp\n#include <QString>\n\n/* Profile overrides\n   for the mock. */\n"
        + "ket::Profile profile = ket::Profile::ketPlus();\nprofile.codePixels = 14;\n"
        + "if (profile.monoFamily().isEmpty())\n    return 1;\n```\n\n"
        + "```sh\n# Build and capture the mock.\ncmake --build build/mock --parallel 8\n"
        + "KetPlusMock --screen=editor --state=markdown --dark --screenshot=\"$OUT/md.png\"\n```\n\n"
        + "```json\n{\n  \"name\": \"ket-qt\",\n  \"version\": \"0.4.0\",\n  \"private\": false\n}\n```\n\n"
        + "## Diagrams\n\nA Mermaid fence shows its source; switch it to Mermaid for the diagram, "
        + "and click the diagram to open it in the image viewer.\n\n"
        + "```mermaid\n" + mermaid.agentTurn + "\n```\n"

    // Repository & worktree picker.
    readonly property var pickerRepositories: [
        { name: "ketplus-desktop", count: 2, worktrees: [
            { branch: "develop", main: true, path: "/workspace/ketplus-desktop" },
            { branch: "fix/compaction-status", path: "/workspace/ketplus-desktop-compaction" }] },
        { name: "ketviet", count: 3, agent: true, worktrees: [
            { branch: "main", main: true, path: "/workspace/ketviet" },
            { branch: "feat/ketview-production-pilot", path: "/workspace/ketviet-pilot" },
            { branch: "fix/relay-timeout", path: "/workspace/ketviet-relay" }] },
        { name: "ketplus-app", count: 2, worktrees: [
            { branch: "develop", main: true, path: "/workspace/ketplus-app" },
            { branch: "feat/question-sheet", path: "/workspace/ketplus-app-questions" }] },
        { name: "ketplus", count: 1, worktrees: [
            { branch: "integration", main: true, path: "/workspace/ketplus" }] },
        { name: "ketchat-desktop", count: 1, worktrees: [
            { branch: "develop", main: true, path: "/workspace/ketchat-desktop" }] },
        { name: "fusetter", count: 1, worktrees: [
            { branch: "main", main: true, path: "/workspace/fusetter" }] }
    ]

    // Quick search (Spotlight).
    readonly property var spotlightGroups: [
        { title: "Files", items: [
            { id: "file:rail", title: "NavigationRail.qml", detail: "modules/ai/src/agent/qml",
              iconName: "file-code", screen: "editor" },
            { id: "file:worktree", title: "WorktreeItem.qml", detail: "modules/ai/src/agent/qml",
              iconName: "file-code", screen: "editor" },
            { id: "file:readme", title: "README.md", detail: "ketplus-desktop", iconName: "file",
              screen: "editor" }] },
        { title: "Conversations", items: [
            { id: "chat:2035", title: "Refine navigation", detail: "@agent/2035 · ketplus-desktop",
              iconName: "bot", screen: "workspace" },
            { id: "chat:9410", title: "Review the startup flow", detail: "@agent/9410 · develop",
              iconName: "bot", screen: "workspace" }] },
        { title: "Atlases", items: [
            { id: "atlas:onboarding", title: "Onboarding experience", detail: "2 flows · 4 screens",
              iconName: "layers", screen: "atlas" }] },
        { title: "Commands", items: [
            { id: "go:settings", title: "Open Settings", detail: "⌘ ,", iconName: "settings",
              screen: "settings" },
            { id: "go:connections", title: "Connect a phone", detail: "Connections",
              iconName: "display", screen: "connections" }] }
    ]

    // Background tasks above the composer.
    readonly property var backgroundTasks: [
        { title: "npm run dev", meta: "localhost:5173 · 2m 14s", kindIcon: "terminal", current: true,
          status: KetTheme.TaskStatus.Running },
        { title: "Review the diff", meta: "@agent/9410 · completed in 48s", kindIcon: "bot",
          status: KetTheme.TaskStatus.Completed },
        { title: "ctest --test-dir build", meta: "2 tests failed", kindIcon: "terminal",
          status: KetTheme.TaskStatus.Failed }
    ]

    // Atlas builder.
    readonly property var atlasFlows: [
        { title: "Getting started", screens: 2 },
        { title: "Reconnect a workspace", screens: 2 }
    ]

    // KetPlus Terminal sample output (mockups/terminal/demo/demo.sh): spans of [text, role],
    // where the role names a token colour; "" is text-main.
    readonly property var terminalLines: [
        [["$ ", "muted"], ["git status --short --branch", ""]],
        [["## develop", "success"], ["...", ""], ["origin/develop", "danger"], [" [ahead 2]", ""]],
        [[" M", "danger"], [" core/src/terminal/TerminalView.cpp", ""]],
        [[" M", "danger"], [" mockups/terminal/src/TerminalMockWindow.cpp", ""]],
        [["??", "danger"], [" mockups/terminal/demo/", ""]],
        [],
        [["$ ", "muted"], ["ctest --test-dir build -R terminal", ""]],
        [["1/3 Test #12: terminal_session ...........   ", ""], ["Passed", "success"], ["    0.41 sec", ""]],
        [["2/3 Test #13: terminal_view ..............   ", ""], ["Passed", "success"], ["    0.88 sec", ""]],
        [["3/3 Test #14: terminal_app ...............   ", ""], ["Passed", "success"], ["    1.02 sec", ""]],
        [],
        [["100% tests passed", "success"], [", 0 tests failed out of 3", ""]],
        [],
        [["info", "info"], ["  ", ""], ["warn", "warning"], ["  ", ""], ["error", "danger"], ["  ", ""], ["accent", "accent"], ["  ", ""], ["dim", "muted"]],
        [],
        [["➜ ", "success"], ["ketplus-desktop ", "info"], ["git:(", "accent"], ["develop", "danger"], [") ", "accent"]]
    ]

    // Settings › Providers, as SettingsProvidersPage reports them.
    // Settings → Providers when nothing needs the user. A provider with a setup state
    // (providerSetup) shows that state's badge, rows and guide instead.
    readonly property var providerStatus: [
        { id: "codex", name: "Codex CLI", note: "Codex CLI 0.162.1 is newer than the newest version KetPlus has been tested with (0.154.0). It should work; some features may not.",
          badge: "Newer than tested", tone: "warning",
          rows: [["Version", "0.162.1"], ["Supported", "0.142.1 or newer · tested up to 0.154.0"],
                 ["Location", "~/.local/bin/codex"], ["Update with", "npm install -g @openai/codex@latest"]] },
        { id: "claude", name: "Claude Agent SDK", note: "", badge: "Ready", tone: "success",
          rows: [["Version", "Agent SDK 0.3.289 · Claude Code 2.1.289"], ["Signed in", "Claude subscription"],
                 ["Location", "/Applications/KetPlus.app/Contents/Resources/claude-sidecar/index.mjs"]] },
        { id: "cursor", name: "Cursor CLI", note: "", badge: "Ready", tone: "success",
          rows: [["Version", "2026.9.18"], ["Supported", "2026.6.25 or newer · tested up to 2026.9.18"],
                 ["Location", "~/.local/bin/agent"]] }
    ]

    // What stops a provider and how the user fixes it. Each state follows what the daemon
    // reports: the version check (provider-compatibility.json: missing, outdated),
    // provider_unavailable, Claude's credential source ("none") and the provider's own text in
    // runtime.error (error, quoted as the provider wrote it). The commands are the daemon's
    // (CursorAdapter, the compatibility manifest) and the providers' documented ones. In the app,
    // Type in Terminal types a command for the user to run with Return; KetPlus never runs it.
    // Codex and Cursor sign-ins stay with their CLIs; only the optional Anthropic API key is
    // saved by KetPlus (Keychain, Settings → Providers). Here Type in Terminal only shows a toast.
    //   limits: rows for the Context & limits popover while this state lasts.
    //   blocking: the provider cannot start a turn, so the guide sits above the composer;
    //   otherwise a turn ended with it and the guide closes that turn in the transcript.
    //   source: who wrote `error` when it is not the provider (KetPlus's own messages).
    //   summary: the reason as Settings → Providers says it, outside any turn.
    //   transient: about one model at one moment, so Settings does not flag the provider.
    //   topic: the failed conversation's title, when it is not "Speed up the cold start".
    //   A step with `turn` only applies in the conversation.
    readonly property var providerSetup: ({
        "cli-missing": {
            provider: "cursor", name: "Cursor", model: "Composer 2", blocking: true, tone: "danger",
            badge: "Not installed", title: "Cursor CLI isn't installed",
            reason: "KetPlus runs Cursor through the Cursor CLI on this Mac and couldn't find it. The Cursor app alone is not enough.",
            error: "Cursor CLI was not found.", source: "KetPlus",
            steps: [
                { text: "Install the Cursor CLI in a terminal.", command: "curl https://cursor.com/install -fsS | bash" },
                { text: "Sign in with the account that has your Cursor plan. A browser window opens.", command: "agent login" },
                { text: "Check again. Your message stays in the composer." }
            ],
            primary: "Check again", secondary: "Use Codex instead", secondaryAction: "switch:codex",
            still: "Still not found. Run agent --version in a terminal: if it fails there too, the install didn't finish. If it works there, quit and reopen KetPlus so it sees the new location.",
            ready: "Cursor CLI 2026.9.18 is ready",
            rows: [["Supported", "2026.6.25 or newer · tested up to 2026.9.18"], ["Looked for", "agent and cursor-agent on PATH"]]
        },
        "cli-outdated": {
            provider: "codex", name: "Codex", model: "GPT-5.6 Luna", blocking: true, tone: "danger",
            badge: "Update needed", title: "Codex CLI 0.139.0 is too old",
            reason: "KetPlus needs Codex CLI 0.142.1 or newer to talk to it. Updating keeps your Codex sign-in and settings.",
            steps: [
                { text: "Update the Codex CLI in a terminal.", command: "npm install -g @openai/codex@latest" },
                { text: "Installed it with Homebrew? Use this instead.", command: "brew upgrade codex" },
                { text: "Check again." }
            ],
            primary: "Check again", secondary: "Use Claude instead", secondaryAction: "switch:claude",
            ready: "Codex CLI 0.154.0 is ready",
            rows: [["Version", "0.139.0"], ["Supported", "0.142.1 or newer · tested up to 0.154.0"], ["Location", "~/.local/bin/codex"]]
        },
        "signed-out": {
            provider: "codex", name: "Codex", model: "GPT-5.6 Luna", blocking: true, tone: "danger",
            badge: "Not signed in", title: "Codex CLI isn't signed in",
            reason: "Codex is installed but has no account on this Mac. KetPlus uses the CLI's own sign-in and does not store Codex credentials.",
            steps: [
                { text: "Sign in in a terminal. A browser window opens for your ChatGPT account.", command: "codex login" },
                { text: "Using an OpenAI API key instead? Pass it on standard input.", command: "printenv OPENAI_API_KEY | codex login --with-api-key" },
                { text: "Check again." }
            ],
            primary: "Check again", secondary: "Use Claude instead", secondaryAction: "switch:claude",
            ready: "Codex is signed in",
            rows: [["Version", "0.154.0"], ["Signed in", "No"], ["Location", "~/.local/bin/codex"]]
        },
        "claude-signed-out": {
            provider: "claude", name: "Claude", model: "Opus 5.5", blocking: true, tone: "danger",
            badge: "Not signed in", title: "Claude needs a sign-in",
            reason: "Claude runs from the Agent SDK bundled with KetPlus. It can use your Claude subscription, signed in through Claude Code, or your own Anthropic API key.",
            steps: [
                { text: "Subscription: install Claude Code if you don't have it.", command: "curl -fsSL https://claude.ai/install.sh | bash" },
                { text: "Start it, type /login and finish in the browser.", command: "claude" },
                { text: "API key instead: add it in Settings › Providers › Claude API access." }
            ],
            primary: "Check again", secondary: "Add an API key", secondaryAction: "settings/providers",
            ready: "Claude is signed in",
            rows: [["Version", "Agent SDK 0.3.289 · Claude Code 2.1.289"], ["Signed in", "No credential found"]]
        },
        "sidecar-missing": {
            provider: "claude", name: "Claude", model: "Opus 5.5", blocking: true, tone: "danger",
            badge: "Damaged install", title: "KetPlus can't find its Claude runtime",
            reason: "The Claude Agent SDK that ships inside KetPlus is missing, often after an interrupted update. Your conversations are safe.",
            error: "Claude Agent SDK sidecar was not found.", source: "KetPlus",
            steps: [
                { text: "Install the latest KetPlus over this copy from Settings › Updates, or download it again." },
                { text: "Reopen KetPlus. Nothing else needs to change." }
            ],
            primary: "Check for updates", primaryAction: "settings/updates",
            secondary: "Use Codex instead", secondaryAction: "switch:codex",
            ready: "Claude is ready",
            rows: [["Version", "Not found"], ["Expected at", "/Applications/KetPlus.app/Contents/Resources/claude-sidecar/index.mjs"]]
        },
        "expired": {
            provider: "claude", name: "Claude", model: "Opus 5.5", blocking: false, tone: "danger",
            badge: "Sign-in expired", title: "Your Claude sign-in expired",
            reason: "Claude stopped this turn because its sign-in could not be renewed. Your message and the work so far are kept.",
            summary: "Claude's sign-in expired and could not be renewed. New turns fail until you sign in again.",
            error: "Failed to authenticate: OAuth session expired and could not be refreshed",
            steps: [
                { text: "Sign in again: start Claude Code in a terminal, type /login and finish in the browser.", command: "claude" },
                { text: "Retry. KetPlus sends your last message again.", turn: true }
            ],
            primary: "Retry", secondary: "Use an API key", secondaryAction: "settings/providers",
            prompt: "Find what makes the first window take four seconds on a cold start.",
            rows: [["Version", "Agent SDK 0.3.289 · Claude Code 2.1.289"], ["Signed in", "Expired at 09:12"]]
        },
        "key-rejected": {
            provider: "claude", name: "Claude", model: "Opus 5.5", blocking: false, tone: "danger",
            badge: "Key rejected", title: "Anthropic rejected your API key",
            reason: "KetPlus is set to use your Anthropic API key, and Anthropic refused it. The key may have been revoked, or its organization disabled.",
            summary: "Anthropic refuses the saved API key, so Claude turns fail. The key may have been revoked, or its organization disabled.",
            error: "Anthropic rejected this API key.", source: "KetPlus",
            steps: [
                { text: "Create a new key in Anthropic Console › API keys." },
                { text: "Paste it in Settings › Providers › Claude API access and save." },
                { text: "Retry, or set Authentication back to your Claude subscription.", turn: true }
            ],
            primary: "Open Settings", primaryAction: "settings/providers", secondary: "Retry", secondaryAction: "retry",
            prompt: "Find what makes the first window take four seconds on a cold start.",
            rows: [["Version", "Agent SDK 0.3.289 · Claude Code 2.1.289"], ["Signed in", "API key · rejected"]]
        },
        "limit": {
            provider: "codex", name: "Codex", model: "GPT-5.6 Luna", blocking: false, tone: "warning",
            badge: "Limit reached", title: "You've used this week's Codex limit",
            reason: "Codex stopped here until your weekly limit resets on Nov 13 at 07:00. Nothing is lost; continue this conversation after the reset.",
            summary: "This week's Codex limit is used up. Codex turns fail until it resets on Nov 13 at 07:00.",
            error: "You've hit your weekly limit · resets Nov 13 at 7am",
            steps: [
                { text: "Wait for the reset. Limits in the composer shows the time left." },
                { text: "Or carry on in a new conversation with Claude or Cursor.", turn: true }
            ],
            primary: "View limits", primaryAction: "limits", secondary: "New conversation with Claude", secondaryAction: "new:claude",
            limits: [{ label: "Session · 5 hours", resets: "Resets Fri, 06 Nov  9:40 PM", used: 12 },
                     { label: "Weekly · all models", resets: "Resets Fri, 13 Nov  7:00 AM", used: 100 }],
            prompt: "Add a size limit and a warning to the journal, with a setting for it.", topic: "Journal size limit",
            rows: [["Version", "0.154.0"], ["Weekly limit", "100% used · resets Nov 13, 07:00"]]
        },
        "capacity": {
            provider: "codex", name: "Codex", model: "GPT-5.6 Luna", blocking: false, tone: "warning",
            badge: "Model busy", title: "GPT-5.6 Luna is at capacity",
            reason: "OpenAI couldn't run this model right now. Your account and limits are fine.", transient: true,
            error: "Selected model is at capacity. Please try a different model.",
            steps: [
                { text: "Choose another model for this conversation, then retry." },
                { text: "Or retry later with the same model." }
            ],
            primary: "Choose model", primaryAction: "model", secondary: "Retry", secondaryAction: "retry",
            prompt: "Add a size limit and a warning to the journal, with a setting for it.", topic: "Journal size limit",
            rows: [["Version", "0.154.0"], ["Last error", "Model at capacity · 10:42"]]
        },
        "plan-blocked": {
            provider: "cursor", name: "Cursor", model: "Composer 2", blocking: false, tone: "warning",
            badge: "Not on plan", title: "The Cursor CLI account can't use Composer 2",
            reason: "The Cursor app and the Cursor CLI sign in separately. The CLI is signed in to an account whose plan doesn't include this model.",
            summary: "The Cursor CLI is signed in to an account whose plan doesn't include Composer 2. The Cursor app signs in separately.",
            error: "Cursor blocked this model for the Cursor Agent CLI account (dev@example.com).",
            steps: [
                { text: "Sign the CLI in to the account that has this model.", command: "agent login" },
                { text: "Or choose Auto, or another model on the CLI's plan, and retry.", turn: true }
            ],
            primary: "Choose model", primaryAction: "model", secondary: "Retry", secondaryAction: "retry",
            prompt: "Trace how a staging tenant is created, end to end.", topic: "Trace the staging tenant flow",
            rows: [["Version", "2026.9.18"], ["Signed in", "dev@example.com · plan without Composer 2"]]
        }
    })

    // Settings → Storage. Proportions follow a six-week journal measured on a working Mac
    // (most of the file was free pages SQLite never returned); the conversations are made up.
    readonly property var journalStorage: ({
        totalGb: 9.0, limitGb: 5, warnPercent: 80, oldest: "24 Sep 2026", lastCompaction: "Never",
        parts: [
            { label: "Conversation history", detail: "Messages, tool calls and command output", gb: 2.3 },
            { label: "Generated images", detail: "422 images at full size", gb: 1.2 },
            { label: "Activity and usage index", detail: "Activity groups, usage samples", gb: 0.3 },
            { label: "Free space", detail: "Left by deleted data; compacting returns it to macOS", gb: 5.2 }
        ],
        largest: [
            { number: "5498", title: "Port the image pipeline", provider: "Codex", size: "349 MB", note: "318 generated images" },
            { number: "5208", title: "Browser screenshot audit", provider: "Codex", size: "123 MB", note: "2,140 browser tool results" },
            { number: "7045", title: "Refactor the daemon journal", provider: "Codex", size: "115 MB", note: "6,800 commands" },
            { number: "8854", title: "Review the startup flow", provider: "Claude", size: "80 MB", note: "5,200 commands" }
        ]
    })
}
