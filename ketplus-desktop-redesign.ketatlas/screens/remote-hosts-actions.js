/* Synthetic remote pairing. Pairing codes never enter K.demo, query strings or logs. */
if (K.page === "connections") {
    let cancelled = false;
    const navigate = (remote, extra = {}) => K.go("connections", {
        state: K.state, remote, dialog: null, auto: null, outcome: null, host: null, ...extra,
    });
    const hostId = button => button?.dataset.host || K.remoteHost()?.id;
    const selectHost = button => K.demo.remoteHosts.find(host => host.id === hostId(button));
    Object.assign(handlers, {
        // Add → Linux machine first chooses SSH or the relay.
        "remote-add": () => navigate("method"),
        "remote-method": () => navigate("method"),
        "remote-relay": () => navigate("add"),
        "remote-upgrade": () => navigate("method", { plan: null }),
        "remote-ssh": () => navigate("ssh"),
        "remote-ssh-retry": () => navigate("ssh-checking", { auto: "1", outcome: null }),
        "remote-hostkey-confirm": () => {
            if (!document.querySelector("#remote-hostkey-matched")?.checked) return;
            navigate("ssh-connecting", { auto: "1", outcome: K.query.get("outcome") });
        },
        "remote-copy-command": button => { button.textContent = "Copied"; },
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
        const gate = { "remote-code-matched": "remote-confirm", "remote-hostkey-matched": "remote-hostkey-confirm" }[event.target.id];
        if (!gate) return;
        const confirm = document.querySelector(`[data-action="${gate}"]`);
        if (confirm) confirm.disabled = !event.target.checked;
    });
    document.addEventListener("input", event => {
        const error = { "remote-code": "#remote-code-error", "remote-ssh-target": "#remote-ssh-error" }[event.target.id];
        if (!error) return;
        event.target.removeAttribute("aria-invalid");
        document.querySelector(error).textContent = "";
    });
    document.addEventListener("submit", event => {
        if (event.target.id !== "remote-ssh-form") return;
        event.preventDefault();
        const input = event.target.elements.target;
        const target = input.value.trim();
        if (!/^([A-Za-z0-9._-]+@)?[A-Za-z0-9.-]+$/.test(target)) {
            input.setAttribute("aria-invalid", "true");
            document.querySelector("#remote-ssh-error").textContent =
                "Enter user@host or a Host name from ~/.ssh/config.";
            input.focus();
            return;
        }
        // Only the synthetic outcome crosses a route; the typed host stays in this page.
        const outcome = { "old@dev-server.local": "outdated", "new@dev-server.local": "no-daemon",
            "bad@dev-server.local": "auth", "nobody@nowhere": "unreachable" }[target] || "hostkey";
        navigate("ssh-checking", { auto: "1", outcome });
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
            "ssh-checking": () => navigate({ hostkey: "ssh-hostkey", auth: "ssh-auth",
                unreachable: "ssh-unreachable", "no-daemon": "ssh-no-daemon", outdated: "ssh-outdated" }[K.query.get("outcome")] || "ssh-connecting"),
            "ssh-connecting": () => {
                if (!K.demo.remoteHosts.some(host => host.id === K.remoteSshSampleHost.id))
                    K.demo.remoteHosts.push({ ...K.remoteSshSampleHost });
                navigate("connected", { host: K.remoteSshSampleHost.id });
            },
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
