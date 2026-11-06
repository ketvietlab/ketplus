// SPDX-License-Identifier: MPL-2.0
pragma Singleton
import QtQuick
import Ket

// Deterministic sample data for every screen. Nothing here talks to a daemon, Git or a
// provider; the mock only presents these values.
QtObject {
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
                { number: "2035", title: "Refine navigation", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("UI")], worktree: "develop", transcript: "activity" },
                { number: "9410", title: "Review the startup flow", status: KetTheme.ConversationStatus.Working,
                  tags: [tag("Performance"), tag("Review")], worktree: "develop", transcript: "running" },
                { number: "3372", title: "Atlas: Onboarding experience", status: KetTheme.ConversationStatus.Completed,
                  tags: [tag("Atlas")], worktree: "develop", transcript: "atlas" },
                { number: "9079", title: "Check this worktree", status: KetTheme.ConversationStatus.Completed,
                  tags: [], worktree: "feature-onboarding", transcript: "short" },
                { number: "7542", title: "Check this worktree", status: KetTheme.ConversationStatus.Completed,
                  tags: [], worktree: "fix-scroll-anchor", transcript: "short" },
                { number: "3366", title: "Review feature onboarding", status: KetTheme.ConversationStatus.NeedsInput,
                  tags: [tag("Review")], worktree: "feature-onboarding", transcript: "question" },
                { number: "4583", title: "Review fix scroll anchor", status: KetTheme.ConversationStatus.Working,
                  tags: [tag("Review")], worktree: "fix-scroll-anchor", transcript: "running" },
                { number: "6120", title: "Profile the editor startup", status: KetTheme.ConversationStatus.Error,
                  tags: [tag("Performance"), tag("Bug")], worktree: "develop", transcript: "error" },
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
                  tags: [tag("Account"), tag("Bug"), tag("Review")], worktree: "main", transcript: "running" }
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
        + "- Every component has a Qt Widgets and a Qt Quick twin.\n"

    // Manage worktrees.
    readonly property var worktrees: [
        { name: "develop", conversations: 3, working: 1, state: "Uncommitted changes", dirty: true,
          path: "/workspace/ketplus-desktop" },
        { name: "feature-onboarding", conversations: 2, working: 1, state: "Clean", dirty: false,
          path: "/workspace/ketplus-desktop-onboarding" },
        { name: "fix-scroll-anchor", conversations: 2, working: 1, state: "Clean", dirty: false,
          path: "/workspace/ketplus-desktop-scroll" }
    ]

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
              iconName: "display", screen: "connections" },
            { id: "go:worktrees", title: "Manage worktrees", detail: "Workspace",
              iconName: "branch", screen: "worktrees" }] }
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
}
