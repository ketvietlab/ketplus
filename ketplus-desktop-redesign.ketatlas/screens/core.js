/* Prototype data only. No network, account, filesystem or provider calls. */
const K = (window.K = {
    page: document.body.dataset.screen,
    query: new URLSearchParams(location.search),
    views: {},
    state: "",
    icon(name, cls = "") {
        return `<svg class="icon ${cls}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${window.KET_ICONS[name] || window.KET_ICONS["circle"]}</svg>`;
    },
    escape(value = "") {
        return String(value).replace(
            /[&<>"']/g,
            (s) =>
                ({
                    "&": "&amp;",
                    "<": "&lt;",
                    ">": "&gt;",
                    '"': "&quot;",
                    "'": "&#39;",
                })[s],
        );
    },
    url(page, query = {}) {
        const params = new URLSearchParams();
        for (const key of [
            "theme",
            "system",
            "provider",
            "mode",
            "conversationWidth",
            "model",
            "effort",
            "compactWindow",
            "permission",
            "tree",
            "fontSize",
            "editorSize",
            "terminalSize",
            "markdownSize",
            "editorFont",
            "skills",
            "chat",
            "flow",
            "scope",
            "requestFlow",
            "brief",
            "hasAtlas",
            "project",
            "bundle",
            "bundleState",
            "repo",
            "machine",
            "repositories",
            "userMenu",
            "returnTo",
            "fromConversation",
            "bg",
            "bgCollapsed",
            "task",
            "update",
            "autoCheck",
            "autoDownload",
        ])
            if (K.query.has(key)) params.set(key, K.query.get(key));
        Object.entries(query).forEach(([key, value]) => {
            if (value !== null && value !== "") params.set(key, value);
            else params.delete(key);
        });
        K.conversationParams?.(page, query, params);
        // State belongs in the fragment: never send a growing transcript snapshot
        // as an HTTP request target or Referer header to the static viewer.
        params.delete("demo");
        const fragment = K.demo ? "#" + new URLSearchParams({ demo: JSON.stringify(K.demo) }) : "";
        return `${page}.html${params.size ? "?" + params : ""}${fragment}`;
    },
    go(page, query = {}) {
        K.captureDraft?.();
        location.href = K.url(page, query);
    },
    link(page, label, icon, query = {}, cls = "") {
        return `<a class="${cls}" href="${K.url(page, query)}"${label ? "" : ` aria-label="${icon === "x" ? "Close" : icon === "arrow-left" ? "Go back" : "Open screen"}"`}>${icon ? K.icon(icon) : ""}${label}</a>`;
    },
    button(label, action, icon, cls = "", extra = "") {
        return `<button type="button" class="btn ${cls}" data-action="${action}" ${extra}>${icon ? K.icon(icon) : ""}${label}</button>`;
    },
    ib(icon, label, action, extra = "") {
        return K.button(
            "",
            action,
            icon,
            "icon-btn",
            `aria-label="${label}" title="${label}" ${extra}`,
        );
    },
    badge(text, variant = "") {
        return `<span class="badge ${variant}">${text}</span>`;
    },
    template(name, slots) {
        return window.ATLAS_DESIGN_SYSTEM.templates[name].replace(
            /__ATLAS_SLOT_([A-Z_]+)__/g,
            (_, key) => slots[key.toLowerCase()] || "",
        );
    },
    surface(body) {
        return K.template("surface", { body });
    },
    field(label, control, help = "") {
        return `<div class="field-row"><label>${label}</label><div>${control}${help ? `<p class="help">${help}</p>` : ""}</div></div>`;
    },
    modal(title, body, footer, cls = "") {
        return `<div class="modal-backdrop"><section class="modal ${cls}" role="dialog" aria-modal="true" aria-labelledby="dialog-title"><header><h2 id="dialog-title">${title}</h2>${K.ib("x", "Close", "close-dialog")}</header><div class="modal-body">${body}</div>${footer ? `<footer>${footer}</footer>` : ""}</section></div>`;
    },
    toast(message) {
        document.querySelector(".toast")?.remove();
        const div = document.createElement("div");
        div.className = "toast";
        div.setAttribute("role", "status");
        div.innerHTML = `${K.icon("circle-check")}<span>${K.escape(message)}</span>`;
        document.body.append(div);
        setTimeout(() => div.remove(), 3600);
    },
    mark(size = "") {
        return `<span class="ket-mark ${size}" aria-hidden="true"><img src="../assets/brand/ketplus-v1/ketplus-mark-transparent.png" alt="" width="1254" height="1254" /></span>`;
    },
});
const fragmentState = new URLSearchParams(location.hash.slice(1)).get("demo");
if (fragmentState) K.query.set("demo", fragmentState);
K.state = K.query.get("state") || "ready";
for (const [queryKey, cssKey, defaultSize] of [
    ["fontSize", "ui", 14.95],
    ["editorSize", "editor", 12.65],
    ["terminalSize", "terminal", 12.65],
    ["markdownSize", "markdown", 13.8],
]) {
    if (K.query.has(queryKey)) {
        const size = Math.min(18, Math.max(10, parseFloat(K.query.get(queryKey)) || defaultSize));
        document.body.style.setProperty(`--kp-${cssKey}-size`, `${size}px`);
        if (cssKey === "ui") document.body.style.setProperty("--kp-font-scale", size / 13);
    }
}
if (["Menlo", "Monospace"].includes(K.query.get("editorFont")))
    document.body.style.setProperty("--kp-editor-font", K.query.get("editorFont"));

const conversationWidths = {default: "760px", wide: "864px", "extra-wide": "1200px", full: "100%"};
if (K.page === "workspace") document.body.style.setProperty("--kp-conversation-width", conversationWidths[K.query.get("conversationWidth")] || "760px");
