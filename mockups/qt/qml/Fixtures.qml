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
    // A member's role in its team (Fixtures.teams), drawn as a locked tag beside its
    // conversation. The team sets it; conversations carry no user tags (ADR 0117).
    // A team has one leader and this many developers at most (and one at least).
    readonly property int mostDevelopers: 5
    // T-23: a team has one or two reviewers.
    readonly property int mostReviewers: 2
    // ADR 0114, open point 2: Settings → Teams. A new team's automatic-turn budget starts from
    // this value; each team then keeps its own, which changing this one leaves alone.
    property int turnBudget: 40
    readonly property int fewestTurns: 5
    readonly property int mostTurns: 200
    // The daemon's refusal, in the same words, for a value it would refuse.
    function turnBudgetError(text) {
        const trimmed = String(text).trim()
        const value = Number(trimmed)
        return /^[0-9]+$/.test(trimmed) && value >= fewestTurns && value <= mostTurns ? ""
            : "Automatic turns are a whole number from " + fewestTurns + " to " + mostTurns + "."
    }
    readonly property var roleColors: ({ leader: "#b4436c", developer: "#5b6b7f", reviewer: "#7c5cd6" })
    function roleTag(role) {
        return { label: role.charAt(0).toUpperCase() + role.slice(1), color: roleColors[role] || "#5b6b7f" }
    }

    // The goal --open=new-team-goal types: paragraphs of context, a link and a mention.
    readonly property string longTeamGoal: "Make the first run explain itself: a new user should reach a first answer without reading the guide.\n\n"
        + "Context: the welcome tour was cut in 1.3 and support tickets doubled (see https://cinder-trail.example/issues/139). "
        + "@agent/8213 rebuilt the repository picker and knows where onboarding starts.\n\n"
        + "Out of scope: the account flow and pricing."
    readonly property var repositories: [
        {
            name: "cinder-trail",
            conversations: [
                { number: "4821", title: "Release handoff", status: KetTheme.ConversationStatus.Completed,
                  canReview: true, worktree: "release/1.4", transcript: "turn-report", hasPlan: true, provider: "Codex",
                  providerId: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                { number: "2035", title: "Trail picker focus", status: KetTheme.ConversationStatus.Completed,
                  worktree: "develop", transcript: "activity", provider: "Cursor",
                  providerId: "cursor", model: "Composer 2", effort: "Default" },
                // A finished turn and a running one, both closed by the work report (Fixtures.turnReport).
                { number: "8213", title: "Verify trail picker", status: KetTheme.ConversationStatus.Completed,
                  unread: true, worktree: "develop", transcript: "report", provider: "Codex",
                  providerId: "codex", model: "GPT-5.6 Sol", effort: "Medium" },
                { number: "8215", title: "Offline settings", status: KetTheme.ConversationStatus.Working,
                  worktree: "develop", transcript: "report-running", provider: "Claude",
                  providerId: "claude", model: "Opus 5.5", effort: "Medium" },
                { number: "9410", title: "Profile cold start", status: KetTheme.ConversationStatus.Working,
                  canReview: true, worktree: "develop", transcript: "running" },
                { number: "3372", title: "Atlas Builder", status: KetTheme.ConversationStatus.Completed,
                  worktree: "develop", transcript: "atlas" },
                { number: "3366", title: "Offline onboarding", status: KetTheme.ConversationStatus.NeedsInput,
                  canReview: true, worktree: "feat/offline-onboarding", transcript: "question" },
                { number: "4822", title: "Prepare the hotfix", status: KetTheme.ConversationStatus.NeedsInput,
                  canReview: true, worktree: "fix/hotfix", transcript: "short", hasPlan: true,
                  provider: "Codex", providerId: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                { number: "9079", title: "Check this worktree", status: KetTheme.ConversationStatus.Completed,
                  worktree: "feat/offline-onboarding", transcript: "short" },
                // The turn finished with a request for approval: "Waiting for you", not Completed.
                { number: "7542", title: "Check this worktree", status: KetTheme.ConversationStatus.AwaitingReply,
                  worktree: "fix/trail-picker", transcript: "short" },
                // The reviewer of the Release 1.4 team (Fixtures.teams). A reviewer runs on Claude.
                { number: "4583", title: "Picker review", status: KetTheme.ConversationStatus.Working,
                  canReview: true, worktree: "fix/trail-picker", transcript: "running",
                  provider: "Claude", providerId: "claude", model: "Opus 5.5", effort: "High" },
                { number: "6120", title: "Profile the editor startup", status: KetTheme.ConversationStatus.Error,
                  worktree: "develop", transcript: "error" },
                // A turn the provider ended: the guide in the transcript (workspace/setup-expired…).
                { number: "7310", title: "Speed up the cold start", status: KetTheme.ConversationStatus.Error,
                  worktree: "develop", transcript: "setup", setup: "expired",
                  provider: "Claude", providerId: "claude", model: "Opus 5.5", effort: "Medium" },
                { number: "5904", title: "Draft release notes", status: KetTheme.ConversationStatus.Stopped,
                  worktree: "develop", transcript: "short" },
                // The rest of the Docs refresh team (teams).
                { number: "5911", title: "Rewrite the build guide", status: KetTheme.ConversationStatus.Completed,
                  worktree: "docs/1.4", transcript: "short" },
                { number: "5923", title: "Review the user guide", status: KetTheme.ConversationStatus.Completed,
                  canReview: true, worktree: "docs/1.4", transcript: "short",
                  provider: "Claude", providerId: "claude", model: "Opus 5.5", effort: "High" }
            ]
        },
        {
            name: "cinder-trail-lab",
            conversations: [
                { number: "7992", title: "Trail picker focus", status: KetTheme.ConversationStatus.Completed,
                  worktree: "main", transcript: "activity" },
                { number: "8503", title: "Profile cold start", status: KetTheme.ConversationStatus.Working,
                  canReview: true, worktree: "main", transcript: "running" },
                // Every content type of one provider's journal (workspace/claude, codex, cursor).
                { number: "6120", title: "Fix the choppy sidebar spinner", status: KetTheme.ConversationStatus.Completed,
                  worktree: "main", transcript: "claude", provider: "Claude",
                  providerId: "claude", model: "Opus 5.5", effort: "Medium" },
                { number: "6188", title: "Journal size warning", status: KetTheme.ConversationStatus.Stopped,
                  worktree: "feat/journal-storage", transcript: "codex", provider: "Codex",
                  providerId: "codex", model: "GPT-5.6 Luna", effort: "Medium" },
                { number: "6203", title: "Trace an offline sync failure", status: KetTheme.ConversationStatus.Error,
                  worktree: "main", transcript: "cursor", provider: "Cursor",
                  providerId: "cursor", model: "Composer 2", effort: "Default" },
                { number: "1187", title: "Offline note prompts", status: KetTheme.ConversationStatus.Completed,
                  worktree: "develop", transcript: "short" },
                { number: "1204", title: "Pair a field phone", status: KetTheme.ConversationStatus.NotStarted,
                  worktree: "develop", transcript: "welcome" }
            ]
        }
    ]

    // The conversation's work report: what the agent reported through the `report` tool,
    // merged by key over four turns. The app counts it; cancelled items are not counted.
    readonly property var turnReport: ({
        title: "Trail picker release checks",
        turn: 4,
        activity: "Running picker tests on Linux",
        groups: [
            { title: "Implementation", items: [
                { key: "picker-focus", title: "Keep focus after filtering", status: "done", change: "updated",
                  owner: "Agent", evidence: "2 picker tests passed on macOS",
                  verified: ["Code", "Tests"] }
            ] },
            { title: "Release verification", items: [
                { key: "picker-dialog", title: "Verify offline on Linux", status: "blocked", change: "new",
                  owner: "You", remaining: "The field dataset needs approval for testing.",
                  next: "Choose a dataset in the conversation" },
                { key: "picker-docs", title: "Update the release checklist", status: "done", change: "new",
                  owner: "Agent", evidence: "Smoke commands and the offline gate are recorded.",
                  verified: ["Code"] }
            ] }
        ]
    })

    readonly property string accountName: "Tess Wayfern"
    readonly property string atlasPrompt: "Map the first offline field note in Cinder Trail, from choosing a saved trail to saving an observation."
    readonly property string datasetQuestion: "Which dataset should I use for the Linux offline check?"

    readonly property var machines: ["This Mac", "Linux · cinder-build"]

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
        { id: "codex", company: "OpenAI", label: "Codex", caption: "Codex is ready. Claude and Cursor are available." },
        { id: "claude", company: "Anthropic", label: "Claude", caption: "Claude uses the local Agent SDK sidecar." },
        { id: "cursor", company: "Anysphere", label: "Cursor", caption: "Cursor lists models from the installed CLI (`agent models`)." },
        { id: "copilot", company: "GitHub", product: "Copilot", label: "GitHub Copilot", caption: "GitHub Copilot uses the signed-in Copilot CLI." },
        { id: "ketrouter", company: "KétViệt", label: "KétRouter", caption: "KétRouter is not available yet." }
    ]
    // A provider in a select box: its company first, as "OpenAI – Codex"; short labels elsewhere.
    function providerChoice(entry) { return entry.company ? entry.company + " – " + (entry.product || entry.label) : entry.label }
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
        copilot: [
            { name: "Auto", description: "Copilot picks a model for each request", isDefault: true },
            { name: "Claude Sonnet 5.5", description: "Through your Copilot plan" },
            { name: "GPT-5.6", description: "Through your Copilot plan" }
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
        "## Trail picker changes are ready to review\n\n"
        + "Filtering keeps focus on the results. The selected trail stays open until you choose another one.\n\n"
        + "The changes below cover the list, its row component, and the release checklist."

    readonly property string runningAnswer:
        "The first frame waits for the offline trail index. I’m measuring how much time we recover "
        + "if the picker opens with cached trails and refreshes the index in the background.\n\n"
        + "```cpp\nauto trails = trailStore.cachedTrails();\nshowTrailPicker(trails);\n"
        + "trailStore.refreshIndexAsync();\n```"

    readonly property var changedFiles: [
        { name: "TrailList.qml", path: "src/trails", added: 4, removed: 1, diff: sampleDiff },
        { name: "TrailRow.qml", path: "src/trails", added: 1, removed: 1,
          diff: "@@ -11,3 +11,3 @@ TrailRow\n     required property string title\n-    activeFocusOnTab: true\n+    activeFocusOnTab: visible && enabled\n     Accessible.name: title\n" },
        { name: "release-checklist.md", path: "docs", added: 1, removed: 0,
          diff: "@@ -7,2 +7,3 @@ Release checks\n - [x] Trail picker tests on macOS\n+- [ ] Offline smoke test on Linux\n - [ ] Approve the release build\n" }
    ]

    readonly property var stagedFiles: [
        { name: "offline-tests.cpp", path: "tests", added: 1, removed: 1,
          diff: "@@ -30,3 +30,3 @@ PickerTests::restoresCachedTrails\n     const auto trails = store.cachedTrails();\n-    QVERIFY(!trails.empty());\n+    QCOMPARE(trails.size(), 3);\n }\n" }
    ]

    readonly property string sampleDiff:
        "@@ -14,5 +14,8 @@ TrailList.qml\n"
        + "     TrailFilter {\n         id: filter\n         width: parent.width\n"
        + "-        onAccepted: trailList.currentIndex = 0\n"
        + "+        onAccepted: {\n"
        + "+            trailList.currentIndex = 0\n"
        + "+            trailList.forceActiveFocus()\n"
        + "+        }\n"
        + "     }\n"

    // Files & Editor.
    readonly property var explorer: [
        { name: "cinder-trail", folder: true, depth: 0, open: true, root: true },
        { name: "src", folder: true, depth: 1, open: true },
        { name: "trails", folder: true, depth: 2, open: true },
        { name: "TrailList.qml", depth: 3, status: "M", path: "src/trails" },
        { name: "TrailRow.qml", depth: 3, status: "M", path: "src/trails" },
        { name: "docs", folder: true, depth: 1, open: true },
        { name: "release-checklist.md", depth: 2, status: "M", path: "docs" },
        { name: "architecture.md", depth: 2, path: "docs" },
        { name: "design-system.md", depth: 2, path: "docs" },
        { name: "licensing.md", depth: 2, path: "docs" },
        { name: "tests", folder: true, depth: 1, open: true },
        { name: "offline-tests.cpp", depth: 2, path: "tests" },
        { name: "README.md", depth: 1, path: "" },
        { name: "CMakeLists.txt", depth: 1, path: "" }
    ]

    readonly property string trailList:
        "import QtQuick\nimport QtQuick.Controls\n\nItem {\n"
        + "    id: picker\n\n    required property var trailStore\n"
        + "    signal trailOpened(string trailId)\n\n"
        + "    Column {\n        anchors.fill: parent\n        spacing: 8\n\n"
        + "        TrailFilter {\n            id: filter\n            width: parent.width\n"
        + "            onAccepted: {\n                trailList.currentIndex = 0\n"
        + "                trailList.forceActiveFocus()\n            }\n        }\n\n"
        + "        ListView {\n            id: trailList\n            width: parent.width\n"
        + "            model: picker.trailStore.filter(filter.text)\n"
        + "            delegate: TrailRow {\n                width: ListView.view.width\n"
        + "                onActivated: picker.trailOpened(trailId)\n            }\n        }\n    }\n}\n"

    readonly property string trailRow:
        "import QtQuick\nimport QtQuick.Controls\n\n"
        + "ItemDelegate {\n    required property string trailId\n    required property string title\n"
        + "    text: title\n    activeFocusOnTab: visible && enabled\n    Accessible.name: title\n}\n"

    readonly property string releaseChecklist:
        "# Cinder Trail 1.4 · release checklist\n\n"
        + "The field journal should open a saved trail and record an observation with the network turned off.\n\n"
        + "## Release checks\n\n"
        + "- [x] Trail picker tests on macOS\n"
        + "- [ ] Offline smoke test on Linux\n"
        + "- [ ] Approve the release build\n\n"
        + "## Offline smoke test\n\n"
        + "Use the approved dataset. Disable Wi-Fi, open **Ember Ridge Loop**, and add a field note. "
        + "Restart the app and confirm the note reappears before reconnecting.\n\n"
        + "```sh\nctest --test-dir build -R 'trail_picker|offline_journal' --output-on-failure\n```\n\n"
        + "> Record the dataset and build number in the work report before approving the release."

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
        { name: "cinder-trail", count: 2, worktrees: [
            { branch: "develop", main: true, path: "/workspace/cinder-trail" },
            { branch: "fix/trail-picker", path: "/workspace/cinder-trail-picker" }] },
        { name: "cinder-trail-lab", count: 3, agent: true, worktrees: [
            { branch: "main", main: true, path: "/workspace/cinder-trail-lab" },
            { branch: "feat/sample-preview-pilot", path: "/workspace/cinder-trail-lab-pilot" },
            { branch: "fix/sync-timeout", path: "/workspace/cinder-trail-lab-relay" }] },
        { name: "cinder-trail-mobile", count: 2, worktrees: [
            { branch: "develop", main: true, path: "/workspace/cinder-trail-mobile" },
            { branch: "feat/question-sheet", path: "/workspace/cinder-trail-mobile-questions" }] },
        { name: "cinder-trail-notes", count: 1, worktrees: [
            { branch: "develop", main: true, path: "/workspace/cinder-trail-notes" }] },
        { name: "cinder-trail-maps", count: 1, worktrees: [
            { branch: "main", main: true, path: "/workspace/cinder-trail-maps" }] }
    ]

    // Quick search (Spotlight).
    readonly property var spotlightGroups: [
        { title: "Files", items: [
            { id: "file:rail", title: "TrailList.qml", detail: "src/trails",
              iconName: "file-code", screen: "editor" },
            { id: "file:worktree", title: "TrailRow.qml", detail: "src/trails",
              iconName: "file-code", screen: "editor" },
            { id: "file:readme", title: "README.md", detail: "cinder-trail", iconName: "file",
              screen: "editor" }] },
        { title: "Conversations", items: [
            { id: "chat:2035", title: "Trail picker focus", detail: "@agent/2035 · cinder-trail",
              iconName: "bot", screen: "workspace" },
            { id: "chat:9410", title: "Profile cold start", detail: "@agent/9410 · develop",
              iconName: "bot", screen: "workspace" }] },
        { title: "Atlases", items: [
            { id: "atlas:onboarding", title: "First field entry", detail: atlasSummary,
              iconName: "layers", screen: "atlas" }] },
        { title: "Commands", items: [
            { id: "go:settings", title: "Open Settings", detail: "⌘ ,", iconName: "settings",
              screen: "settings" },
            { id: "go:connections", title: "Connect a phone", detail: "Connections",
              iconName: "display", screen: "connections" }] }
    ]

    // Background tasks above the composer.
    readonly property var backgroundTasks: [
        { title: "Profile the trail index", meta: "cinder-build · 2m 14s", kindIcon: "terminal", current: true,
          status: KetTheme.TaskStatus.Running },
        { title: "Review the picker diff", meta: "@agent/4583 · completed in 48s", kindIcon: "bot",
          status: KetTheme.TaskStatus.Completed },
        { title: "Offline smoke test", meta: "Waiting for the field dataset", kindIcon: "terminal",
          status: KetTheme.TaskStatus.Failed }
    ]

    // Atlas builder.
    readonly property var atlasFlows: [
        {
            title: "Record a field note",
            summary: "Choose a saved trail, log an observation and confirm it is stored.",
            nodes: [
                { kind: "trails", title: "Saved trails", heading: "Choose a trail", context: "Offline library",
                  icon: "download", role: "Start", transition: "Open" },
                { kind: "note", title: "Field editor", heading: "New observation", context: "Offline mode",
                  icon: "wifi-off", role: "Input", transition: "Save" },
                { kind: "receipt", title: "Local receipt", heading: "Observation saved", context: "Field journal",
                  icon: "file-text", role: "End", status: "Saved on device",
                  detail: "Queued until you reconnect.", action: "Review sync" }
            ]
        },
        {
            title: "Sync after the hike",
            summary: "Review the local queue and confirm the note uploaded.",
            nodes: [
                { kind: "queue", title: "Sync queue", heading: "Ready to sync", context: "Back online",
                  icon: "clock", role: "Start", transition: "Sync" },
                { kind: "synced", title: "Sync receipt", heading: "Observation synced", context: "Field journal",
                  icon: "check", role: "End", status: "Up to date",
                  detail: "Available on your other devices.", action: "Back to trails" }
            ]
        }
    ]
    readonly property string atlasSummary: atlasFlows.reduce((total, flow) => total + flow.nodes.length, 0)
                                           + " screens · " + atlasFlows.length + " flows"

    // KetPlus Terminal sample output (mockups/terminal/demo/demo.sh): spans of [text, role],
    // where the role names a token colour; "" is text-main.
    readonly property var terminalLines: [
        [["$ ", "muted"], ["git status --short --branch", ""]],
        [["## develop", "success"], ["...origin/develop", "muted"]],
        [[" M", "danger"], [" src/trails/TrailList.qml", ""]],
        [[" M", "danger"], [" src/trails/TrailRow.qml", ""]],
        [[" M", "danger"], [" docs/release-checklist.md", ""]],
        [["M ", "success"], [" tests/offline-tests.cpp", ""]],
        [],
        [["$ ", "muted"], ["ctest --test-dir build -R trail_picker", ""]],
        [["1/2 Test #12: trail_picker_focus ..........  ", ""], ["Passed", "success"], ["   0.41 sec", ""]],
        [["2/2 Test #13: trail_picker_filter .........  ", ""], ["Passed", "success"], ["   0.32 sec", ""]],
        [["100% tests passed", "success"], [", 0 tests failed out of 2", ""]],
        [["cinder-trail ", "info"], ["git:(", "accent"], ["develop", "danger"], [") $ ", "accent"]]
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
          rows: [["Version", "Agent SDK 0.3.322 · Claude Code 2.1.322"], ["Signed in", "Claude subscription"],
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
            rows: [["Version", "Agent SDK 0.3.322 · Claude Code 2.1.322"], ["Signed in", "No credential found"]]
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
            rows: [["Version", "Agent SDK 0.3.322 · Claude Code 2.1.322"], ["Signed in", "Expired at 09:12"]]
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
            rows: [["Version", "Agent SDK 0.3.322 · Claude Code 2.1.322"], ["Signed in", "API key · rejected"]]
        },
        "limit": {
            provider: "codex", name: "Codex", model: "GPT-5.6 Luna", blocking: false, tone: "warning",
            badge: "Limit reached", title: "You've used your Codex limit",
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
            prompt: "Trace how a staging tenant is created, end to end.", topic: "Trace an offline sync failure",
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
            { number: "8854", title: "Profile cold start", provider: "Claude", size: "80 MB", note: "5,200 commands" }
        ]
    })

    // Teams (concept): conversations that share one goal. The leader, who also writes the
    // requirements, assigns tasks; each developer owns a lane of the code (path prefixes) and a
    // reviewer checks the lanes it is given. A task has one owner, who also holds its worktree,
    // and acceptance criteria only the leader changes. Questions for the user wait in
    // `decisions`; `updates` are task changes that woke nobody and reach the leader as one digest.
    // Members are conversations of `repositories` above, so their rows open them.
    // `teams/live`: a team as its members write it. Choices are sentences, a task carries a long
    // Markdown description, lanes and paths are long, and an update is a paragraph. The page and
    // its dialogs must stay inside their width with this content.
    // Synthetic long-form variant; no private repository tasks or source paths.
    readonly property var liveTeam: Object.assign({}, teams[0], {
        id: "pilot", name: "Sample release",
        goal: "Prepare a sample release with clear tasks, review criteria and decisions. "
              + "This fixture demonstrates layout only and contains no live project data."
    })

    // T-37 (ADR 0114 §10): the free space, in GB, under which the team page warns, and under
    // which it says builds should stop.
    readonly property int diskWarnGb: 20
    readonly property int diskStopGb: 12
    readonly property string unmovedConversationNote: "“Quick search” (Wait for startup or shutdown to finish, or answer the pending request before switching.) could not be moved to the main checkout and still points at the removed folder. Open it on another folder before its next turn."
    readonly property string dirtyWorktreeRefusal: "The worktree has uncommitted or untracked changes (2 files). Commit or discard them, then remove it again."
    readonly property string releaseBriefName: "Morning release brief"
    readonly property string releaseBriefPrompt: "Summarize the Cinder Trail release checks and surface any blockers before the 09:00 handoff."
    readonly property var releaseLead: repositories[0].conversations[0]

    function releaseMember(number, assignment) {
        const conversation = repositories[0].conversations.find(entry => entry.number === number)
        return Object.assign({
            number: conversation.number,
            title: conversation.title,
            provider: conversation.provider + " · " + conversation.model,
            lane: "",
            paths: []
        }, assignment)
    }

    readonly property var teams: [
        {
            id: "release", name: "Cinder Trail 1.4",
            goal: "Ship offline field notes. Keep the trail picker fast, prove notes survive a restart, and sign off the Linux release check.\n\n"
                  + "Context: the offline library now keeps the last selected trail and restores saved notes. "
                  + "Ask @agent/8213 about the bundled dataset and Linux smoke plan.\n\n"
                  + "Done means the keyboard review has passed, a note survives an offline restart, "
                  + "and the guide explains what happens to queued uploads.",
            repository: repositories[0].name, branch: releaseLead.worktree, paused: false, merges: "leader",
            turnBudget: 60, automaticTurns: 23,
            storage: { free: 64.2, worktrees: [
                { id: "T-2", path: "/workspace/cinder-trail-offline-notes", size: "4.2 GB" }] },
            members: [
                releaseMember("4821", { role: "leader", state: "working", now: "Tracking release checks" }),
                releaseMember("2035", { role: "developer", lane: "Trail picker", paths: ["src/trails/"],
                    state: "idle", now: "T-1 · review passed" }),
                releaseMember("8213", { role: "developer", lane: "Offline QA", paths: ["tests/"],
                    state: "blocked", now: "T-5 · waiting for D-1" }),
                releaseMember("8215", { role: "developer", lane: "Offline settings", paths: ["src/settings/"],
                    state: "working", now: "T-6 · preparing review" }),
                releaseMember("4583", { role: "reviewer", reviews: ["Trail picker", "Offline QA", "Offline settings"],
                    state: "idle", now: "T-1 · keyboard review passed" })
            ],
            tasks: [
                { id: "T-1", title: "Keep keyboard focus after filtering", owner: "2035", lane: "Trail picker",
                  state: "done", pr: "#412", branch: "fix/trail-picker", worktree: "", dependsOn: "",
                  criteria: [
                      { text: "Enter moves focus from the filter to the results", done: true },
                      { text: "Opening the selected trail works with the keyboard", done: true }],
                  history: [
                      { when: "Today, 10:12", text: "@agent/2035 submitted #412 for review" },
                      { when: "Today, 10:42", text: "@agent/4583 passed review: both keyboard checks passed" }] },
                { id: "T-2", title: "Restore a saved note after restart", owner: "8213", lane: "Offline QA",
                  state: "done", pr: "#405", branch: "fix/restore-note", worktree: "cinder-trail-offline-notes", dependsOn: "",
                  criteria: [
                      { text: "Restarting offline restores the note text and its trail", done: true },
                      { text: "Restoring a note does not queue a duplicate upload", done: true }],
                  history: [{ when: "Yesterday, 16:20", text: "@agent/4583 verified restore and sync; #405 merged" }] },
                { id: "T-3", title: "Verify trail picker with a large library", owner: "8213", lane: "Offline QA",
                  state: "doing", pr: "", branch: "test/large-trail-library", worktree: "cinder-trail-library", dependsOn: "",
                  outsideLane: { path: "src/trails/TrailList.qml", lane: "Trail picker", owner: "2035" },
                  criteria: [
                      { text: "Recently opened trails appear first", done: true },
                      { text: "Filtering preserves keyboard focus", done: true },
                      { text: "The picker handles 0, 1 and 200 saved trails", done: false }],
                  criteriaNote: "The leader added the 200-trail case this morning.",
                  history: [{ when: "Today, 11:30", text: "@agent/8213 found a picker change outside the Offline QA lane" }] },
                { id: "T-4", title: "Check the empty offline library", owner: "8213", lane: "Offline QA",
                  state: "todo", pr: "", branch: "", worktree: "", dependsOn: "T-3",
                  criteria: [{ text: "An empty library offers a way to save a trail", done: false }],
                  history: [{ when: "Today, 09:14", text: "@agent/4821 wrote the acceptance criteria" }] },
                { id: "T-5", title: "Verify an offline note on Linux", owner: "8213", lane: "Offline QA",
                  state: "blocked", pr: "", branch: "develop", worktree: "", dependsOn: "", decision: "D-1",
                  criteria: [
                      { text: "A note saves with the network off", done: false },
                      { text: "The note survives a restart", done: false }],
                  history: [{ when: "Today, 11:05", text: "The smoke test is waiting for the dataset choice" }] },
                { id: "T-6", title: "Show pending sync in settings", owner: "8215", lane: "Offline settings",
                  state: "review", pr: "#418", branch: "feat/pending-sync", worktree: "", dependsOn: "",
                  criteria: [
                      { text: "Settings show how many notes are waiting to sync", done: true },
                      { text: "The count clears once the queued notes upload", done: true }],
                  history: [{ when: "Today, 10:50", text: "@agent/8215 submitted #418 for review" }] },
                { id: "T-7", title: "Verify keyboard navigation in the field editor", owner: "", lane: "Trail picker",
                  state: "todo", pr: "", branch: "", worktree: "", dependsOn: "T-1",
                  criteria: [{ text: "Tab reaches the observation field and Save offline", done: false }],
                  history: [{ when: "Today, 09:20", text: "@agent/4821 wrote the acceptance criteria" }] },
                { id: "T-8", title: "Publish the offline release notes", owner: "4821", lane: "",
                  state: "todo", pr: "", branch: "", worktree: "", dependsOn: "T-1, T-3, T-5", decision: "D-2",
                  criteria: [
                      { text: "The Linux release gate passes", done: false },
                      { text: "The guide explains where queued notes appear", done: false }],
                  history: [{ when: "Today, 09:22", text: "@agent/4821 added the release gate as a dependency" }] }
            ],
            decisions: [
                { id: "D-1", question: datasetQuestion, detail: "The bundled set covers the release gate. The full QA set adds sync history.",
                  askedBy: releaseLead.number, task: "T-5", when: "8 min ago",
                  options: ["Bundled trail dataset", "Full QA dataset"], recommended: 0 },
                { id: "D-2", question: "Which languages should the offline guide ship in?",
                  detail: "The Vietnamese guide needs one more editorial review.",
                  askedBy: releaseLead.number, task: "T-8", when: "2 hours ago",
                  options: ["English only", "English and Vietnamese"], recommended: 0 }
            ],
            updates: [
                { icon: "check", text: "T-1 review passed · 2/2 criteria",
                  detail: "@agent/4583 · Keyboard focus and trail selection verified", when: "24 min ago" },
                { icon: "warning", text: "T-3 edited a file in the Trail picker lane", detail: "@agent/8213 · TrailList.qml", when: "30 min ago", warning: true },
                { icon: "clock", text: "T-5 is blocked on a decision", detail: "@agent/8213 · D-1 waits for you", when: "55 min ago" },
                { icon: "pull-request", text: "T-6 moved to In review", detail: "@agent/8215 · #418", when: "1 hour ago" }
            ]
        },
        {
            id: "docs", name: "Docs refresh",
            goal: "Bring the user guide in line with 1.4.",
            repository: "cinder-trail", branch: "docs/1.4", paused: false, merges: "user",
            turnBudget: 40, automaticTurns: 4,
            members: [
                { number: "5904", title: "Draft release notes", role: "leader", provider: "Claude · Opus 5.5",
                  lane: "", paths: [], state: "idle", now: "Waiting for its first task list" },
                { number: "5911", title: "Rewrite the build guide", role: "developer", provider: "Claude · Opus 5.5",
                  lane: "Guides", paths: ["docs/guide/"], state: "idle", now: "No task yet" },
                { number: "5923", title: "Review the user guide", role: "reviewer", provider: "Claude · Opus 5.5",
                  lane: "", paths: [], reviews: ["Guides"], state: "idle", now: "No review yet" }
            ],
            tasks: [], decisions: [], updates: []
        }
    ]
}
