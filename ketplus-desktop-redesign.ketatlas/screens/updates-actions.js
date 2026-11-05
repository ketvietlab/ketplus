/* Simulated update lifecycle. Timers only advance after a user action, so atlas states stay deterministic. */
const updateQuery = (update, extra = {}) => ({ tab: "updates", update, saved: null, pending: null, ...extra });
Object.assign(handlers, {
    "check-updates": () => K.go("settings", updateQuery("checking", { pending: "1" })),
    "retry-updates": () => K.go("settings", updateQuery("checking", { pending: "1" })),
    "install-update": () => K.go("settings", updateQuery("downloading", { pending: "1" })),
    "cancel-update": () => K.go("settings", updateQuery(null, { saved: "update-cancelled" })),
    "later-update": () =>
        K.go("workspace", {
            tab: null,
            update: K.update.state() === "ready" ? "ready" : null,
            saved: null,
            pending: null,
        }),
    "skip-update": () => K.go("settings", updateQuery(null, { saved: "skipped" })),
    "restart-update": () => K.go("workspace", { tab: null, update: null, pending: null, saved: "updated" }),
});
document.addEventListener("change", (event) => {
    if (event.target.id === "update-auto-check")
        K.go("settings", { tab: "updates", autoCheck: event.target.checked ? null : "off" });
    if (event.target.id === "update-auto-download")
        K.go("settings", { tab: "updates", autoDownload: event.target.checked ? "on" : null });
});
if (K.page === "settings" && K.query.get("tab") === "updates" && K.query.get("pending") === "1") {
    const next = { checking: K.query.get("result") === "error" ? "error" : "available", downloading: "ready" }[K.update.state()];
    if (next) setTimeout(() => K.go("settings", updateQuery(next, { result: null })), 1400);
}
if (K.query.get("saved") === "updated") K.toast(`Updated to KetPlus ${K.update.available}. Running agents were not interrupted.`);
