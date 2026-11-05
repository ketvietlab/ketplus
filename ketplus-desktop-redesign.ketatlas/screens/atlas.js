K.thumbnail = (variant = "desktop") =>
    variant === "mobile"
        ? `<div class="mini-phone"><i></i><b>Your work.<br>Always with you.</b><p>Continue Agent sessions from your phone.</p><span></span><span></span><span></span><div class="mini-phone-cta">Connect your computer</div></div><div class="mini-phone"><b>Your<br>workspace</b><i></i><span></span><span></span><p>ketplus-desktop</p><span></span><span></span></div>`
        : `<div class="mini-app"><div class="mini-app-top"><i></i><i></i><i></i></div><div class="mini-app-body"><div class="mini-app-rail"><i></i><i></i><i></i><i></i><i></i><i></i></div><div class="mini-app-main"><b>${variant === "dashboard" ? "Everything in one workspace." : "Welcome to KetPlus."}</b><i></i><i></i><div class="mini-blocks"><div><span></span>${variant === "dashboard" ? "12 tasks" : "Open project"}</div><div><span></span>${variant === "dashboard" ? "3 running" : "Agent"}</div></div><i></i><i></i><i></i></div></div></div>`;
K.onboarding = () => {
    const step = K.query.get("step") || "workspace";
    if (step === "missing")
        return `<div class="demo-onboarding"><header><span>${K.mark()} <b>KétPlus</b></span><small class="muted">Reconnect workspace</small></header><div class="onboarding-step">PROJECT UNAVAILABLE</div><h2>Let’s find your project.</h2><p>The workspace folder has moved. Choose an available project to continue.</p><div class="demo-workspaces"><div class="banner warning">${K.icon("folder")}<span>ketplus-desktop<br><small>Folder not found</small></span></div></div>${K.link("atlas-preview", "Choose project", "folder-open", { step: "complete" }, "btn primary")}<footer>Your conversations are preserved.</footer></div>`;
    return `<div class="demo-onboarding"><header><span>${K.mark()} <b>KétPlus</b></span><small class="muted">Get started</small></header><div class="onboarding-step">${step === "complete" ? "READY TO CREATE SOMETHING NEW" : "STEP 01 / 02"}</div><h2>${step === "complete" ? "Your workspace is ready." : "Great work<br>starts here."}</h2><p>${step === "complete" ? "Your project is open. Start a conversation with your Agent to explore the codebase together." : "Open a project and explore what you want to build with your Agent."}</p><div class="demo-workspaces">${step === "complete" ? `<div class="onboarding-success">${K.icon("circle-check")}<b>ketplus-desktop</b><small>develop · Local workspace</small></div>` : `<div class="demo-workspace selected">${K.icon("folder-open")}<span><b>ketplus-desktop</b><small>Recent project · develop</small></span>${K.icon("circle-check")}</div><div class="demo-workspace">${K.icon("folder")}<span><b>docs-workspace</b><small>Recent project · main</small></span></div>`}</div>${K.button(step === "complete" ? "Start with your Agent" : "Open workspace", "onboarding-next", step === "complete" ? "messages-square" : "arrow-right", "primary")}<footer>${step === "complete" ? "Every idea is a starting point." : "Designed for what you are creating."}</footer></div>`;
};
K.views["atlas-library"] = () =>
    `<section class="view"><div class="page-content"><div class="page-heading"><div><div class="eyebrow">FROM IDEA TO EXPERIENCE</div><h1>Your Atlases</h1><p>Design, test, and connect screens in a single flow.</p><p class="atlas-scope">${K.icon("folder")}<b>ketplus-desktop</b></p></div>${K.button("Create Atlas", "create-atlas", "plus", "primary")}</div>${
        K.state === "empty"
            ? `<div class="empty-state">${K.icon("network")}<h2>Your first idea is waiting.</h2><p>Describe what you want to build. Your Agent will create screens and interactive flows in this workspace.</p>${K.button("Create your first Atlas", "create-atlas", "plus", "primary")}</div>`
            : `<div class="atlas-toolbar"><label class="search-field">${K.icon("search")}<input id="atlas-search" aria-label="Search Atlases" placeholder="Search Atlases in this workspace…"></label><span class="spacer"></span><span id="atlas-count">3 Atlases</span><div class="pill-tabs"><button class="active" data-action="atlas-grid" aria-label="Grid view">${K.icon("layout-grid")}</button><button data-action="atlas-list" aria-label="List view">${K.icon("list-filter")}</button></div></div><div class="atlas-grid" id="atlas-collection">${[
                  [
                      "Onboarding experience",
                      "From the first launch to your first Agent conversation.",
                      "desktop",
                      "6 screens",
                      "2 flows",
                      "Just now",
                  ],
                  [
                      "KetPlus Mobile",
                      "Connect and continue working from your phone.",
                      "mobile",
                      "8 screens",
                      "3 flows",
                      "Yesterday",
                  ],
                  [
                      "Workspace",
                      "Navigation, files, and context for each worktree.",
                      "dashboard",
                      "5 screens",
                      "2 flows",
                      "Sep 10",
                  ],
              ]
                  .map(([name, description, variant, screens, flows, time]) =>
                      K.link(
                          "atlas-builder",
                          `<div class="atlas-thumbnail">${K.thumbnail(variant)}</div><div class="atlas-card-details"><h3>${name}</h3><p>${description}</p><div class="atlas-meta"><span>${K.icon("panels-top-left")}${screens}</span><span>${K.icon("git-branch")}${flows}</span><time>${time}</time></div></div>`,
                          null,
                          {
                              project: name,
                              bundle: Object.keys(K.bundleTitles).find(
                                  (id) => K.bundleTitles[id] === name,
                              ),
                              chat: "collapsed",
                          },
                          "atlas-card",
                      ),
                  )
                  .join(
                      "",
                  )}</div><div id="atlas-no-results" class="empty-state" hidden><h2>No Atlases found.</h2><p>Try another name or clear your search.</p>${K.button("Clear search", "clear-atlas-search", "x")}</div><div class="atlas-section-note">${K.icon("folder-open")} Bundles in worktree <b>${K.escape(K.tree)}</b> <span>·</span> Updates automatically when files change</div><div class="page-section"><div class="section-heading"><h3>Continue where you left off</h3>${K.link("atlas-builder", "Open conversation", "arrow-up-right", {}, "text-link")}</div><table class="recent-atlas"><tbody><tr><td>${K.link("atlas-builder", "Atlas: Onboarding experience", "messages-square")}</td><td>develop</td><td>Just updated</td><td>${K.badge("Ready to preview", "success")}</td></tr><tr><td>${K.link("atlas-builder", "Atlas: Workspace experience", "messages-square", { state: "empty" })}</td><td>develop</td><td>Sep 10, 14:20</td><td>${K.badge("Draft")}</td></tr></tbody></table></div>`
    }</div></section>`;
