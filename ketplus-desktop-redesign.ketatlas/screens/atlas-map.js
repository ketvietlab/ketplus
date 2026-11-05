/* Product Atlas surface. The enclosing KetAtlas viewer remains CLI-owned. */
K.atlasMap = (ready, building, error) => {
    const collapsed = K.query.get("chat") === "collapsed";
    const flow = K.atlasFlows[K.activeAtlasFlow()];
    const node = (id, title, step, x) =>
        `<article class="atlas-map-node" style="left:${x}px;top:155px"><header><span>${id}</span><strong>${title}</strong>${K.icon("panels-top-left")}</header><a class="atlas-node-preview" aria-label="Preview ${title}" href="${K.url("atlas-preview", { step })}"><iframe title="${title} screen" src="${K.url("atlas-preview", { embed: "screen", step })}" loading="lazy" tabindex="-1" sandbox="allow-scripts allow-forms"></iframe></a><footer>${K.badge(id === "01" ? "Start" : "End", id === "01" ? "brand" : "success")}${K.link("atlas-preview", "Try screen", "arrow-up-right", { step }, "text-link")}</footer></article>`;
    const fromConversation = K.query.get("fromConversation");
    const back = fromConversation
        ? K.ib("arrow-left", "Back to conversation", "close-atlas-editor", 'data-editor-back')
        : K.link("atlas-library", "", "arrow-left", {}, "btn icon-btn");
    return `<div class="builder-map ${ready ? "has-flow-sidebar" : ""}"><header class="atlas-map-toolbar">${back}${ready ? K.button("", "toggle-atlas-flows", "git-branch", "icon-btn atlas-flow-mobile-toggle", 'aria-label="Show flows" title="Show flows" aria-controls="atlas-flow-sidebar" aria-expanded="false"') : ""}<div><strong>${ready ? K.escape(K.query.get("project") || "Onboarding experience") : "New Atlas"}</strong><small>${ready ? `${flow.title} · 2 screens` : "develop · ketplus-desktop"}</small></div><span class="spacer"></span>${ready ? K.badge(K.query.get("bundleState") === "invalid" ? "Last valid preview" : "Validated preview", "success") : K.badge(building ? "Generating" : error ? "Interrupted" : "Draft")}<button type="button" class="btn atlas-chat-toggle ${collapsed ? "" : "active"}" data-action="toggle-atlas-chat" aria-controls="atlas-ai-chat" aria-expanded="${!collapsed}">${K.icon("messages-square")}<span>${collapsed ? "AI Chat" : "Hide AI Chat"}</span></button></header>${ready ? K.atlasFlowSidebar() : ""}<div class="atlas-map-viewport ${ready ? "" : "is-empty"}" aria-label="Atlas workflow map"><div class="atlas-map-stage" style="--map-zoom:1">${ready ? `<div class="atlas-flow-label"><span class="flow-index">${String(Object.keys(K.atlasFlows).indexOf(K.activeAtlasFlow()) + 1).padStart(2, "0")}</span><div><h2>${flow.title}</h2><p>${flow.description}</p></div><button class="btn ghost tag-current-flow" type="button" data-action="tag-flow" data-flow="${K.activeAtlasFlow()}">${K.icon("plus")} Tag this flow</button></div><svg class="atlas-map-connectors" viewBox="0 0 1400 850" aria-hidden="true"><defs><marker id="flow-arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M1 1 L7 4 L1 7" fill="none" stroke="currentColor"/></marker></defs><path d="M340 338 H485" marker-end="url(#flow-arrow)"/></svg><div class="atlas-edge-label">${flow.action} ${K.icon("arrow-right")}</div>${node("01", flow.firstTitle, flow.firstStep, 80)}${node("02", "Workspace ready", "complete", 490)}` : `<div class="atlas-map-placeholder">${K.icon("network")}<div class="eyebrow">YOUR IDEAS, CONNECTED</div><h2>${building ? "Your Atlas is taking shape." : error ? "Pick up where you left off." : "Your Atlas will appear here."}</h2><p>${building ? "Your Agent is creating screens and the flows between them." : error ? "Continue the conversation to finish generating your screens and flows." : "Describe an experience in AI Chat. Your Agent will map the screens, states, and journeys in this workspace."}</p><div class="placeholder-flow"><span>${K.icon("panels-top-left")}</span><i></i><span>${K.icon("panels-top-left")}</span><i></i><span>${K.icon("circle-check")}</span></div></div>`}</div></div>${ready ? `<div class="atlas-map-tools" role="group" aria-label="Map zoom">${K.ib("zoom-out", "Zoom out", "map-zoom-out")}<output id="map-zoom">100%</output>${K.ib("zoom-in", "Zoom in", "map-zoom-in")}<span></span>${K.button("Fit map", "map-fit", "maximize", "ghost")}</div><span class="atlas-map-hint">Scroll to explore · Select a screen to try it</span>` : ""}</div>`;
};
document.addEventListener("click", (event) => {
    const action = event.target.closest("[data-action]")?.dataset.action;
    if (action === "toggle-atlas-flows") {
        const map = document.querySelector(".builder-map");
        const toggle = event.target.closest("[data-action]");
        const open = map.classList.toggle("flows-open");
        toggle.setAttribute("aria-expanded", String(open));
        toggle.setAttribute("aria-label", open ? "Hide flows" : "Show flows");
        return;
    }
    if (["toggle-atlas-chat", "hide-atlas-chat"].includes(action)) {
        const panel = document.querySelector("#atlas-ai-chat"),
            toggle = document.querySelector(".atlas-chat-toggle");
        panel.hidden = action === "hide-atlas-chat" || !panel.hidden;
        toggle.setAttribute("aria-expanded", String(!panel.hidden));
        toggle.classList.toggle("active", !panel.hidden);
        toggle.querySelector("span").textContent = panel.hidden ? "AI Chat" : "Hide AI Chat";
        K.query.set("chat", panel.hidden ? "collapsed" : "open");
        if (panel.hidden) toggle.focus();
        return;
    }
    if (!["map-zoom-in", "map-zoom-out", "map-fit"].includes(action)) return;
    const stage = document.querySelector(".atlas-map-stage"),
        viewport = document.querySelector(".atlas-map-viewport");
    let zoom = parseFloat(stage.style.getPropertyValue("--map-zoom")) || 1;
    zoom =
        action === "map-fit"
            ? Math.min(1, (viewport.clientWidth - 80) / 900, (viewport.clientHeight - 60) / 610)
            : Math.min(1.5, Math.max(0.25, zoom + (action === "map-zoom-in" ? 0.1 : -0.1)));
    stage.style.setProperty("--map-zoom", zoom);
    document.querySelector("#map-zoom").textContent = `${Math.round(zoom * 100)}%`;
    if (action === "map-fit") {
        viewport.scrollTop = 0;
        viewport.scrollLeft = 0;
    }
});
