K.atlasFlows = {
    "getting-started": {
        title: "Getting started",
        description: "From opening a project to a workspace ready for your Agent.",
        firstTitle: "Open a project",
        firstStep: "workspace",
        action: "Open workspace",
    },
    "workspace-recovery": {
        title: "Reconnect a workspace",
        description: "Recover a missing project and return to your workspace.",
        firstTitle: "Project unavailable",
        firstStep: "missing",
        action: "Choose project",
    },
};
Object.setPrototypeOf(K.atlasFlows, null);
K.activeAtlasFlow = () =>
    K.atlasFlows[K.query.get("flow")] ? K.query.get("flow") : "getting-started";
K.hasAtlasMap = () =>
    K.query.get("bundleState") !== "missing" &&
    Boolean(K.current.bundleReady || K.query.get("hasAtlas") === "1");
K.atlasScope = () =>
    K.query.get("scope") === "entire-atlas" || !K.hasAtlasMap()
        ? null
        : K.atlasFlows[K.query.get("scope")]
          ? K.query.get("scope")
          : K.activeAtlasFlow();
K.flowScopeContent = () => {
    const id = K.atlasScope(),
        flow = K.atlasFlows[id];
    return `<div class="flow-scope-line"><span class="flow-scope-caption">EDIT SCOPE</span>${flow ? `<span class="flow-tag" data-flow-tag="${id}"><span>@ ${flow.title}</span><button type="button" data-action="remove-flow-tag" aria-label="Remove flow tag" title="Remove flow tag">${K.icon("x")}</button></span>` : '<span class="atlas-wide-scope">Entire Atlas</span>'}<button type="button" class="flow-tag-picker" data-action="pick-flow" aria-expanded="false" aria-controls="flow-options" ${!K.hasAtlasMap() ? "disabled" : ""}>${K.icon("plus")}<span>${flow ? "Change" : "Tag flow"}</span></button></div><div id="flow-options" class="flow-options" hidden><div class="flow-picker-heading"><b>Tag a flow</b><span>Focus the request on one flow. Shared assets may affect others.</span></div>${Object.entries(
        K.atlasFlows,
    )
        .map(
            ([key, value]) =>
                `<button type="button" class="flow-option" data-action="tag-flow" data-flow="${key}">${K.icon("git-branch")}<span><b>${value.title}</b><small>${value.description}</small></span>${id === key ? K.icon("check") : ""}</button>`,
        )
        .join(
            "",
        )}</div><span class="flow-scope-help">${!K.hasAtlasMap() ? "Flow tags become available when your Atlas is ready." : flow ? "Preferred context, not an edit boundary. Review shared-screen changes." : "This request applies to the entire Atlas."}</span>`;
};
K.flowComposer = () => `<div id="flow-scope" class="flow-scope">${K.flowScopeContent()}</div>`;
K.refreshFlowScope = () => {
    const el = document.querySelector("#flow-scope");
    if (el) el.innerHTML = K.flowScopeContent();
};
K.requestFlowTag = () => {
    const flow = K.atlasFlows[K.query.get("requestFlow")];
    return `<span class="request-flow-tag">${K.icon(flow ? "git-branch" : "network")}${flow ? `@ ${flow.title}` : "Entire Atlas"}</span>`;
};
document.addEventListener("click", (event) => {
    const button = event.target.closest("[data-action]");
    if (!button || button.disabled) return;
    if (button.dataset.action === "pick-flow") {
        const menu = document.querySelector("#flow-options");
        menu.hidden = !menu.hidden;
        button.setAttribute("aria-expanded", String(!menu.hidden));
        if (!menu.hidden) menu.querySelector("button").focus();
    }
    if (button.dataset.action === "tag-flow") {
        const id = button.dataset.flow || K.activeAtlasFlow();
        if (!K.atlasFlows[id]) return;
        if (document.querySelector("#atlas-ai-chat")?.hidden)
            document.querySelector(".atlas-chat-toggle").click();
        K.query.set("scope", id);
        K.refreshFlowScope();
        document.querySelector("#prompt").focus();
        K.toast(`Tagged flow: ${K.atlasFlows[id].title}`);
    }
    if (button.dataset.action === "remove-flow-tag") {
        K.query.set("scope", "entire-atlas");
        K.refreshFlowScope();
        document.querySelector("#prompt").focus();
    }
});
K.atlasFlowSidebar = () => `<aside id="atlas-flow-sidebar" class="atlas-flow-sidebar" aria-label="Atlas flows"><div class="atlas-flow-sidebar-heading"><span>Flows</span><small>${Object.keys(K.atlasFlows).length}</small></div><nav aria-label="Choose an Atlas flow">${Object.entries(K.atlasFlows).map(([id, flow]) => `<button type="button" class="atlas-flow-item" data-action="select-atlas-flow" data-flow="${id}" ${id === K.activeAtlasFlow() ? 'aria-current="true"' : ""}>${K.icon("git-branch")}<span><b>${K.escape(flow.title)}</b><small>2 screens</small></span></button>`).join("")}</nav></aside>`;
document.addEventListener("click", (event) => {
    const button = event.target.closest('[data-action="select-atlas-flow"]');
    if (!button || !K.atlasFlows[button.dataset.flow]) return;
    const scope = K.atlasScope();
    K.query.set("scope", scope || "entire-atlas");
    K.query.set("flow", button.dataset.flow);
    document.querySelector(".builder-map").outerHTML = K.atlasMap(true, false, false);
    K.refreshFlowScope();
    document.querySelector('.atlas-flow-item[aria-current="true"]')?.focus();
});
document.addEventListener("keydown", (event) => {
    const menu = document.querySelector("#flow-options");
    if (event.key === "Escape" && menu && !menu.hidden) {
        menu.hidden = true;
        const button = document.querySelector("[data-action=pick-flow]");
        button.setAttribute("aria-expanded", "false");
        button.focus();
    }
});

document.addEventListener("DOMContentLoaded", () => {
    if (K.page === "atlas-builder" && K.query.get("dialog") === "flow")
        document.querySelector("[data-action=pick-flow]")?.click();
});
