// SPDX-License-Identifier: MPL-2.0
pragma Singleton
import QtQuick
import KetPlusMock

// One transcript per provider with every content type its journal events produce
// (measured on a working journal: Claude Agent SDK, Codex app-server and Cursor CLI). The
// text is made up; only the shapes follow the journal.
//
// Journal events → block types (TranscriptBlock):
//   user.message                         user { text, images, from }
//   turn.steered                         steer { text }
//   turn.queued (not yet started)        queued { text }
//   item agentMessage phase commentary   commentary { text }
//   item agentMessage phase final_answer answer { markdown, status }
//   items grouped by activity family     activity { summary, expanded, steps: [step] }
//   fileChange / Write / Edit totals     changes { files, note }
//   approval.requested kind input        question { header, question, options, multi,
//                                                    allowCustom, state, answer }
//   approval.requested other kinds       approval { tool, reason, command, state }
//   agent_message.state                  agent { kind, peer, state, expectReply, text }
//   contextCompaction, session.*         event { text, detail }
//   runtime.error will_retry             reconnect { text }
//   runtime.error                        error { title, message, action }
//   turn.completed                       turn { status, text }
//   compacted detail (issue #128)        trimmed { text }
// Steps (TranscriptActivityItem) by item type and tool:
//   commandExecution                     command / read / search / list (Codex commandActions)
//   Read, Write, Edit / fileChange       read / create / edit / delete / move
//   ketplus_browser.*                    browser       webSearch, WebSearch, WebFetch  web
//   mcpToolCall, mcp__*                  mcp           imageView  image
//   imageGeneration                      generate      reasoning  thinking
//   sleep, Monitor, ScheduleWakeup       wait          ToolSearch, TaskStop  tool
//   Skill                                skill         Task (Cursor)  subagent
//   EnterPlanMode                        plan
QtObject {
    readonly property string asset: "qrc:/qt/qml/KetPlusMock/assets/"
    function shot(name) { return { source: asset + "screenshot-" + name + ".png", caption: name } }

    readonly property string sidebarDiff: "@@ -548,7 +548,9 @@ Item {\n"
        + "     id: statusShine\n"
        + "-    readonly property bool animating: agentHost.motionEnabled && modelData.running\n"
        + "+    // Steps on the shared clock instead of a 60 fps animation.\n"
        + "+    readonly property bool animating: agentHost.motionEnabled && modelData.running\n"
        + "+        && Qt.application.state === Qt.ApplicationActive\n"
        + "     visible: animating\n"

    // #468: what team Release 1.4 (Fixtures.teams) sends into its members' conversations. The
    // daemon writes these messages (assignment, digest, review, decision), so each is a block,
    // not a paragraph. `workspace/team-*` opens the member's conversation with them.
    readonly property var teamAssignment: [
        { type: "day", text: "Today" },
        { type: "team", kind: "assignment", team: "Release 1.4", task: "T-3",
          title: "Repository picker: recent and pinned repositories",
          facts: ["Lane Repository picker", "feat/picker-recents", "Assigned by @agent/4821"],
          criteria: [
              { text: "The five most recent repositories come first", done: false },
              { text: "A pinned repository stays above the recent ones", done: false },
              { text: "Typing filters by name and by path", done: false },
              { text: "The list works with 0, 1 and 200 repositories", done: false }] },
        { type: "commentary", text: "Starting T-3 in its worktree. I'll read the picker model first." },
        { type: "activity", summary: "Explored 3 files", expanded: false, steps: [
              { kind: "read", title: "Read", detail: "apps/desktop/src/app/picker/RepositoryPickerModel.cpp" },
              { kind: "read", title: "Read", detail: "apps/desktop/src/app/picker/RepositoryPickerModel.h" },
              { kind: "read", title: "Read", detail: "apps/desktop/src/app/picker/RecentRepositories.cpp" }] },
        { type: "answer", status: "", markdown: "T-3 is **In progress**. Recents and pinned repositories are ordered; filtering is next." }
    ]
    readonly property var teamDigest: [
        { type: "day", text: "Today" },
        { type: "team", kind: "digest", team: "Release 1.4", title: "Ten changes since your last digest",
          groups: [
              { task: "T-1", title: "Sidebar destinations and their counts", state: "In review", lines: [
                    { text: "@agent/2035 opened #412" },
                    { text: "Moved to In review" },
                    { text: "@agent/4583 started round 1 of 3" }] },
              { task: "T-3", title: "Repository picker: recent and pinned repositories", state: "In progress", lines: [
                    { text: "Two of four criteria met" },
                    { text: "@agent/8213 edited ActivityBar.cpp, a file of the Navigation lane", needsYou: true }] },
              { task: "T-5", title: "Settings: zoom and text size", state: "Blocked", lines: [
                    { text: "@agent/8215 asked whether a change applies at once" },
                    { text: "D-1 waits for the user" }] },
              { task: "T-6", title: "Settings: the update reminder", state: "In review", lines: [
                    { text: "@agent/8215 opened #418" },
                    { text: "Moved to In review" }] },
              { task: "", title: "", state: "", lines: [
                    { text: "T-7 has no owner and T-1 is nearly done", needsYou: true }] }] },
        { type: "commentary", text: "Two things need me: the lane edit in T-3 and an owner for T-7." },
        { type: "answer", status: "", markdown: "T-7 goes to @agent/2035 after T-1. @agent/2035 keeps ActivityBar.cpp; @agent/8213 reverts its edit." }
    ]
    readonly property var teamReview: [
        { type: "day", text: "Today" },
        { type: "team", kind: "review", team: "Release 1.4", task: "T-1", round: 1,
          title: "Sidebar destinations and their counts",
          facts: ["By @agent/2035", "feat/nav-destinations into release/1.4", "#412"],
          criteria: [
              { text: "The four destinations match the mock at 100% and 125% zoom", done: true },
              { text: "Source Control shows the number of changed files", done: true },
              { text: "Every destination is reachable by keyboard", done: true }],
          diff: ["6 files, +214 −38", "apps/desktop/src/workspace/ActivityBar.cpp", "apps/desktop/src/workspace/SidebarDestinations.cpp",
                 "tests/SidebarDestinationsTest.cpp"] },
        { type: "commentary", text: "Reviewing T-1 against its three criteria, read-only." },
        { type: "answer", status: "", markdown: "Verdict: no findings. Every criterion holds at 100% and 125%." }
    ]
    readonly property var teamVerdict: [
        { type: "day", text: "Today" },
        { type: "team", kind: "verdict", team: "Release 1.4", task: "T-6", round: 1, passed: false,
          title: "Settings: the update reminder",
          facts: ["Round 1 of 3 found issues", "Back to In progress"],
          findings: [
              "The reminder opens over a running turn's composer; criterion 2 says it stays out of the way.",
              "Restart has no accessible name."] },
        { type: "commentary", text: "Two findings. Fixing both, then back to review." },
        { type: "answer", status: "", markdown: "Both findings fixed in `UpdateReminder.qml`; T-6 is **In review** again (round 2)." }
    ]
    readonly property var teamDecision: [
        { type: "day", text: "Today" },
        { type: "team", kind: "decision", team: "Release 1.4", task: "T-5", title: "D-1 answered",
          question: "Should a change of zoom or text size apply at once, or after Apply?",
          answer: "Apply at once", text: "T-5 can go on." },
        { type: "commentary", text: "Telling @agent/8215 that a change applies at once." },
        { type: "answer", status: "", markdown: "@agent/8215 resumes T-5: a change of zoom or text size applies at once." }
    ]

    // Marketing sample: an honest end-of-turn report, not a claim about a real release.
    readonly property var turnReport: [
        { type: "user", text: "Get the release ready. Tell me what is verified, what is blocked, and who owns the next step." },
        { type: "activity", summary: "Reviewed changes · ran tests · checked release readiness", expanded: false, steps: [
            { kind: "read", title: "Reviewed", detail: "Release checklist and pull request changes" },
            { kind: "command", title: "Ran regression tests", detail: "48 passed · 0 failed", duration: "12.4 s",
              command: "npm test", output: "Tests: 48 passed, 48 total" },
            { kind: "read", title: "Checked signing access", detail: "Release key unavailable on the build runner" }
        ] },
        { type: "answer", markdown: "**The fix is verified. The release is waiting on signing access.**\n\n"
            + "The code review and regression tests are complete. The package is built, but signing and device checks are still open.\n\n"
            + "| Task | Status | Evidence / remaining work | Next step & owner |\n"
            + "| --- | --- | --- | --- |\n"
            + "| Code review | 🟢 Complete | Changes reviewed; no open findings. | None. |\n"
            + "| Regression tests | 🟢 Verified | 48 tests passed; 0 failed. | None. |\n"
            + "| Release package | 🟢 Built | Package generated; not signed yet. | Agent: sign after access is restored. |\n"
            + "| Release signing | 🔴 Blocked | Build runner cannot access the release key. | Release owner: grant signing access. |\n"
            + "| Device checks | 🟡 Waiting | Signed build required; not tested on device. | Agent: install and verify after signing. |\n\n"
            + "**Next:** the release owner restores signing access. Then I sign the package and run device checks."
        },
        { type: "turn", status: "completed", text: "Turn complete · Release checks still open" }
    ]

    readonly property var claude: [
        { type: "day", text: "Thursday, 24 September" },
        { type: "user", text: "Profile why the sidebar keeps the CPU busy when nothing happens." },
        { type: "trimmed", text: "This turn is older than 30 days: 38 commands and 12 captures were reduced to their summaries (1.2 MB)." },
        { type: "turn", status: "completed", text: "6m 20s · 141k context · $2.08 session" },

        { type: "day", text: "Today" },
        { type: "user", text: "The sidebar spinner still looks choppy. Check it against this capture and fix it.",
          images: [shot("sidebar")] },
        { type: "commentary", text: "I'll read how the sidebar draws its working rows, then measure the idle CPU." },
        { type: "activity", summary: "Explored 3 files, searched code, ran 2 commands", steps: [
            { kind: "read", title: "Read", detail: "modules/ai/src/agent/qml/AgentSidebar.qml:540-600" },
            { kind: "search", title: "Searched", detail: "“Animation.Infinite” in modules/ai", command: "rg -n \"Animation.Infinite\" modules/ai",
              output: "modules/ai/src/agent/qml/AgentRunStatus.qml:33\nmodules/ai/src/agent/qml/AgentSidebar.qml:579\nmodules/ai/src/agent/qml/BackgroundTaskBar.qml:149" },
            { kind: "command", title: "Measure idle CPU", detail: "top -l 4 -s 2 -pid 41087", expanded: true,
              description: "Sample the mock's CPU while nothing happens.",
              command: "top -l 4 -s 2 -pid 41087 -stats pid,cpu", output: "41087  44.3\n41087  47.4\n41087  41.5\n41087  45.0" },
            { kind: "command", title: "Run tests", detail: "ctest --test-dir build -R sidebar", status: "failed", exitCode: 8, duration: "12.4 s",
              command: "ctest --test-dir build -R sidebar", output: "1/2 Test #31: sidebar_rows ........   Passed\n2/2 Test #32: sidebar_spin ........***Failed\n50% tests passed, 1 tests failed out of 2",
              outputLines: 214 }
        ] },
        { type: "question", header: "Spinner", state: "answered", question: "How should a working row animate?",
          answer: "Step on a shared clock (Recommended)" },
        { type: "activity", summary: "Used browser 6 times", steps: [
            { kind: "browser", title: "Opened", detail: "localhost:5173/sidebar" },
            { kind: "browser", title: "Observed the page", detail: "142 nodes · 3 running rows" },
            { kind: "browser", title: "Clicked", detail: "“Review the startup flow”" },
            { kind: "browser", title: "Waited for", detail: "text “Working”", status: "failed", error: "browser_wait: timed out after 10 s" },
            { kind: "browser", title: "Captured 2 screenshots", detail: "1440 × 900 · light, dark", expanded: true,
              images: [shot("sidebar"), shot("editor")] },
            { kind: "browser", title: "Compared with the reference", detail: "0.4% of pixels differ" }
        ] },
        { type: "activity", summary: "Searched the web, loaded tools, used a skill", expanded: false, steps: [
            { kind: "web", title: "Searched the web", detail: "Qt Quick RotationAnimator render loop CPU",
              results: [{ title: "Qt Quick Scene Graph", url: "https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html" },
                        { title: "Animator QML Type", url: "https://doc.qt.io/qt-6/qml-qtquick-animator.html" }] },
            { kind: "web", title: "Fetched", detail: "doc.qt.io/qt-6/qml-qtquick-frameanimation.html" },
            { kind: "tool", title: "Loaded tools", detail: "Monitor, ScheduleWakeup" },
            { kind: "skill", title: "Used skill", detail: "enterprise-grade" }
        ] },
        { type: "agent", kind: "request", peer: { number: "8941", title: "DS Icon", provider: "Claude" },
          state: "replied", expectReply: true, text: "Is the stepped spinner fine at 20 steps a second?" },
        { type: "user", from: { number: "8941", title: "DS Icon", provider: "Claude" },
          text: "20 steps a second reads as smooth at 14 px. Keep 4 behind another app." },
        { type: "user", from: { number: "", title: "Example team", provider: "" },
          text: "You asked to check again: synthetic build status.\n" + "A completed check has no warnings; the remaining check is still running.\n".repeat(8) },
        { type: "event", text: "Context compacted", detail: "Older steps are summarised; the conversation continues." },
        { type: "steer", text: "Also check dark mode before you finish." },
        { type: "activity", summary: "Edited 2 files, ran tests", steps: [
            { kind: "edit", title: "Edited", detail: "modules/ai/src/agent/qml/AgentSidebar.qml  +3 −1", expanded: true, diff: sidebarDiff },
            { kind: "create", title: "Created", detail: "modules/ai/src/agent/qml/SpinClock.qml  +48" },
            { kind: "wait", title: "Waited for the build", detail: "Monitor · 1m 12s" },
            { kind: "command", title: "Run tests", detail: "ctest --test-dir build -R sidebar", duration: "9.8 s",
              command: "ctest --test-dir build -R sidebar", output: "100% tests passed, 0 tests failed out of 2" },
            { kind: "tool", title: "Stopped a background task", detail: "dev server · port 5173" }
        ] },
        { type: "answer", markdown: "The sidebar now steps every working row on **one shared clock**: 20 steps a second in front, 4 behind another app.\n\n| Build | Idle CPU |\n|---|---|\n| Before | 41–47% |\n| After | 0–2% |\n\n```qml\nrotation: 360 * (KetTheme.animationClock % spinMs) / spinMs\n```\n\nEvery indicator holds the same clock:\n\n```mermaid\n" + Fixtures.mermaid.sharedClock + "\n```" },
        { type: "changes", files: [
            { name: "AgentSidebar.qml", path: "modules/ai/src/agent/qml", added: 3, removed: 1 },
            { name: "SpinClock.qml", path: "modules/ai/src/agent/qml", added: 48, removed: 0 }] },
        { type: "turn", status: "completed", text: "4m 12s · 182k context · $1.24 session" }
    ]

    readonly property var codex: [
        { type: "event", text: "Switched from Claude to Codex", detail: "GPT-5.6 Luna · High · the conversation history comes along" },
        { type: "user", text: "Add a journal size warning to the daemon status and the sidebar." },
        { type: "activity", summary: "Explored 4 files, listed 1 folder", steps: [
            { kind: "thinking", title: "Thought", detail: "14 s" },
            { kind: "read", title: "Read", detail: "apps/daemon/src/storage/LocalJournal.cpp" },
            { kind: "read", title: "Read", detail: "apps/daemon/src/storage/LocalJournal.h" },
            { kind: "search", title: "Searched code", detail: "“pruneRetention” in apps/daemon" },
            { kind: "list", title: "Listed files", detail: "modules/ai/src/agent/qml" }
        ] },
        { type: "commentary", text: "Retention never reclaims free pages. I'll measure the journal before changing it." },
        { type: "activity", summary: "Ran 2 commands", steps: [
            { kind: "command", title: "Ran", detail: "sqlite3 -readonly journal.sqlite3 \"pragma freelist_count\"", duration: "0.2 s",
              cwd: "~/Library/Application Support/Ketsuite/KetPlus/daemon", command: "sqlite3 -readonly journal.sqlite3 \"pragma page_count; pragma freelist_count\"",
              output: "2205637\n1261337" },
            { kind: "command", title: "Build the daemon", detail: "cmake --build build --target ketplus_daemon", status: "running", duration: "38 s",
              command: "cmake --build build --target ketplus_daemon", output: "[412/980] Building CXX object apps/daemon/CMakeFiles/…/LocalJournal.cpp.o\n[413/980] Building CXX object apps/daemon/CMakeFiles/…/SessionHost.cpp.o" }
        ] },
        { type: "approval", tool: "Command", state: "pending", reason: "Codex wants to run a command outside the workspace sandbox (network access).",
          command: "git push origin feat/journal-storage" },
        { type: "approval", tool: "ketplus_browser.browser_open", state: "approved", command: "http://localhost:5173/settings/storage" },
        { type: "activity", summary: "Edited 3 files", steps: [
            { kind: "edit", title: "Edited", detail: "apps/daemon/src/storage/LocalJournal.cpp  +64 −2", expanded: true,
              diff: "@@ -794,6 +794,14 @@ bool LocalJournal::pruneRetention() {\n     QSqlQuery query(database_);\n+    // Return free pages while idle; a full VACUUM runs once to switch modes.\n+    if (freePageShare() > 0.25 && !query.exec(QStringLiteral(\"PRAGMA incremental_vacuum(2048)\")))\n+        return false;\n     const QStringList statements{\n" },
            { kind: "create", title: "Created", detail: "apps/daemon/src/storage/JournalBudget.h  +41" },
            { kind: "move", title: "Moved", detail: "docs/journal.md → docs/daemon-journal.md" },
            { kind: "delete", title: "Deleted", detail: "apps/daemon/src/storage/OldRetention.cpp  −120" }
        ] },
        { type: "activity", summary: "Used ketplus_browser 3 times, used node_repl, used ketplus_agents", steps: [
            { kind: "browser", title: "Opened", detail: "localhost:5173/settings/storage" },
            { kind: "browser", title: "Observed the page", detail: "Storage · 4 meters" },
            { kind: "browser", title: "Captured", detail: "1440 × 900", images: [shot("settings")], expanded: true },
            { kind: "mcp", title: "Used node_repl", detail: "js · format the size table", duration: "0.4 s" },
            { kind: "mcp", title: "Used ketplus_agents", detail: "agents_list · 12 conversations", status: "failed",
              error: "agents_list: the daemon closed the connection" }
        ] },
        { type: "activity", summary: "Searched the web, viewed and generated images, waited", steps: [
            { kind: "web", title: "Searched the web", detail: "sqlite incremental_vacuum free pages",
              results: [{ title: "PRAGMA incremental_vacuum", url: "https://sqlite.org/pragma.html#pragma_incremental_vacuum" }] },
            { kind: "web", title: "Opened a page", detail: "sqlite.org/lang_vacuum.html" },
            { kind: "web", title: "Found in page", detail: "“VACUUM INTO” on sqlite.org/lang_vacuum.html" },
            { kind: "image", title: "Viewed an image", detail: "docs/assets/storage-mock.png", images: [shot("settings")] },
            { kind: "generate", title: "Generated an image", detail: "1024 × 1024 · transparent", expanded: true,
              prompt: "A flat storage gauge icon in the Két style, 2 px strokes, no frame.", images: [shot("editor")] },
            { kind: "wait", title: "Waited", detail: "30 s" }
        ] },
        { type: "question", header: "Limit", state: "expired", question: "What should the default journal size limit be?" },
        { type: "question", header: "Scope", state: "deferred", question: "Should remote devices show the warning too?",
          answer: "You chose to answer later" },
        { type: "reconnect", text: "Reconnecting… 2/5" },
        { type: "error", title: "Selected model is at capacity", message: "Codex could not finish the turn. Try again, or pick another model for this conversation.",
          action: "Switch model" },
        { type: "event", text: "Model changed to GPT-5.6 Luna · Medium" },
        { type: "event", text: "Moved to worktree feat/journal-storage", detail: "The agent now works in ketplus-journal" },
        { type: "answer", status: "interrupted", markdown: "The daemon now reports `journal.size` in `session.list` and warns past **80%** of the limit. Still to do:" },
        { type: "turn", status: "interrupted", text: "1m 08s · stopped by you" },
        { type: "queued", text: "Then add the Storage settings page." }
    ]

    readonly property var cursor: [
        { type: "event", text: "Forked from @agent/5208 at turn 14", detail: "Cursor · Composer 2" },
        { type: "user", text: "Find where the staging tenant gets created and summarise the flow." },
        { type: "activity", summary: "Ran 3 commands, used a subagent", steps: [
            { kind: "command", title: "Ran", detail: "rg -n \"createTenant\" services", command: "rg -n \"createTenant\" services",
              output: "services/tenant/src/create.ts:18\nservices/gateway/src/routes/tenant.ts:42" },
            { kind: "command", title: "Ran", detail: "sed -n 1,80p services/tenant/src/create.ts" },
            { kind: "subagent", title: "Ran a subagent", detail: "Task · Trace the staging tenant flow · 9 steps" },
            { kind: "browser", title: "Used ketplus_browser", detail: "browser_observe · staging admin" }
        ] },
        { type: "approval", tool: "Cursor tool", state: "approved", command: "rg -n \"tenant\" services/gateway" },
        { type: "answer", markdown: "A staging tenant is created in **two steps**:\n\n1. `POST /tenants` in the gateway validates the slug.\n2. `createTenant()` provisions the schema and seeds the admin user." },
        { type: "error", title: "Cursor CLI exited", message: "The agent process ended with code 1 before the turn completed.", action: "Retry" },
        { type: "turn", status: "failed", text: "2m 31s" }
    ]
}
