/* Worktree-scoped Atlas fixtures; the real Desktop owns CLI processes and browser windows. */
K.atlasEntries = () => {
    if (K.query.get("atlasList") === "empty") return [];
    const standard = K.repo === "api-service" || (K.tree !== "develop" && K.tree !== "feature-onboarding") ? []
        : K.repo === "ketplus-desktop" && K.tree === "develop"
        ? [
              { id: "desktop", title: "KétPlus Desktop", detail: "18 flows · 7 screens", updated: "Updated just now" },
              { id: "onboarding", title: "Onboarding experience", detail: "4 flows · 8 screens", updated: "Updated yesterday" },
          ]
        : [
              { id: "onboarding", title: "Onboarding experience", detail: "4 flows · 8 screens", updated: "Updated yesterday" },
          ];
    const created = Object.entries(K.scope.atlasBundles || {}).map(([id, title]) => ({
        id,
        title,
        detail: "Agent generated · ready to open",
        updated: "Just created",
    }));
    return [...standard, ...created];
};
K.activeAtlasSessions = () => {
    const fixture = K.query.get("atlasView");
    const ids = ["running", "stopping"].includes(fixture) ? ["desktop"]
        : fixture === "multiple" ? ["desktop", "onboarding"]
        : ["browser-closed", "stopped", "starting", "error"].includes(fixture) ? []
        : K.scope.atlasSessions || [];
    const available = new Set(K.atlasEntries().map(item => item.id));
    return ids.filter(id => available.has(id));
};
K.otherAtlasSessions = () => Object.entries(K.demo.scopes).flatMap(([scopeKey, scope]) =>
    scope === K.scope ? [] : (scope.atlasSessions || []).map(id => ({
        id,
        scopeKey,
        scope,
        title: scope.atlasBundles?.[id] || (id === "desktop" ? "KétPlus Desktop" : id === "onboarding" ? "Onboarding experience" : id),
    })));
K.runningAtlasCount = () => K.activeAtlasSessions().length + K.otherAtlasSessions().length;
K.atlasHeaderButton = () => {
    const count = K.runningAtlasCount();
    return `<button type="button" class="atlas-header-button icon-btn ${K.query.get("panel") === "atlas" ? "active" : ""}" data-action="open-atlas-panel" aria-label="Atlases, ${count} running across worktrees" aria-pressed="${K.query.get("panel") === "atlas"}" title="Atlases across worktrees">${K.icon("network")}${count ? `<span class="atlas-header-count" aria-hidden="true">${count}</span>` : ""}</button>`;
};
K.atlasPanel = (remoteUnavailable = false) => {
    const entries = K.atlasEntries();
    const running = new Set(K.activeAtlasSessions());
    const state = K.query.get("atlasView") || K.scope.atlasEvent || "";
    const notice = state === "browser-closed"
        ? `<div class="atlas-panel-notice" role="status">${K.icon("circle-check")}<span>The last browser tab closed. Atlas stopped automatically.</span></div>`
        : state === "stopping"
          ? `<div class="atlas-panel-notice" role="status">${K.icon("loader-circle")}<span>Stopping Atlas…</span></div>`
        : state === "stopped" || state === "owner-stop"
          ? `<div class="atlas-panel-notice" role="status">${K.icon("square")}<span>Atlas stopped from KétPlus.</span></div>`
          : state === "error"
            ? `<div class="atlas-panel-notice error" role="alert">${K.icon("triangle-alert")}<span>Atlas could not start. Check the CLI and try again.</span>${K.button("Retry", "atlas-retry", "refresh-cw", "ghost")}</div>`
            : remoteUnavailable
              ? `<div class="atlas-panel-notice warning" role="status">${K.icon("unplug")}<span>This Linux machine is offline. Reconnect it to open an Atlas.</span></div>`
              : "";
    const rows = entries.map(item => {
        const active = running.has(item.id);
        const starting = state === "starting" && item.id === (K.scope.atlasStarting || "desktop");
        const stopping = state === "stopping" && item.id === (K.scope.atlasStopping || "desktop");
        const status = stopping ? K.badge("Stopping") : active ? K.badge("Running", "success") : starting ? K.badge("Starting") : "";
        return `<article class="atlas-conversation-item" data-atlas-row data-atlas-title="${K.escape(item.title.toLowerCase())}"><div class="atlas-item-icon">${K.icon("network")}</div><div class="atlas-item-main"><div class="atlas-item-heading"><h3 title="${K.escape(item.title)}">${K.escape(item.title)}</h3>${status}</div><p>${K.escape(item.detail)} <span aria-hidden="true">·</span> ${K.escape(item.updated)}</p><div class="atlas-item-actions">${K.button("Edit", "atlas-edit", "square-pen", "ghost", `data-atlas-id="${K.escape(item.id)}" ${remoteUnavailable ? "disabled" : ""}`)}${K.button(stopping ? "Stopping…" : active ? "Open browser" : starting ? "Starting…" : "Open in browser", "atlas-open", active ? "external-link" : "arrow-up-right", active ? "ghost" : "", `data-atlas-id="${K.escape(item.id)}" ${starting || stopping || remoteUnavailable ? "disabled" : ""}`)}${active ? K.button(stopping ? "Stopping…" : "Stop", "atlas-stop", "square", "ghost", `data-atlas-id="${K.escape(item.id)}" ${stopping ? "disabled" : ""}`) : ""}</div></div></article>`;
    }).join("");
    const elsewhere = K.otherAtlasSessions();
    const otherRows = elsewhere.map(item => {
        const stopping = item.scope.atlasStopping === item.id;
        return `<article class="atlas-other-item"><div><strong title="${K.escape(item.title)}">${K.escape(item.title)}</strong><span>${K.escape(item.scope.repo)} · ${K.escape(item.scope.tree)}</span></div>${K.button(stopping ? "Stopping…" : "Stop", "atlas-stop", "square", "ghost", `data-atlas-id="${K.escape(item.id)}" data-atlas-scope="${K.escape(item.scopeKey)}" ${stopping ? "disabled" : ""}`)}</article>`;
    }).join("");
    return `<div class="atlas-conversation-panel"><div class="atlas-panel-lead"><div><h2>Atlases in this worktree</h2><p>${K.escape(K.repo)} · ${K.escape(K.tree)}</p></div>${K.button("New Atlas", "atlas-new", "plus", "ghost", remoteUnavailable ? "disabled" : "")}</div>${notice}${entries.length ? `<label class="atlas-panel-search">${K.icon("search")}<span class="sr-only">Search Atlases</span><input id="atlas-conversation-search" type="search" placeholder="Search Atlases" autocomplete="off"></label><div class="atlas-conversation-list">${rows}</div><p class="atlas-filter-empty" hidden>No Atlases match this search.</p>` : `<div class="atlas-panel-empty">${K.icon("network")}<h3>No Atlases in this worktree</h3><p>Create an Atlas with your Agent to see it here.</p>${K.button("Create with Agent", "atlas-new", "plus", "primary", remoteUnavailable ? "disabled" : "")}</div>`}${elsewhere.length ? `<section class="atlas-other-worktrees" aria-label="Running in other worktrees"><h3>Running in other worktrees</h3>${otherRows}</section>` : ""}<p class="atlas-panel-footnote">Atlas stops when its last browser tab closes. You can also stop it here.</p></div>`;
};
