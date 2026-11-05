/* Synthetic Linux-host connection review inside Connections. No code, key, path or credential is persisted. */
const remoteSample = K.remoteSampleHost;
K.demo.remoteHosts ||= [];
K.remoteHost = () => K.demo.remoteHosts?.find(host => host.id === K.query.get("host")) ||
    K.demo.remoteHosts?.[0];
K.remoteState = () => K.demo.auth !== "signed-in" ? "auth-required" :
    K.query.get("remote") || (K.demo.remoteHosts?.length ? "connected" : "empty");
K.remoteHostRow = host => {
    const online = host.status === "connected";
    return `<article class="remote-host-row"><div class="remote-host-mark">${K.icon("monitor")}</div><div class="remote-host-copy"><div class="remote-host-name"><strong>${K.escape(host.name)}</strong>${K.badge(online ? "Connected" : "Offline", online ? "success" : "warning")}${K.badge(K.remoteRouteLabel(host))}</div><p>${K.escape(host.os)} · ${host.route === "ssh" ? `${K.escape(host.target)} · ` : ""}${online ? "Ready for remote work" : `Last seen ${K.escape(host.lastSeen)}`}</p></div><div class="remote-host-actions">${K.button("Details", "remote-details", null, "ghost", `data-host="${K.escape(host.id)}"`)}${K.button(online ? "Disconnect" : "Reconnect", online ? "remote-disconnect" : "remote-reconnect", online ? "unplug" : "refresh-cw", online ? "ghost" : "primary", `data-host="${K.escape(host.id)}"`)}</div></article>`;
};
K.remoteSetup = state => {
    const error = state === "invalid" ? "Enter the eight-character code shown by the Linux machine."
        : state === "code-expired" ? "That code expired. Request a new one on the Linux machine."
        : state === "unavailable" ? "The machine did not respond. Check its connection and request a new code."
        : "";
    return K.surface(`<div class="remote-section-head"><div><h2>Pair through KetPlus Relay</h2><p>Start pairing in KetPlus Daemon on the headless machine, then enter its one-time code here.</p></div>${K.button("Back", "remote-method", "arrow-left", "ghost")}</div><ol class="remote-steps"><li><span>1</span><div><strong>Prepare the machine</strong><p>Sign in with the same KetSuite account and start pairing in the Linux daemon.</p></div></li><li><span>2</span><div><strong>Enter the code</strong><p>The daemon prints a short-lived code. No SSH password or inbound port is needed.</p></div></li><li><span>3</span><div><strong>Compare and approve</strong><p>Confirm the matching identity code on both machines before granting access.</p></div></li></ol><form id="remote-code-form" class="remote-code-form" novalidate><label for="remote-code">One-time pairing code</label><div class="remote-code-controls"><input id="remote-code" name="code" type="text" inputmode="text" maxlength="9" autocomplete="off" autocapitalize="characters" spellcheck="false" placeholder="ABCD-EFGH" aria-describedby="remote-code-help remote-code-error" ${error ? 'aria-invalid="true"' : ""}><button class="btn primary" type="submit">Check code</button></div><p id="remote-code-help" class="remote-help">Eight letters or digits, shown as XXXX-XXXX. Demo code: KETP-4826.</p><p id="remote-code-error" class="remote-error" role="alert">${error}</p></form>`);
};
K.remoteVerify = mismatch => mismatch
    ? K.surface(`<div class="remote-section-head"><div><h2>Identity did not match</h2><p>The Linux machine was not trusted.</p></div></div>${K.notice("Codes do not match", "Do not connect. Request a new pairing code on the Linux machine and compare again.", "", "", "error")}${K.button("Start again", "remote-relay", "refresh-cw", "primary")}`)
    : K.surface(`<div class="remote-section-head"><div><h2>Confirm this machine</h2><p>Approve only if the code shown on Linux matches this one.</p></div>${K.button("Cancel", "remote-cancel", null, "ghost")}</div><div class="remote-identity"><span class="remote-host-mark">${K.icon("monitor")}</span><div><strong>${remoteSample.name}</strong><p>${remoteSample.os} · KetPlus Daemon</p></div></div><div class="remote-compare"><span>Identity confirmation code</span><strong aria-label="Confirmation code 4 8 2 6 1 9">482 619</strong><p>Compare all six digits with the code printed by the daemon.</p></div><label class="remote-check"><input type="checkbox" id="remote-code-matched"><span>I checked the code on both machines.</span></label><div class="remote-action-row">${K.button("Codes do not match", "remote-mismatch", null, "ghost")}${K.button("Trust and connect", "remote-confirm", "check", "primary", "disabled")}</div>`);
