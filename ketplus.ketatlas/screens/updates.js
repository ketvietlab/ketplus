/* Desktop updates are simulated. No feed, download, signature or relaunch happens here. */
K.update = {
    installed: "0.1.17",
    available: "0.1.18",
    size: "24.6 MB",
    state() {
        const state = K.query.get("update") || "current";
        return ["current", "checking", "available", "downloading", "ready", "error", "disabled"].includes(state)
            ? state
            : "current";
    },
};
K.updateStatus = () => {
    const { installed, available, size } = K.update;
    const state = K.update.state();
    const checkDisabled = ["checking", "downloading", "disabled"].includes(state) ? "disabled" : "";
    const check = K.button(
        state === "checking" ? "Checking…" : "Check for updates",
        "check-updates",
        state === "checking" ? "loader-circle" : "refresh-cw",
        "",
        checkDisabled,
    );
    if (state === "disabled")
        return `${K.notice("Updates are not configured for this build", "This build has no signed update feed. Install new versions manually; KetPlus will not contact an update server.", "", "", "warning")}<div class="update-actions">${check}</div>`;
    if (state === "error")
        return `${K.notice("Could not check for updates", "The update feed did not respond. KetPlus " + installed + " keeps working and nothing was downloaded.", "retry-updates", "Retry", "error")}`;
    if (state === "available")
        return `<section class="update-card"><header>${K.badge("New version", "brand")}<h3>KetPlus ${available} is available</h3><span class="muted">You have ${installed} · ${size}</span></header><ul class="update-notes"><li>Start conversations from the phone in a new worktree.</li><li>Agent turns survive a desktop update.</li><li>Faster transcript loading for long conversations.</li></ul><div class="update-actions">${K.button("Install and relaunch", "install-update", "download", "primary")}${K.button("Remind me later", "later-update", null, "ghost")}<span class="spacer"></span>${K.button("Skip this version", "skip-update", null, "ghost")}</div></section>`;
    if (state === "downloading")
        return `<section class="update-card"><header><h3>Downloading KetPlus ${available}</h3><span class="muted">15.2 of ${size}</span></header><progress value="62" max="100" aria-label="Update download progress">62%</progress><p class="help">The signature is verified before anything is installed.</p><div class="update-actions">${K.button("Cancel", "cancel-update", null, "ghost")}</div></section>`;
    if (state === "ready")
        return `<section class="update-card ready"><header>${K.badge("Ready", "success")}<h3>KetPlus ${available} is ready to install</h3><span class="muted">Downloaded and verified</span></header>${K.query.get("agents") === "idle" ? '<p class="help">No agent turns are running. KetPlus restarts in a few seconds.</p>' : K.notice("2 agent turns are running", "They keep running in the background service. Only the window restarts; the new service takes over when those turns finish.", "", "", "")}<div class="update-actions">${K.button("Restart to update", "restart-update", "refresh-cw", "primary")}${K.button("Later", "later-update", null, "ghost")}</div></section>`;
    return `<section class="update-card"><header>${K.badge(state === "checking" ? "Checking" : "Up to date", state === "checking" ? "" : "success")}<h3>${state === "checking" ? "Looking for a newer version…" : "KetPlus is up to date"}</h3><span class="muted">${state === "checking" ? "Contacting the signed update feed" : "Last checked today at 09:41"}</span></header><div class="update-actions">${check}</div></section>`;
};
K.updatePreferences = () => {
    const disabled = K.update.state() === "disabled" ? "disabled" : "";
    const autoCheck = K.query.get("autoCheck") !== "off";
    const autoDownload = K.query.get("autoDownload") === "on";
    return `<div class="settings-divider"></div><h3>Preferences</h3><label class="choice"><input type="checkbox" id="update-auto-check" ${autoCheck ? "checked" : ""} ${disabled}><span><b>Check for updates automatically</b><small>Once a day while KetPlus is open.</small></span></label><label class="choice"><input type="checkbox" id="update-auto-download" ${autoDownload ? "checked" : ""} ${disabled || (!autoCheck ? "disabled" : "")}><span><b>Download updates in the background</b><small>You still choose when KetPlus restarts.</small></span></label><div class="settings-divider"></div><h3>Running agents</h3><p class="help">Agents run in the KetPlus background service, not in this window. Restarting to update never stops a turn: the current service finishes its turns, then the new version takes over.</p>`;
};
K.updateReceipt = () => {
    const saved = K.query.get("saved");
    if (saved === "skipped")
        return K.notice("Version skipped", `KetPlus will not offer ${K.update.available} again. Newer versions are still offered.`, "", "", "");
    if (saved === "update-cancelled")
        return K.notice("Download cancelled", "Nothing was installed. You can check again at any time.", "", "", "");
    return "";
};
K.settingsNav = (active) =>
    `<nav class="settings-nav" aria-label="Settings categories">${K.link("settings", "Appearance", "palette")}${K.link("settings", "Typography", "type", { tab: "typography" })}${K.link("connections", "Account & devices", "monitor")}${K.link("settings", "Providers & browser", "sliders-horizontal", { tab: "providers" })}${K.link("settings", "Archived", "archive", { tab: "archived" })}${K.link("settings", "Updates", "download", { tab: "updates" }, active === "updates" ? "active" : "")}</nav>`;
const settingsWithProviders = K.views.settings;
K.views.settings = () => {
    if (K.query.get("tab") !== "updates")
        return settingsWithProviders().replace(
            '</nav><div class="settings-main">',
            `${K.link("settings", "Updates", "download", { tab: "updates" })}</nav><div class="settings-main">`,
        );
    return `<section class="view"><div class="page-content settings-page"><div class="page-heading"><div><h1>Settings</h1></div><kbd>⌘ ,</kbd></div><div class="settings-layout">${K.settingsNav("updates")}<div class="settings-main"><div class="section-heading"><h2>Updates</h2></div><p class="settings-intro">KetPlus Desktop ${K.update.installed} · Stable channel</p>${K.updateReceipt()}${K.updateStatus()}${K.updatePreferences()}</div></div></div></section>`;
};
const sidebarWithoutUpdates = K.sidebar;
K.sidebar = () => {
    const pill =
        K.query.get("update") === "ready" && !(K.page === "settings" && K.query.get("tab") === "updates")
            ? K.link("settings", `<span>Update available</span><small>KetPlus ${K.update.available} · Review and install</small>`, "download", { tab: "updates", userMenu: null }, "update-pill")
            : "";
    return sidebarWithoutUpdates()
        .replace(
            `${K.link("connections", "<span>Account & devices</span>", "connect", { userMenu: null }, "user-menu-item")}</div>`,
            `${K.link("connections", "<span>Account & devices</span>", "connect", { userMenu: null }, "user-menu-item")}${K.link("settings", "<span>Check for updates…</span>", "refresh-cw", { tab: "updates", userMenu: null }, "user-menu-item")}</div>`,
        )
        .replace('<button type="button" class="identity"', `${pill}<button type="button" class="identity"`);
};
