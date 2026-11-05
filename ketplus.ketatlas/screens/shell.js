K.nav = [
    ["workspace", "Agent Workspace", "messages-square"],
    ["editor", "Files & Editor", "files"],
    ["changes", "Source Control", "git-branch"],
    ["connections", "Connections", "unplug"],
];
K.sidebar = function () {
    const active = K.page.startsWith("atlas")
        ? "workspace"
        : ["search", "worktrees"].includes(K.page)
          ? "workspace"
          : K.page;
    const nav = K.nav
        .map(([page, label, icon]) =>
            K.link(
                page,
                `<span>${label}</span>${page === "changes" ? '<span class="nav-count">3</span>' : ""}`,
                icon === "files" ? "folder" : icon,
                {},
                `nav-item ${page === active ? "is-active" : ""}`,
            ),
        )
        .join("");
    const repositoryGroup = repo => {
        const records = K.repositoryRecords(repo.id);
        const host = K.repositoryHost(repo.id);
        const collapsed = (K.demo.collapsedRepositories || []).includes(repo.id);
        const title = records.length
            ? `<button class="repository-group-name" data-action="toggle-repository" data-repo="${K.escape(repo.id)}" aria-expanded="${!collapsed}" title="${K.escape(repo.label)}${host ? ` · ${K.escape(host.name)}` : ""}">${K.icon(repo.location === "remote" ? "monitor" : collapsed ? "folder" : "folder-open")}<span>${K.escape(repo.label)}</span></button>`
            : K.link("workspace", `${K.icon(repo.location === "remote" ? "monitor" : "folder")}<span>${K.escape(repo.label)}</span>`, null, { repo: repo.id, tree: "develop", state: "empty" }, "repository-group-name");
        return `<section class="repository-group ${repo.location === "remote" ? "remote-repository" : ""} ${repo.id === K.repo ? "selected" : ""}"><div class="repository-group-header">${title}${K.ib("plus", host?.status === "offline" ? `Reconnect ${host.name} to start a conversation` : `New conversation in ${repo.label}`, "new-chat", `data-repo="${K.escape(repo.id)}" ${host?.status === "offline" ? "disabled" : ""}`)}</div>${collapsed ? "" : K.conversationRows(repo.id)}</section>`;
    };
    const machine = K.selectedMachine();
    const host = K.demo.remoteHosts?.find(item => item.id === machine);
    const machineOptions = [`<option value="local" ${machine === "local" ? "selected" : ""}>This Mac</option>`,
        ...(K.demo.remoteHosts || []).map(item => `<option value="${K.escape(item.id)}" ${machine === item.id ? "selected" : ""}>Linux: ${K.escape(item.name)}</option>`)].join("");
    const repositories = K.repositoryCatalog.filter(repo => K.repositoryMachine(repo.id) === machine)
        .filter(repo => !K.demo.tagFilter || K.repositoryRecords(repo.id).some(({ record }) => K.isTagFiltered(record)))
        .sort((a, b) => K.repositoryRecency(b.id) - K.repositoryRecency(a.id)
            || a.label.localeCompare(b.label))
        .map(repositoryGroup).join("");
    const empty = machine === "local" ? "No local repositories yet." : "No repositories on this Linux machine yet.";
    return `<div class="sidebar-inner"><a class="brand-link" aria-label="KétPlus Pro" href="${K.url("workspace")}"><img class="brand-wordmark brand-wordmark-light" src="../assets/brand/ketplus-v1/ketplus-logo-light.svg" alt="KétPlus Pro" width="2878" height="720" /><img class="brand-wordmark brand-wordmark-dark" src="../assets/brand/ketplus-v1/ketplus-logo-dark.svg" alt="KétPlus Pro" width="2878" height="720" /><span class="brand-compact">${K.mark()}</span></a>
    ${K.link("search", "<span>Quick search</span><kbd>⌘ K</kbd>", "search", {}, "search-launch")}
    <nav aria-label="Main navigation">${nav}</nav>
    <div class="sidebar-context"><div class="sidebar-rule"></div>
    <div class="machine-switcher"><div class="machine-select-wrap"><select id="machine-select" aria-label="Machine for repositories" title="${machine === "local" ? "Local repositories on this Mac" : `Linux machine ${K.escape(host?.name || "unavailable")} · ${host?.status === "connected" ? "Connected" : "Offline"}`}">${machineOptions}</select>${K.icon("chevron-down")}</div>${K.ib("plus", "Add repository", "repository")}</div>
    ${host?.status === "offline" ? `<p class="machine-note">Offline · ${K.link("connections", "Reconnect in Connections", null, { remote: "offline" })}</p>` : ""}
    ${K.tagFilterBar?.() || ""}
    <nav class="repository-groups" aria-label="Repositories and conversations">${repositories || `<p class="repository-empty">${K.demo.tagFilter ? "No conversations with this tag." : empty}</p>`}</nav></div>
    <div class="sidebar-bottom">${K.query.get("userMenu") === "open" ? `<div class="user-menu" role="menu"><div class="user-menu-heading"><span class="avatar">AN</span><div><b>An Nguyen</b><small>Personal workspace</small></div></div><div class="user-menu-rule"></div>${K.link("settings", "<span>Settings</span><kbd>⌘ ,</kbd>", "settings", { userMenu: null }, "user-menu-item")}${K.link("connections", "<span>Account & devices</span>", "unplug", { userMenu: null }, "user-menu-item")}</div>` : ""}
    <button type="button" class="identity" data-action="user-menu" aria-expanded="${K.query.get("userMenu") === "open" ? "true" : "false"}"><span class="avatar">AN</span><span><b>An Nguyen</b><small>Personal workspace</small></span>${K.icon(K.query.get("userMenu") === "open" ? "chevron-up" : "chevron-down")}</button></div></div>`;
};
K.statusbar = () =>
    `<footer class="statusbar"><span>${K.icon("circle-check")} Local workspace</span><span class="status-right"><span>UTF-8</span><span>LF</span><span>KétPlus Pro</span>${K.icon("keyboard")}</span></footer>`;
K.mount = () => {
    if (K.page === "atlas-preview" && K.query.get("embed") === "screen") {
        document.body.classList.add("embedded-screen");
        document.querySelector("#app").innerHTML = K.onboarding();
        return;
    }
    const content = K.views[K.page]();
    document.querySelector("#app").innerHTML = K.template("shell", {
        sidebar: K.sidebar(),
        main: `<div class="desktop-main">${content}${K.productDialog?.() || ""}${K.page === "editor" ? K.statusbar() : ""}</div>`,
    });
    const userText = document.querySelector(".user-bubble-text");
    if (userText && userText.scrollHeight > userText.clientHeight + 1)
        userText.nextElementSibling.hidden = false;
    const receipts = {
        appearance: "Appearance applied to your workspace.",
        connection: "Reconnected · demo data.",
        cli: "Demo CLI selected.",
    };
    if (receipts[K.query.get("saved")]) K.toast(receipts[K.query.get("saved")]);
};