K.remotePending = (title, description, cancellable = true) => K.surface(`<div class="remote-pending" role="status"><span class="remote-pending-icon">${K.icon("refresh-cw")}</span><h2>${title}</h2><p>${description}</p>${cancellable ? K.button("Cancel", "remote-cancel", null, "ghost") : ""}</div>`);
K.remoteDetails = host => K.surface(`<div class="remote-section-head"><div><h2>${K.escape(host.name)}</h2><p>Trusted Linux machine</p></div>${K.button("Back to machines", "remote-back", "arrow-left", "ghost")}</div><dl class="remote-facts"><div><dt>Operating system</dt><dd>${K.escape(host.os)}</dd></div><div><dt>Connection</dt><dd>${host.status === "connected" ? "Connected" : "Offline"}</dd></div><div><dt>Last seen</dt><dd>${K.escape(host.lastSeen)}</dd></div><div><dt>Route</dt><dd>${host.route === "ssh" ? `SSH · ${K.escape(host.target)}` : "KetPlus Relay · authenticated outbound connection"}</dd></div></dl><div class="remote-detail-actions">${K.button(host.status === "connected" ? "Disconnect" : "Reconnect", host.status === "connected" ? "remote-disconnect" : "remote-reconnect", host.status === "connected" ? "unplug" : "refresh-cw", "ghost", `data-host="${K.escape(host.id)}"`)}${K.button("Forget machine…", "remote-forget", null, "ghost danger", `data-host="${K.escape(host.id)}"`)}</div>`);
K.remoteSection = () => {
    const state = K.remoteState();
    const hosts = K.demo.remoteHosts || [];
    const host = K.remoteHost();
    const notice = state === "removed" ? K.notice("Machine forgotten", "This desktop no longer trusts that Linux machine.", "", "", "success")
        : state === "connect-error" ? K.notice("Connection was not confirmed", "The machine was not added. Check the code and try again.", "remote-relay", "Try again", "error")
        : state === "reconnect-error" ? K.notice("Still offline", "The trusted machine did not respond. Your local workspace remains available.", "", "", "error")
        : state === "remove-error" ? K.notice("Could not forget this machine", "Its trust record is still present. Retry while connected.", "remote-forget", "Retry", "error")
        : state === "denied" ? K.notice("Pairing is not allowed", "Your workspace does not grant access to pair another machine. Ask an administrator.", "", "", "error")
        : "";
    let body = "";
    if (state === "auth-required")
        body = K.surface(`<div class="remote-empty">${K.icon("shield")}<h2>Sign in to connect a Linux machine</h2><p>Use your KetSuite account on both machines. Local conversations remain available while signed out.</p>${K.link("connections", "Sign in", "arrow-right", { state: "signed-out", dialog: "signin", remote: "auth-required" }, "btn primary")}</div>`);
    else if (K.remoteSshStates.includes(state)) body = K.remoteSshBody(state);
    else if (["add", "invalid", "code-expired", "unavailable"].includes(state)) body = K.remoteSetup(state);
    else if (state === "checking") body = K.remotePending("Checking the pairing request…", "Waiting for the Linux machine to respond.");
    else if (["verify", "mismatch"].includes(state)) body = K.remoteVerify(state === "mismatch");
    else if (state === "connecting") body = K.remotePending("Connecting to the machine…", "Trust is added only after the confirmation is acknowledged.");
    else if (state === "removing") body = K.remotePending("Forgetting the machine…", "The trust record stays visible until removal is acknowledged.", false);
    else if (state === "details" && host) body = K.remoteDetails(host);
    else if (hosts.length) body = K.surface(`<div class="remote-host-list">${hosts.map(K.remoteHostRow).join("")}</div>`);
    else body = K.surface(`<div class="remote-empty">${K.icon("monitor")}<h2>No Linux machines yet</h2><p>Connect a headless machine over SSH or the KetPlus Relay, then work in its repositories from this desktop.</p>${["denied", "connect-error"].includes(state) ? "" : K.button("Connect Linux machine", "remote-method", "plus", "ghost")}</div>`);
    const existing = hosts.length && [...K.remoteSshStates, "add", "invalid", "code-expired", "unavailable", "checking", "verify", "mismatch", "connecting"].includes(state)
        ? K.surface(`<div class="remote-host-list">${hosts.map(K.remoteHostRow).join("")}</div>`) : "";
    return `<section class="connections-remote" aria-labelledby="connections-linux-title"><div class="section-heading"><h3 id="connections-linux-title">Linux machines</h3><span>${hosts.length} trusted</span></div>${notice}${existing}${body}<div class="remote-foot">${K.icon("shield")}<p>Linux machines connect over your SSH access (free) or through the KetPlus Relay (Pro). Verify the host fingerprint or pairing code before trusting one.</p></div></section>`;
};
K.remoteDialog = () => {
    const host = K.remoteHost();
    return K.query.get("dialog") === "forget" && host && K.demo.auth === "signed-in"
        ? K.modal("Forget this machine?", `<p><strong>${K.escape(host.name)}</strong> will be removed from this desktop's trusted machines. Files and running work on Linux remain on that machine.</p><p class="help">Pair it again to reconnect later.</p>`, `${K.button("Keep machine", "close-dialog", null, "ghost")}${K.button("Forget machine", "remote-confirm-forget", null, "danger")}`, "remote-forget-dialog") : "";
};
