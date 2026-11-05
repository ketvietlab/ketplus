/* Synthetic Linux connection over SSH, and the choice between SSH and the relay.
   No host, key, fingerprint or command output is persisted or sent anywhere. */
K.remoteSshSampleHost = Object.freeze({
    id: "dev-server",
    name: "dev-server",
    os: "Ubuntu 24.04 LTS",
    status: "connected",
    lastSeen: "Just now",
    route: "ssh",
    target: "ubuntu@dev-server.local",
});
K.remoteSshStates = ["method", "ssh", "ssh-checking", "ssh-hostkey", "ssh-unreachable", "ssh-auth",
    "ssh-no-daemon", "ssh-outdated", "ssh-connecting"];
// Pro is the default so the existing relay journey stays reachable; `plan=free` shows the gate.
K.remotePro = () => K.query.get("plan") !== "free";
K.remoteRouteLabel = host => host.route === "ssh" ? "SSH" : "Relay";

K.remoteMethod = () => {
    const pro = K.remotePro();
    const relayAction = pro
        ? K.button("Pair with a code", "remote-relay", "arrow-right", "ghost")
        : K.button("Upgrade to Pro", "remote-upgrade", "arrow-up-right", "ghost");
    return K.surface(`<div class="remote-section-head"><div><h2>Connect a Linux machine</h2><p>Choose how this Mac reaches the machine. Files and running work stay on Linux either way.</p></div>${K.button("Cancel", "remote-cancel", null, "ghost")}</div>
<div class="remote-methods">
  <article class="remote-method"><div class="remote-method-head"><span class="remote-host-mark">${K.icon("terminal")}</span><div><strong>SSH</strong>${K.badge("Free", "success")}</div></div>
    <p>Use the SSH access you already have. KetPlus runs your system <code>ssh</code> and reaches the daemon through it.</p>
    <ul><li>Works with <code>~/.ssh/config</code>, ssh-agent and jump hosts</li><li>Needs the machine to be reachable from this Mac</li><li>Desktop only</li></ul>
    ${K.button("Connect with SSH", "remote-ssh", "arrow-right", "primary")}</article>
  <article class="remote-method"><div class="remote-method-head"><span class="remote-host-mark">${K.icon("globe")}</span><div><strong>KetPlus Relay</strong>${K.badge("Pro", pro ? "" : "warning")}</div></div>
    <p>No SSH, VPN or open port. The daemon connects out to the relay with end-to-end encryption.</p>
    <ul><li>Works behind NAT and firewalls</li><li>Also from the KetPlus iPhone and Android apps</li><li>Notifications when a turn needs you</li></ul>
    ${pro ? "" : '<p class="remote-help">Your plan does not include the relay. SSH stays free.</p>'}${relayAction}</article>
</div>`);
};

K.remoteSshForm = state => {
    const error = state === "ssh-unreachable" ? "dev-server.local did not answer on port 22. Check the address, your network or VPN."
        : state === "ssh-auth" ? "The server refused the key (Permission denied, publickey). Add your key to ssh-agent or pick it in ~/.ssh/config."
        : "";
    return K.surface(`<div class="remote-section-head"><div><h2>Connect with SSH</h2><p>The same as <code>ssh user@host</code>. Passwords and keys stay in your SSH setup; KetPlus never stores them.</p></div>${K.button("Back", "remote-add", "arrow-left", "ghost")}</div>
<form id="remote-ssh-form" class="remote-code-form remote-ssh-form" novalidate>
  <label for="remote-ssh-target">Host</label>
  <div class="remote-code-controls"><input id="remote-ssh-target" name="target" type="text" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="user@host or a Host from ~/.ssh/config" value="${state === "ssh" ? "" : "ubuntu@dev-server.local"}" aria-describedby="remote-ssh-help remote-ssh-error" ${error ? 'aria-invalid="true"' : ""}><button class="btn primary" type="submit">Connect</button></div>
  <p id="remote-ssh-help" class="remote-help">Port, key and jump host come from <code>~/.ssh/config</code>. Demo: <code>ubuntu@dev-server.local</code>; <code>old@dev-server.local</code>, <code>new@dev-server.local</code>, <code>bad@dev-server.local</code> and <code>nobody@nowhere</code> show the other outcomes.</p>
  <p id="remote-ssh-error" class="remote-error" role="alert">${error}</p>
</form>`);
};

