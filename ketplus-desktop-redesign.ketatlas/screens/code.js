K.sourceLines = [
    ["comment", "// NavigationRail.qml"],
    ["", "import QtQuick"],
    ["", "import QtQuick.Controls"],
    ["", ""],
    ["", "Item {"],
    ["", "    id: navigationRail"],
    ["", ""],
    ["", '    property string activeWorktree: "develop"'],
    ["", "    property var conversations: []"],
    ["", ""],
    ["comment", "    // Keep workspace context visible."],
    ["", "    width: theme.sidebarWidth"],
    ["", ""],
    ["", "    Column {"],
    ["", "        anchors.fill: parent"],
    ["", "        spacing: theme.spacingSmall"],
    ["", ""],
    ["", "        RepositoryPicker {"],
    ["", "            repository: workspace.repository"],
    ["", "            onActivated: workspace.openPicker()"],
    ["", "        }"],
    ["", ""],
    ["", "        WorktreeList {"],
    ["", "            model: workspace.worktrees"],
    ["", "            currentPath: activeWorktree"],
    ["", "            onSelected: path => {"],
    ["", "                agent.selectWorktree(path)"],
    ["", "            }"],
    ["", "        }"],
    ["", ""],
    ["", "        ConversationList {"],
    ["", "            model: conversations"],
    ["", "            activeId: agent.conversationId"],
    ["", "        }"],
    ["", "    }"],
    ["", "}"],
];
K.highlight = function (line) {
    if (line.trim().startsWith("//"))
        return `<span class="syntax-comment">${K.escape(line)}</span>`;
    const tokens =
        /("(?:\\.|[^"\\])*")|\b(import|property|string|var|id|width|spacing|model|currentPath|activeId)\b/g;
    let output = "",
        end = 0;
    for (const token of line.matchAll(tokens)) {
        output += K.escape(line.slice(end, token.index));
        output += `<span class="syntax-${token[1] ? "string" : "keyword"}">${K.escape(token[0])}</span>`;
        end = token.index + token[0].length;
    }
    return output + K.escape(line.slice(end));
};
K.diff = function (compact = false) {
    const staged = K.query.get("group") === "staged";
    const lines = staged
        ? [
              ["context", "  // Shared navigation colors"],
              ["remove", "  activeColor = palette.highlight();"],
              ["add", "  activeColor = tokens.accentSubtle();"],
              ["add", "  activeText = tokens.accent();"],
              ["context", "  borderColor = tokens.panelBorder();"],
          ]
        : [
              ["context", "    WorktreeItem {"],
              ["context", "        id: worktreeRow"],
              ["context", "        width: parent.width"],
              ["remove", '        color: "transparent"'],
              ["remove", "        radius: 2"],
              ["add", "        color: selected ? theme.accentSubtle"],
              ["add", '                        : "transparent"'],
              ["add", "        radius: theme.radiusSmall"],
              ["context", ""],
              ["context", "        Column {"],
              ["context", "            spacing: 4"],
              ["context", "            Text {"],
              ["context", "                text: worktree.branch"],
              ["remove", "                color: theme.textSecondary"],
              ["add", "                color: selected ? theme.accent"],
              ["add", "                                : theme.textPrimary"],
              ["context", "            }"],
              ["add", "            Text {"],
              ["add", "                visible: worktree.isRunning"],
              ["add", '                text: qsTr("Agent working")'],
              ["add", "                color: theme.success"],
              ["add", "                font.pixelSize: theme.textSmall"],
              ["add", "            }"],
              ["context", "        }"],
              ["context", "    }"],
          ];
    return `<div class="diff-content"><div class="diff-summary"><span>${staged ? "Staged · index" : "Working tree · develop"}</span><span class="spacer"></span><span class="positive">${staged ? "+6" : "+28"}</span><span class="negative">${staged ? "−2" : "−9"}</span></div><div class="hunk-header">@@ ${staged ? "-42,3 +42,4" : "-18,16 +18,28"} @@ ${staged ? "ThemeManager" : "WorktreeItem"}</div><div class="diff-lines">${lines.map(([type, line], i) => `<div class="diff-line ${type}"><span class="line-number">${type === "add" ? "" : 18 + i}</span><span class="line-number">${type === "remove" ? "" : 18 + i}</span><span class="diff-sign">${type === "add" ? "+" : type === "remove" ? "−" : " "}</span><code>${K.highlight(line)}</code></div>`).join("")}</div><div class="diff-end">${K.icon("check")} End of changes${compact ? "" : `<span class="spacer"></span>${K.link("editor", "Open full file", "arrow-up-right", {}, "text-link")}`}</div></div>`;
};
K.terminal = () =>
    `<div class="terminal-surface"><div class="terminal-session"><span class="positive">●</span> zsh <span class="muted">· ketplus-desktop</span><span class="spacer"></span>${K.badge("Local")}<button class="btn ghost" onclick="this.closest('.terminal-surface').querySelector('.terminal-search').hidden=false">Find</button><button class="btn ghost" onclick="this.closest('.terminal-surface').querySelector('.terminal-output').textContent=''">Clear</button><button class="btn ghost" onclick="this.closest('.terminal-surface').querySelector('.terminal-output').textContent='New zsh session ready.'">Restart</button></div><input class="terminal-search" hidden placeholder="Find in terminal" aria-label="Find in terminal"><div class="terminal-output" id="terminal-output"><p class="terminal-prompt">➜ <b>ketplus-desktop</b> <span>git:(develop)</span> git status --short</p><p> <span class="negative">M</span> modules/ai/src/agent/qml/NavigationRail.qml</p><p> <span class="negative">M</span> modules/ai/src/agent/qml/WorktreeItem.qml</p><p> <span class="positive">M</span> apps/desktop/src/ui/ThemeManager.cpp</p><br><p class="terminal-prompt">➜ <b>ketplus-desktop</b> <span>git:(develop)</span> git diff --check</p><p class="positive">✓ No whitespace errors.</p><br><p class="muted">Demo terminal · try: help, pwd, git status, clear</p></div><form id="terminal-form"><label for="terminal-command" class="terminal-prompt">➜ <b>ketplus-desktop</b> <span>git:(develop)</span></label><input id="terminal-command" name="command" autocomplete="off" spellcheck="false" aria-label="Demo terminal command"></form></div>`;
