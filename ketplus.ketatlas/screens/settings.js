K.fontSizes = (key, fallback) =>
    [...new Set([fallback, 12, 13, 14, 15, 16, 18])]
        .sort((a, b) => a - b)
        .map(
            (n) =>
                `<option value="${n}" ${n === Number(K.query.get(key) || fallback) ? "selected" : ""}>${n === fallback ? "Default" : `${n} px`}</option>`,
        )
        .join("");
K.themeCard = (id, title) =>
    `<a href="${K.url("settings", { theme: id, system: "false" })}" class="theme-card ${window.KetPlusTheme.productTheme === id ? "selected" : ""}" aria-label="${title} theme"><div class="theme-mini" data-theme="${id}" data-kv-design-system><div class="theme-mini-side"><i></i><i></i><i></i></div><div class="theme-mini-main"><b></b><i></i><div><span></span><span></span></div><i></i></div></div><div>${title}${window.KetPlusTheme.productTheme === id ? K.icon("circle-check") : K.icon("circle")}</div></a>`;
K.views.settings = () => {
    const typo = K.query.get("tab") === "typography";
    return `<section class="view"><div class="page-content settings-page"><div class="page-heading"><div><div class="eyebrow">MAKE IT YOUR OWN</div><h1>Settings</h1><p>One consistent look across your entire workspace.</p></div><kbd>⌘ ,</kbd></div><div class="settings-layout"><nav class="settings-nav" aria-label="Settings categories">${K.link("settings", "Appearance", "palette", {}, !typo ? "active" : "")}${K.link("settings", "Typography", "type", { tab: "typography" }, typo ? "active" : "")}${K.link("connections", "Account & devices", "monitor")}${K.link("settings", "Providers & browser", "sliders-horizontal", { tab: "providers" })}</nav><div class="settings-main"><div class="section-heading"><h2>${typo ? "Typography" : "Appearance"}</h2></div><p class="settings-intro">${typo ? "Adjust readability across your workspace." : "Choose a theme that suits your lighting and helps you focus."}</p>${!typo ? `<div class="theme-options">${K.themeCard("light", "Light")}${K.themeCard("dark", "Dark")}</div><label class="system-theme-option"><input type="checkbox" id="follow-system" ${K.query.get("system") === "true" ? "checked" : ""}><span>Follow system appearance</span></label><div class="settings-divider"></div><h3>Accent color</h3><div class="accent-setting"><span class="accent-swatch"></span><div><b>Indigo</b><small>The signature color of Két Design System</small></div>${K.icon("check")}</div><div class="settings-divider"></div><h3>Workspace preview</h3><div class="settings-preview"><div class="preview-tab-strip"><span>${K.icon("file-code")} NavigationRail.qml</span><span>README.md</span></div><div class="settings-preview-content"><div class="settings-preview-code"><p><span class="syntax-comment">// Focus on what you are creating.</span></p><p><span class="syntax-keyword">property string</span> workspace: <span class="syntax-string">"develop"</span></p><p><span class="syntax-keyword">color</span>: theme.accentSubtle</p></div><div class="settings-preview-message">${K.mark()}<span>Ready for your next idea.</span>${K.badge("Completed", "success")}</div></div></div><p class="help">Applies to the interface, editor, diffs, Markdown, and terminal.</p>` : `<div class="typography-fields">${K.field('<span id="ui-font-label">Interface font</span>', '<select id="ui-font" aria-labelledby="ui-font-label"><option>KÉT Sans</option></select>', "KetPlus uses KÉT Sans throughout the interface.")}${K.field('<span id="ui-size-label">Interface font size</span>', `<select id="ui-size" aria-labelledby="ui-size-label">${K.fontSizes("fontSize", 15)}</select>`)}${K.field('<span id="conversation-width-label">Conversation width</span>', `<select id="conversation-width" aria-labelledby="conversation-width-label">${[["default","Default · 760 px"],["wide","Wide · 864 px"],["extra-wide","Extra wide · 1200 px"],["full","Full width"]].map(([id,title]) => `<option value="${id}" ${(K.query.get("conversationWidth") || "default") === id ? "selected" : ""}>${title}</option>`).join("")}</select>`, "Applies to Agent Workspace. Atlas keeps its compact reading column.")}${K.field('<span id="editor-font-label">Editor font</span>', '<select id="editor-font" aria-labelledby="editor-font-label"><option>KÉT Sans Mono</option><option>Menlo</option><option>Monospace</option></select>')}${K.field('<span id="editor-size-label">Editor font size</span>', `<select id="editor-size" aria-labelledby="editor-size-label">${K.fontSizes("editorSize", 13)}</select>`)}${K.field('<span id="terminal-size-label">Terminal font size</span>', `<select id="terminal-size" aria-labelledby="terminal-size-label">${K.fontSizes("terminalSize", 13)}</select>`)}${K.field('<span id="markdown-size-label">Markdown font size</span>', `<select id="markdown-size" aria-labelledby="markdown-size-label">${K.fontSizes("markdownSize", 14)}</select>`)}</div><div class="typography-sample"><small>PREVIEW</small><p id="font-ui-preview">A space for what you are creating.</p><pre id="font-editor-preview"><span class="syntax-keyword">const</span> workspace = <span class="syntax-string">"develop"</span>;\nagent.continue(workspace);</pre><div id="font-terminal-preview" class="mono muted">➜ ketplus-desktop git:(develop)</div><p id="font-markdown-preview">Good ideas start with a clear workspace.</p></div>`}<div class="settings-footer">${K.button("Restore defaults", "reset-appearance", null, "ghost")}<span class="spacer"></span>${K.button("Save changes", "save-appearance", "check", "primary")}</div></div></div></div></section>`;
};
K.searchResults = function (query = "") {
    const items = [
        [
            "FILES",
            "NavigationRail.qml",
            "modules/ai/src/agent/qml",
            "file-code",
            "editor",
            {},
        ],
        [
            "FILES",
            "design-system.md",
            "docs · Markdown",
            "file-text",
            "editor",
            { preview: "markdown", file: "design-system.md" },
        ],
        [
            "ATLAS",
            "Onboarding experience",
            "onboarding.ketatlas · develop",
            "network",
            "workspace",
            { panel: "atlas" },
        ],
    ];
    // Spotlight finds files and Atlases only; repositories are switched from the sidebar.
    const q = query.toLowerCase();
    const matched = items.filter((item) => `${item[1]} ${item[2]}`.toLowerCase().includes(q));
    if (!matched.length)
        return `<div class="search-empty">${K.icon("search")}<h3>No results found.</h3><p>Try another file or Atlas name.</p>${K.button("Clear query", "clear-search", null, "ghost")}</div>`;
    let group = "";
    return matched
        .map(([type, title, path, icon, page, params], i) => {
            const heading =
                group !== type
                    ? `<div class="search-group-label">${type}</div>`
                    : "";
            group = type;
            return (
                heading +
                `<a class="spotlight-result ${i === 0 ? "highlighted" : ""}" href="${K.url(page, params)}">${K.icon(icon)}<span><b>${title}</b><small>${path}</small></span>${i === 0 ? "<kbd>↵</kbd>" : K.icon("arrow-up-right")}</a>`
            );
        })
        .join("");
};
K.views.search = () =>
    `<section class="view agent-view"><div class="search-background">${K.mark("large")}<h1>Start with an idea.</h1><p class="muted">Your workspace.</p></div></section><div class="modal-backdrop spotlight-backdrop"><section class="spotlight modal" role="dialog" aria-modal="true" aria-label="Quick search"><form id="search-form"><label for="spotlight-input" class="sr-only">Search files and Atlases</label>${K.icon("search")}<input id="spotlight-input" autocomplete="off" placeholder="Search files and Atlases…" value="${K.escape(K.query.get("q") || "")}"><button type="button" data-action="close-search" class="btn ghost"><kbd>esc</kbd></button></form><div class="spotlight-hint"><span>FILES &amp; ATLASES</span></div><div id="spotlight-results">${K.searchResults(K.query.get("q") || "")}</div><footer><span><kbd>↑ ↓</kbd> navigate</span><span><kbd>↵</kbd> open</span><span class="spacer"></span><span>Search this workspace</span></footer></section></div>`;