K.views["atlas-builder"] = () => {
    const empty = K.state === "empty",
        building = K.state === "building",
        error = K.state === "error";
    return `<section class="view"><div class="builder-layout">${K.atlasMap(K.hasAtlasMap(), building, error)}<aside class="builder-chat" id="atlas-ai-chat" aria-label="Atlas AI Chat" ${K.query.get("chat") === "collapsed" ? "hidden" : ""}><div class="context-tabs"><span>${K.icon("messages-square")} AI Chat</span>${K.ib("panel-right-close", "Hide AI Chat", "hide-atlas-chat")}</div><div class="builder-conversation-title"><span>${K.escape(K.bundleTitles[K.bundle] || "Unavailable Atlas")}</span>${K.statusLabel(K.state)}</div><div class="builder-scroll">${empty ? `<div class="builder-welcome">${K.mark("large")}<h2>What do you<br>have in mind?</h2><p>Describe the experience you want to create. Your Agent will help turn it into interactive screens.</p></div><div class="builder-suggestion">${K.button("Design onboarding for KetPlus", "suggest", "network", "ghost", 'data-prompt="Design onboarding for KetPlus Desktop using Két Design System."')}</div>` : `<div class="builder-brief">${K.requestFlowTag()}<p>${K.escape(K.query.get("brief") || "Design an onboarding flow for KetPlus Desktop using Két Design System, from opening a project to the first conversation with an Agent.")}</p></div><div class="builder-reply"><div class="row">${K.mark()}<b>Codex</b></div><p>${K.atlasFlows[K.query.get("requestFlow")] ? `I will focus on <strong>${K.atlasFlows[K.query.get("requestFlow")].title}</strong> and its connected screens. Shared screens or styles may also affect other flows; review their impact before accepting the result.` : "I will create a focused experience that helps people open a project and get started right away."}</p><div class="build-checklist"><div>${K.icon("circle-check")} Read the context and current features</div><div>${K.icon("circle-check")} Apply Két Design System</div><div>${K.icon(building ? "clock" : "circle-check")} Create screens and interactive flows</div><div>${K.icon(building || error ? "clock" : "circle-check")} Validate the bundle structure</div></div>${building ? `<div class="running-message"><span class="working-bars"><i></i><i></i><i></i></span>Generating screens…</div>` : error ? `<div class="banner error">${K.icon("triangle-alert")}<div><strong>Generation interrupted</strong><p>Continue from the content already generated.</p></div></div><div class="row top-gap">${K.button("Continue generating", "retry", "refresh-cw", "primary")}</div>` : `<div class="bundle-receipt"><h3>${K.icon("circle-check")} ${K.current.bundleReady ? "Validated preview available" : "Turn finished · preview pending validation"}</h3>${K.atlasFlows[K.query.get("requestFlow")] ? `<div class="flow-update-receipt">Requested focus: <strong>${K.atlasFlows[K.query.get("requestFlow")].title}</strong></div>` : ""}<p>${K.escape(K.bundle)}.ketatlas<br>4 screens · 2 flows · Két Design System</p>${K.link("atlas-preview", "Open preview", "arrow-up-right", {}, "btn")}</div><p class="help">Keep refining the design in this conversation.</p>`}</div>`}</div>${K.composer(true)}</aside></div></section>`;
};
K.views["atlas-preview"] = () =>
    K.state === "missing-cli"
        ? `<section class="view"><div class="empty-state">${K.icon("terminal")}<h2>Select KetAtlas CLI to continue.</h2><p>KetPlus could not find KetAtlas on this computer.</p><div class="row">${K.button("Choose executable", "choose-cli", "folder-open")}${K.button("Install with npm", "install-cli", "download", "primary")}</div><small class="muted">This demo does not install software on your computer.</small></div></section>`
        : `<section class="view"><div class="atlas-preview-layout"><div class="atlas-preview-bar">${K.link("atlas-library", "", "arrow-left", {}, "btn icon-btn")}<strong>${K.escape(K.query.get("project") || "Onboarding experience")}</strong>${K.badge("Atlas")}<span class="spacer"></span><span class="muted">${K.query.get("step") === "complete" ? "02 · Ready" : "01 · Open workspace"}</span>${K.link("atlas-builder", "Continue designing", "messages-square", {}, "btn")}</div><div class="prototype-stage">${K.onboarding()}</div><div class="preview-bottom">${K.button("Previous screen", "previous-screen", "chevron-left", "ghost")}<span>${K.query.get("step") === "complete" ? "02" : "01"} / 02 · Getting started</span>${K.button("Next screen", "next-screen", "chevron-right", "ghost")}</div></div></section>`;