K.explorer = () =>
    `<aside class="file-explorer"><div class="explorer-title"><strong>EXPLORER</strong>${K.ib("refresh-cw", "Refresh files", "refresh")}</div><div class="folder-row root-folder">${K.icon("chevron-down")}${K.icon("folder-open")} KETPLUS-DESKTOP</div>${[
        ["apps", "folder", "0"],
        ["desktop", "folder", "1"],
        ["src", "folder", "2"],
        ["modules", "folder", "0"],
        ["ai", "folder", "1"],
        ["src / agent / qml", "folder-open", "2"],
        ["NavigationRail.qml", "file-code", "3"],
        ["WorktreeItem.qml", "file-code", "3"],
        ["atlas", "folder", "1"],
        ["docs", "folder-open", "0"],
        ["architecture.md", "file-text", "1"],
        ["design-system.md", "file-text", "1"],
        ["README.md", "file-text", "0"],
        ["CMakeLists.txt", "file", "0"],
    ]
        .map(([name, icon, depth]) =>
            icon.startsWith("folder")
                ? `<details class="explorer-folder" open><summary style="padding-left:${12 + Number(depth) * 12}px">${K.icon("chevron-down")}${K.icon(icon)}<span>${name}</span></summary></details>`
                : K.link(
                      "editor",
                      `<span>${name}</span>${name === "NavigationRail.qml" || name === "WorktreeItem.qml" ? '<small class="warning-text">M</small>' : ""}`,
                      icon,
                      {
                          file: name,
                          preview: name.endsWith(".md") ? "markdown" : null,
                      },
                      `file-row ${name === (K.query.get("file") || "NavigationRail.qml") ? "active" : ""} indent-${depth}`,
                  ),
        )
        .join("")}</aside>`;
K.markdown = () =>
    `<article class="markdown-preview"><div class="eyebrow">KETPLUS · DOCUMENTATION</div><h1>A space to create.</h1><p class="markdown-lead">A focused space to explore code, work with your Agent, and turn ideas into products.</p><hr><h2>Intentional design</h2><p>KetPlus combines a lightweight editor with Agent Workspace and Atlas. Every surface shares the same colors, density, and interaction patterns.</p><div class="markdown-callout">${K.icon("layers")}<p>Context stays where you need it. Tools appear when needed, keeping your workspace focused.</p></div><h2>Three principles</h2><ol><li><b>Clarity:</b> always know which repository and worktree are selected.</li><li><b>Continuity:</b> open files and terminals beside your conversation.</li><li><b>Consistency:</b> one set of tokens across the interface.</li></ol><h2>Workspace structure</h2><pre>apps/desktop    → Windows & navigation\nmodules/ai      → Agent Workspace\nmodules/atlas   → Atlas & preview</pre><p class="muted">Sample documentation for this prototype.</p></article>`;
