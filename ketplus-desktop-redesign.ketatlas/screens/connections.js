/* Account/device snapshots are synthetic; credentials are never saved in demo state. */
if (K.page === "connections" && !K.query.has("demo")) {
    const remote = K.query.get("remote");
    K.demo.auth = ["signed-out", "signing-in", "signin-error", "auth-expired"].includes(K.state) || remote === "auth-required"
        ? "signed-out"
        : "signed-in";
    if (["paired", "revoking", "revoke-error"].includes(K.state))
        K.demo.devices = ["iphone", "pixel"];
    if (K.demo.auth === "signed-in" && (!remote || ["connected", "details", "offline", "reconnecting", "reconnect-error", "removing", "remove-error"].includes(remote)))
        K.demo.remoteHosts = [{ ...K.remoteSampleHost,
            status: ["offline", "reconnecting", "reconnect-error"].includes(remote) ? "offline" : "connected" }];
}
if (K.page === "connections" && ["signed-out", "auth-expired"].includes(K.state))
    K.demo.auth = "signed-out";
K.deviceName = (id) => (id === "iphone" ? "iPhone 16" : "Pixel 9");
K.deviceId = () => K.query.get("device") || "pixel";
K.pairDialog = () => {
    const stage = K.query.get("dialog");
    if (stage === "add-connection")
        return K.modal(
            "Add a connection",
            `<p class="help">Choose what you want to connect.</p><div class="connection-choices"><button type="button" class="connection-choice" data-action="pair-device">${K.icon("smartphone")}<span><strong>Phone or tablet</strong><small>Let KetPlus Mobile access this Mac.</small></span>${K.icon("chevron-right")}</button><button type="button" class="connection-choice" data-action="remote-add">${K.icon("monitor")}<span><strong>Linux machine</strong><small>Use a remote repository from this desktop.</small></span>${K.icon("chevron-right")}</button></div>`,
        );
    if (stage === "forget") return "";
    if (stage === "signin")
        return K.modal(
            "Sign in",
            `<p class="help">Use your KetSuite username and password.</p><form id="signin-form"><label for="account">Username</label><input id="account" name="account" autocomplete="off" placeholder="Demo username"><label for="password">Password</label><input id="password" name="password" type="password" autocomplete="off" placeholder="Demo password"><p id="signin-error" role="alert" class="error-text">${K.state === "signin-error" ? "Sign-in failed. Check your details and retry." : ""}</p><button class="btn primary" type="submit" ${K.state === "signing-in" ? "disabled" : ""}>${K.state === "signing-in" ? "Signing in…" : "Sign in"}</button></form><p class="demo-note">Prototype only: any nonempty username/password. Do not enter real credentials.</p>`,
        );
    if (stage === "revoke")
        return K.modal(
            "Revoke device access?",
            `<p><b>${K.deviceName(K.deviceId())}</b> will lose access to this Mac. Local Agent sessions keep running.</p><p class="help">The device is removed only after revocation is acknowledged.</p>${K.state === "revoke-error" ? K.notice("Revocation failed", "Access has not been confirmed revoked. Retry while connected.", "", "", "error") : ""}`,
            `${K.button("Keep device", "close-dialog")}${K.button(K.state === "revoking" ? "Revoking…" : "Revoke access", "revoke-device", "unplug", "danger", K.state === "revoking" ? "disabled" : "")}`,
        );
    if (stage === "signout")
        return K.modal(
            "Sign out of Connections?",
            `<p>Remote access from this Mac will be disconnected. Your local conversations and running Agents stay on this Mac.</p><p class="help">Signing out is not device revocation. Revoke an untrusted device explicitly.</p>`,
            `${K.button("Cancel", "close-dialog")}${K.button("Sign out", "confirm-signout", "log-out", "danger")}`,
        );
    if (stage === "details")
        return K.modal(
            K.deviceName(K.deviceId()),
            `<p>KetPlus App · Paired with this Mac</p><p class="help">You can revoke this device at any time.</p>`,
            `${K.button("Close", "close-dialog")}${K.button("Revoke access", "revoke-prompt", "unplug", "danger")}`,
        );
    if (stage === "confirm")
        return K.modal(
            "Confirm device",
            `<div class="pair-copy"><h2>Is this your phone?</h2><p>Compare the code on both devices.</p></div><div class="pair-code">482 619</div><div class="device-facts"><span>Device</span><b>Pixel 9</b><span>Account</span><b>Same signed-in account</b></div>${K.state === "confirm-error" ? K.notice("Confirmation failed", "No access has been granted. Retry with this ticket or decline.", "", "", "error") : ""}`,
            `${K.button("Decline", "close-dialog")}${K.button(K.state === "confirming" ? "Connecting…" : "Yes, connect this device", "confirm-device", "check", "primary", K.state === "confirming" ? "disabled" : "")}`,
        );
    if (stage === "expired")
        return K.modal(
            "Pairing code expired",
            `<p>The old code is no longer valid. No device was connected.</p>`,
            `${K.button("Close", "close-dialog")}${K.button("Generate new code", "new-pair-code", "refresh-cw", "primary")}`,
        );
    if (stage === "cancel-error")
        return K.modal(
            "Pairing cancellation pending",
            `<p>Access has not been approved. We could not confirm ticket cancellation while disconnected.</p><p class="help">The ticket expires automatically. Keep this code private until then.</p>`,
            K.button("Retry cancellation", "cancel-pair-retry", "refresh-cw"),
        );
    return K.modal(
        "Connect your phone",
        `<div class="pair-copy"><h2>Take your workspace with you.</h2><p>In KetPlus on your phone, choose <b>Scan QR code</b>.</p></div><div class="pair-qr"><img src="../assets/pairing-demo.svg" width="180" height="180" alt="Synthetic pairing code; no live service"></div><p class="pair-timer">Code expires in 04:59 · Demo snapshot</p><ol class="pair-steps"><li>Scan this code.</li><li>Compare the confirmation codes.</li><li>Approve this device on your Mac.</li></ol><p class="demo-note">Closing or declining cancels this pairing ticket.</p>`,
        `${K.button("Expire code", "expire-pair", null, "ghost")}${K.button("Simulate scan", "simulate-scan", "scan-qr", "primary")}`,
    );
};
K.views.connections = () => {
    const signedIn = K.demo.auth === "signed-in";
    const offline = ["offline", "reconnecting"].includes(K.state);
    const notice =
        K.state === "auth-expired"
            ? K.notice("Please sign in again", "Your login expired. Local work is preserved.")
            : K.state === "revoked"
              ? K.notice(
                    "Access revoked",
                    `${K.deviceName(K.deviceId())} can no longer access this Mac.`,
                    "",
                    "",
                    "",
                )
              : offline
                ? K.notice(
                      K.state === "reconnecting" ? "Reconnecting…" : "This Mac is offline",
                      "Your local sessions keep running. Remote updates resume when connected.",
                      K.state === "offline" ? "reconnect" : "",
                      "Reconnect",
                  )
                : "";
    const deviceRows = K.demo.devices
        .map(
            (id) =>
                `<div class="device-row"><span class="device-icon">${K.icon("smartphone")}</span><span class="device-description"><b>${K.deviceName(id)}</b><small>KetPlus App · Paired</small></span><span class="spacer"></span>${K.button("Details", "device-details", null, "ghost", `data-device="${id}"`)}${K.button("Revoke access", "revoke-prompt", "unplug", "ghost danger", `data-device="${id}"`)}</div>`,
        )
        .join("");
    const content = signedIn
        ? `<div class="connections-content"><div class="page-heading"><div><h1>Connections</h1><p>Phones that access this Mac and Linux machines this desktop can use.</p></div>${K.button("Add", "add-connection", "plus", "primary", offline ? "disabled" : "")}</div>${notice}<div class="connection-overview"><span class="large-device">${K.icon("monitor")}</span><div><h2>This Mac</h2><p>${offline ? "Working locally" : "Ready for your connected devices"}</p>${K.badge(offline ? "Offline" : "Connected", offline ? "warning" : "success")}</div></div><div class="section-heading"><h3>Phones & tablets</h3><span>${K.demo.devices.length} paired</span></div><div class="devices-list">${deviceRows || '<div class="empty-state"><p>No phones paired yet.</p></div>'}</div>${K.remoteSection?.() || ""}<div class="account-strip"><span class="avatar">AN</span><div><b>Demo account</b><small>Personal workspace</small></div><span class="spacer"></span>${K.button("Sign out", "signout", "log-out", "ghost")}</div><details class="connections-footnote"><summary>About your history</summary><p>Conversations started on this Mac keep their full history here. Recent content is synchronized through the cloud for up to 24 hours; paired devices may keep an account-scoped local cache.</p><p>Remote Linux repositories keep their files and running work on that machine.</p></details></div>`
         : `<div class="connections-intro">
            <header class="connections-intro-header"><h1>Connections</h1><span>${K.icon("unplug")} Not connected</span></header>
            ${notice}${K.query.get("remote") === "auth-required" ? K.notice("Sign in to connect a Linux machine", "The same KetSuite account is required on this desktop and the Linux daemon.") : ""}
            <div class="connections-hero">
                <div class="connections-hero-copy"><div class="eyebrow">KETPLUS CONNECTIONS</div><h2>Your workspace.<br>Within reach.</h2><p>Continue a conversation from your phone, or connect a headless Linux machine to work in its repositories from this desktop.</p>
                    <div class="connections-signin">${K.button("Sign in", "open-signin", "arrow-right", "primary")}<span>Use your KetSuite account to get started.</span></div>
                    <div class="connections-local-note">${K.icon("monitor")}<span>Your local workspace keeps working.<br>Sign in when you’re ready to connect a device.</span></div>
                </div>
                <div class="connections-scene" role="img" aria-label="Illustration of the same Agent conversation on a Mac and a phone">
                    <div class="connections-scene-content" aria-hidden="true">
                        <div class="connection-desktop"><div class="connection-desktop-bar"><span class="connection-window-dots"><i></i><i></i><i></i></span><span>${K.icon("monitor")} This Mac</span></div><div class="connection-desktop-body"><div class="connection-mini-rail">${K.mark()}${K.icon("messages-square")}${K.icon("folder")}${K.icon("network")}</div><div class="connection-mini-chat"><small>ketplus-desktop · develop</small><h3>Refine navigation</h3><div class="connection-sample-prompt">Make the workspace easier to navigate.</div><div class="connection-agent-label">${K.mark()}<b>Agent</b><span>Working</span></div><p>I’ve updated the navigation.<br>One detail needs your input.</p><div class="connection-sample-lines"><i></i><i></i></div></div></div></div>
                        <div class="connection-phone"><div class="connection-phone-camera"></div><header>${K.icon("chevron-left")}<b>Refine navigation</b></header><small>From your Mac</small><div class="connection-phone-question">${K.icon("circle-help")}<b>Your input is needed</b><p>Keep the sidebar expanded by default?</p><span>Yes, keep it open ${K.icon("arrow-up")}</span></div><div class="connection-phone-bottom"></div></div>
                        <div class="connection-scene-caption">${K.icon("messages-square")} Same conversation. Another screen.</div>
                    </div>
                </div>
            </div>
            <section class="connections-setup" aria-labelledby="connection-setup-title"><div class="connections-setup-heading"><h2 id="connection-setup-title">From this Mac to your phone</h2><p>Three steps. You approve the connection.</p></div><ol><li><span>01</span><div><h3>Sign in on this Mac</h3><p>Use the same KetSuite account on both devices.</p></div></li><li><span>02</span><div><h3>Scan with KetPlus Mobile</h3><p>Open the app on your phone and scan the pairing code.</p></div></li><li><span>03</span><div><h3>Confirm, then continue</h3><p>Match the codes and approve your phone on this Mac.</p></div></li></ol></section>
            <div class="connections-control-note">${K.icon("shield")}<p>You choose which devices can connect. Review or revoke their access here at any time.</p></div>
        </div>`;
    const permitted = K.query.get("dialog") === "signin" || signedIn;
    return `<section class="view"><div class="page-content connections-page">${content}</div></section>${K.query.has("dialog") && permitted ? K.pairDialog() : ""}${K.remoteDialog?.() || ""}`;
};