K.remoteSshHostKey = () => K.surface(`<div class="remote-section-head"><div><h2>First connection to dev-server.local</h2><p>SSH has not seen this server before. Compare its fingerprint with the one your administrator gave you.</p></div>${K.button("Cancel", "remote-cancel", null, "ghost")}</div>
<div class="remote-compare"><span>ED25519 host key fingerprint</span><strong class="remote-fingerprint">SHA256:x3Jq…demo…Lr0k</strong><p>On the server: <code>ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub</code></p></div>
<label class="remote-check"><input type="checkbox" id="remote-hostkey-matched"><span>The fingerprint matches. Add it to <code>~/.ssh/known_hosts</code>.</span></label>
<div class="remote-action-row">${K.button("Cancel", "remote-cancel", null, "ghost")}${K.button("Trust and continue", "remote-hostkey-confirm", "check", "primary", "disabled")}</div>`);

K.remoteSshCommand = (label, lines) => `<div class="remote-command"><div class="remote-command-head"><span>${label}</span>${K.button("Copy", "remote-copy-command", "copy", "ghost")}</div><pre><code>${lines.map(K.escape).join("\n")}</code></pre></div>`;

K.remoteSshDaemon = outdated => K.surface(`<div class="remote-section-head"><div><h2>${outdated ? "KetPlus Daemon is out of date" : "KetPlus Daemon is not installed"}</h2><p>${outdated
    ? "dev-server runs daemon 0.1.29; this desktop needs 0.1.47 or newer. Upgrade it, then connect again. Running turns on the server finish first."
    : "SSH works, but dev-server has no KetPlus Daemon for this user. Install it once; it runs as a systemd user service."}</p></div>${K.button("Back", "remote-ssh", "arrow-left", "ghost")}</div>
${K.remoteSshCommand(outdated ? "Upgrade on dev-server" : "Install on dev-server", outdated
    ? ["ketplusctl upgrade --wait-idle"]
    : ["curl -fsSL https://ketplus.example/install-daemon.sh | sh", "systemctl --user enable --now ketplus-daemon", "ketplusctl workspace add ~/src/api-service"])}
<p class="remote-help">KetPlus does not run these for you. Check the script before running it, or install the archive by hand.</p>
<div class="remote-action-row">${K.button("Connect again", "remote-ssh-retry", "refresh-cw", "primary")}</div>`);

K.remoteSshBody = state => state === "method" ? K.remoteMethod()
    : ["ssh", "ssh-unreachable", "ssh-auth"].includes(state) ? K.remoteSshForm(state)
    : state === "ssh-checking" ? K.remotePending("Connecting over SSH…", "Opening ssh, then looking for KetPlus Daemon on the server.")
    : state === "ssh-hostkey" ? K.remoteSshHostKey()
    : state === "ssh-no-daemon" ? K.remoteSshDaemon(false)
    : state === "ssh-outdated" ? K.remoteSshDaemon(true)
    : state === "ssh-connecting" ? K.remotePending("Connecting to KetPlus Daemon…", "Checking the protocol version and loading dev-server's repositories.")
    : "";

// A directly opened SSH node (`host=dev-server`) shows that machine without replaying the journey.
if (K.query.get("host") === K.remoteSshSampleHost.id && K.demo.auth === "signed-in" &&
    !K.demo.remoteHosts?.some(host => host.id === K.remoteSshSampleHost.id))
    (K.demo.remoteHosts ||= []).push({ ...K.remoteSshSampleHost });
