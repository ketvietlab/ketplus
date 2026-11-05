/* KetAtlas 0.5 theme bridge. Viewer overrides never become product preferences. */
(() => {
    const root = document.documentElement;
    const query = new URLSearchParams(location.search);
    const system = matchMedia("(prefers-color-scheme: dark)");
    const preference = query.get("theme") === "dark" ? "dark" : "light";
    let followsSystem = query.get("system") === "true";
    let viewerTheme = null;
    const productTheme = () => followsSystem ? (system.matches ? "dark" : "light") : preference;
    const apply = () => {
        const theme = viewerTheme ?? productTheme();
        root.dataset.theme = theme;
        root.style.colorScheme = theme;
    };
    window.KetPlusTheme = {
        get preference() { return preference; },
        get productTheme() { return productTheme(); },
        setFollowSystem(enabled) {
            followsSystem = enabled;
            window.K?.query.set("system", String(enabled));
            apply();
        },
    };
    window.addEventListener("message", (event) => {
        if (window.parent === window || event.source !== window.parent) return;
        const message = event.data;
        if (message?.type !== "ketatlas:theme" || !["light", "dark", null].includes(message.theme)) return;
        viewerTheme = message.theme;
        apply();
    });
    system.addEventListener("change", apply);
    apply();
    if (window.parent !== window) window.parent.postMessage({ type: "ketatlas:theme-ready" }, "*");
})();
