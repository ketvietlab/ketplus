/* Synthetic remote pairing. Pairing codes never enter K.demo, query strings or logs. */
if (K.page === "connections") {
    let cancelled = false;
    const navigate = (remote, extra = {}) => K.go("connections", {
        state: K.state, remote, dialog: null, auto: null, outcome: null, host: null, ...extra,
    });
    const hostId = button => button?.dataset.host || K.remoteHost()?.id;
    const selectHost = button => K.demo.remoteHosts.find(host => host.id === hostId(button));
    Object.assign(handlers, {
        "remote-add": () => navigate("add"),
        "remote-cancel": () => {
            cancelled = true;
            navigate(K.demo.remoteHosts.length ? "connected" : "empty");
        },
        "remote-mismatch": () => navigate("mismatch"),
        "remote-confirm": () => {
            if (!document.querySelector("#remote-code-matched")?.checked) return;
            navigate("connecting", { auto: "1", outcome: K.query.get("outcome") });
        },
        "remote-details": button => navigate("details", { host: hostId(button) }),
        "remote-back": () => navigate("connected"),
        "remote-disconnect": button => {
            const host = selectHost(button);
            if (!host) return;
            host.status = "offline";
            host.lastSeen = "Just now";
            navigate("offline", { host: host.id });
        },
        "remote-reconnect": button => {
            const host = selectHost(button);
            if (host) navigate("reconnecting", {
                host: host.id, auto: "1", outcome: K.query.get("outcome"),
            });
        },
        "remote-forget": button => {
            const host = selectHost(button);
            if (host) navigate("details", {
                host: host.id, dialog: "forget", outcome: K.query.get("outcome"),
            });
        },
        "remote-confirm-forget": () => {
            const host = K.remoteHost();
            if (host) navigate("removing", {
                host: host.id, auto: "1", outcome: K.query.get("outcome"),
            });
        },
    });
    document.addEventListener("change", event => {
        if (event.target.id !== "remote-code-matched") return;
        const confirm = document.querySelector('[data-action="remote-confirm"]');
        if (confirm) confirm.disabled = !event.target.checked;
    });
    document.addEventListener("input", event => {
        if (event.target.id !== "remote-code") return;
        event.target.removeAttribute("aria-invalid");
        document.querySelector("#remote-code-error").textContent = "";
    });
    document.addEventListener("submit", event => {
        if (event.target.id !== "remote-code-form") return;
        event.preventDefault();
        const input = event.target.elements.code;
        const code = input.value.trim().toUpperCase();
        if (!/^[A-Z0-9]{4}-[A-Z0-9]{4}$/.test(code)) {
            input.setAttribute("aria-invalid", "true");
            document.querySelector("#remote-code-error").textContent =
                "Enter the eight-character code shown by the Linux machine.";
            input.focus();
            return;
        }
        // Only the synthetic outcome crosses a route. The submitted code stays in this page.
        const outcome = code === "KETP-4826" ? null :
            code === "KETP-9999" ? "unavailable" : "expired";
        navigate("checking", { auto: "1", outcome });
    });
    if (K.query.get("auto") === "1") {
        const transition = {
            checking: () => navigate(K.query.get("outcome") === "expired" ? "code-expired" :
                K.query.get("outcome") === "unavailable" ? "unavailable" : "verify"),
            connecting: () => {
                if (K.query.get("outcome") === "error") return navigate("connect-error");
                if (!K.demo.remoteHosts.some(host => host.id === remoteSample.id))
                    K.demo.remoteHosts.push({ ...remoteSample });
                navigate("connected");
            },
            reconnecting: () => {
                const host = K.remoteHost();
                if (!host) return navigate("empty");
                if (K.query.get("outcome") === "error")
                    return navigate("reconnect-error", { host: host.id });
                host.status = "connected";
                host.lastSeen = "Just now";
                navigate("connected", { host: host.id });
            },
            removing: () => {
                const host = K.remoteHost();
                if (!host) return navigate("empty");
                if (K.query.get("outcome") === "error")
                    return navigate("remove-error", { host: host.id });
                K.demo.remoteHosts = K.demo.remoteHosts.filter(item => item.id !== host.id);
                navigate("removed");
            },
        }[K.remoteState()];
        if (transition) setTimeout(() => { if (!cancelled) transition(); }, 850);
    }
}