K.markdownSource = () => `<pre class="markdown-source"># A space to create.

A focused space to explore code, work with your Agent, and turn ideas into products.

## Intentional design

Context stays where you need it.</pre>`;
K.markdownModeBar = (diff = false) => {
    const mode = K.query.get("mode") || (diff ? "diff" : "preview");
    return `<nav class="markdown-mode-bar" aria-label="Markdown display mode">${(diff ? ["diff", "preview", "source"] : ["preview", "source"]).map(value => K.link(diff ? "changes" : "editor", value[0].toUpperCase() + value.slice(1), value === "preview" ? "eye" : "file-code", {file: "design-system.md", preview: "markdown", mode: value, group: K.query.get("group")}, `btn ${mode === value ? "primary" : "ghost"}`)).join("")}</nav>`;
};
K.markdownContent = (diff = false) => {
    const mode = K.query.get("mode") || (diff ? "diff" : "preview");
    return `<div class="markdown-content"><div class="markdown-body">${mode === "preview" ? K.markdown() : mode === "source" ? K.markdownSource() : '<pre class="markdown-source">  # A space to create.\n<span class="positive">+ Context stays where you need it.</span></pre>'}</div>${K.markdownModeBar(diff)}</div>`;
};
K.views.editor = () => {
    const markdown = K.query.get("preview") === "markdown";
    const name =
        K.query.get("file") ||
        (markdown ? "design-system.md" : "NavigationRail.qml");
    return `<section class="view editor-view">${K.explorer()}<div class="editor-workbench"><div class="file-tabs">${K.link("editor", "NavigationRail.qml", "file-code", {}, !markdown ? "active" : "")}${K.link("editor", "design-system.md", "file-text", { preview: "markdown", file: "design-system.md" }, markdown ? "active" : "")}<span class="spacer"></span>${K.ib("terminal", "Open terminal", "terminal")}</div><div class="file-breadcrumb"><span>${markdown ? "docs" : "modules / ai / src / agent / qml"}</span>${K.icon("chevron-right")} ${K.escape(name)}<span class="spacer"></span>${K.link("changes", "View diff", "git-compare-arrows", markdown ? {file: name, mode: "diff"} : {}, "text-link")}</div><div class="editor-document">${markdown ? K.markdownContent() : `<div class="source-code" role="region" aria-label="QML file content">${K.sourceLines.map(([type, line], i) => `<div class="source-line ${i === 7 ? "current-line" : ""}"><span>${i + 1}</span><code>${K.highlight(line)}</code></div>`).join("")}</div>`}</div>${K.query.get("panel") === "terminal" ? `<div class="editor-terminal"><div class="context-tabs"><span>${K.icon("terminal")} TERMINAL</span>${K.ib("x", "Close terminal", "close-panel")}</div>${K.terminal()}</div>` : ""}<div class="editor-infobar"><span>Ln 8, Col 34</span><span>Spaces: 4</span><span>${markdown ? "Markdown" : "QML"}</span><span>UTF-8</span></div></div></section>`;
};
K.views.changes = () => {
    const staged = K.query.get("group") === "staged";
    return `<section class="view editor-view"><aside class="file-explorer git-explorer"><div class="explorer-title"><strong>SOURCE CONTROL</strong>${K.ib("refresh-cw", "Refresh Git status", "refresh")}</div><div class="git-repository">${K.icon("folder-open")} ketplus-desktop <small>develop</small></div>${K.state === "clean" ? '<p class="help git-clean">No changes.</p>' : `<details open class="change-group"><summary>${K.icon("chevron-down")} Changes <span>2</span></summary>${["NavigationRail.qml", "WorktreeItem.qml", "design-system.md"].map((name) => K.link("changes", `<span>${name}<small>modules/ai/src/agent/qml</small></span><b class="negative">M</b>`, "file-code", { file: name }, `change-row ${!staged && (K.query.get("file") || "NavigationRail.qml") === name ? "selected" : ""}`)).join("")}</details><details open class="change-group"><summary>${K.icon("chevron-down")} Staged Changes <span>1</span></summary>${K.link("changes", '<span>ThemeManager.cpp<small>apps/desktop/src/ui</small></span><b class="positive">M</b>', "file-code", { group: "staged" }, `change-row ${staged ? "selected" : ""}`)}</details>`}<div class="git-footer"><p>Run Git commands in the terminal</p>${K.link("editor", "Open terminal", "terminal", { panel: "terminal" }, "btn")}</div></aside><div class="editor-workbench">${K.state === "clean" ? `<div class="empty-state">${K.icon("circle-check")}<h2>Your worktree is clean.</h2><p>There are no uncommitted changes in this snapshot.</p>${K.link("editor", "Back to editor", "arrow-left", {}, "btn")}</div>` : `<div class="file-tabs"><span class="active">${K.icon("git-compare-arrows")} ${staged ? "ThemeManager.cpp" : K.escape(K.query.get("file") || "NavigationRail.qml")} ${K.badge(staged ? "Staged" : "Working tree")}</span><span class="spacer"></span>${K.link("editor", "Open file", "arrow-up-right", {}, "btn ghost")}</div><div class="file-breadcrumb"><span>${staged ? "apps / desktop / src / ui" : "modules / ai / src / agent / qml"}</span><span class="spacer"></span><span>Inline diff</span></div>${(K.query.get("file") || "").endsWith(".md") ? K.markdownContent(true) : K.diff()}`}</div></section>`;
};
