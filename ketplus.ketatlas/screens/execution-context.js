/* Execution selection belongs to the conversation, not the workspace navigation. */
(() => {
  let state = K.query.get("contextSwitch") || "ready";
  K.current.executionTree ||= K.query.get("executionTree") || K.tree;
  K.current.contextRevision ||= 1;
  const dialog = document.createElement("dialog");
  dialog.className = "execution-context-dialog";
  dialog.setAttribute("aria-labelledby", "execution-context-title");
  dialog.innerHTML = '<div class="row"><h2 id="execution-context-title">Execution worktree</h2><button class="btn ghost" type="button" data-close>Done</button></div><p>This conversation keeps its history and draft. Choose where the next message runs.</p><div class="execution-context-options"></div><p role="status" aria-live="polite"></p><button class="btn ghost" type="button" data-refresh hidden>Refresh worktrees</button>';
  document.body.append(dialog);
  const status = dialog.querySelector('[role="status"]');
  status.tabIndex = -1;
  const closeButton = dialog.querySelector('[data-close]');
  const options = dialog.querySelector('.execution-context-options');
  const refresh = dialog.querySelector('[data-refresh]');
  const currentRow = document.createElement("div"); currentRow.className = "worktree-current";
  const search = document.createElement("input"); search.type = "search"; search.className = "worktree-search";
  search.placeholder = "Search branch or folder"; search.setAttribute("aria-label", "Search worktrees");
  search.value = K.query.get("worktreeQuery") || "";
  const count = document.createElement("p"); count.className = "worktree-count"; count.setAttribute("aria-live", "polite");
  const pager = document.createElement("div"); pager.className = "worktree-pager";
  const previous = document.createElement("button"); previous.type = "button"; previous.textContent = "Previous";
  const next = document.createElement("button"); next.type = "button"; next.textContent = "Next 20";
  const all = document.createElement("button"); all.type = "button"; all.textContent = "Browse all 1,248 worktrees";
  pager.append(previous, next, all);
  options.before(currentRow, search, count); options.after(pager);
  const seeds = [...new Set([K.tree, "develop", "feature/atlas-live", "fix/scroll-anchor"])].slice(0, 3);
  const catalog = seeds.concat(Array.from({length: 1248 - seeds.length}, (_, i) =>
    (i % 8 === 0 ? "feature/atlas-" : i % 3 === 0 ? "fix/workspace-" : "feature/editor-") + String(i + 1).padStart(4, "0")));
  // Synthetic recorded activity in this conversation; selecting alone is not activity.
  const modelTouched = new Set([K.tree, "feature/atlas-live", "feature/atlas-0001"]);
  function activityBadge(tree) {
    if (!modelTouched.has(tree)) return null;
    const badge = document.createElement("span"); badge.className = "worktree-model-used";
    badge.textContent = "✦";
    badge.setAttribute("role", "img");
    badge.setAttribute("aria-label", "Model used in this conversation");
    badge.title = "The model worked in this worktree during this conversation";
    return badge;
  }
  const rankedCatalog = [...catalog].sort((a, b) => Number(modelTouched.has(b)) - Number(modelTouched.has(a)));
  let page = 0, browsing = Boolean(search.value);
  search.oninput = () => { page = 0; browsing = Boolean(search.value.trim()); render(); };
  previous.onclick = () => { page = Math.max(0, page - 1); render(); options.scrollTop = 0; };
  next.onclick = () => { page++; render(); options.scrollTop = 0; };
  all.onclick = () => { browsing = true; page = 0; render(); };

  function blocked() { return ["starting", "running", "waiting", "waiting_for_input", "stopping"].includes(K.current.turn) || state === "busy"; }
  function render() {
    options.replaceChildren();
    currentRow.textContent = "Current · " + K.current.executionTree;
    const currentBadge = activityBadge(K.current.executionTree); if (currentBadge) currentRow.append(currentBadge);
    const terms = search.value.toLowerCase().trim().split(/\s+/).filter(Boolean);
    const matches = rankedCatalog.filter(tree => terms.every(term => (tree + " ketplus-" + tree.replaceAll("/", "-")).toLowerCase().includes(term)));
    const recent = rankedCatalog.filter(tree => tree !== K.current.executionTree).slice(0, 4);
    const visible = browsing ? matches.slice(page * 20, page * 20 + 20) : recent;
    count.textContent = browsing ? (matches.length ? `${page * 20 + 1}–${Math.min(page * 20 + 20, matches.length)} of ${matches.length.toLocaleString("en-US")} matches` : "No worktrees match. Try a branch or folder name.") : "Model-touched first · 1,248 worktrees in this repository";
    previous.hidden = !browsing; previous.disabled = page === 0;
    next.hidden = !browsing; next.disabled = (page + 1) * 20 >= matches.length;
    all.hidden = browsing;
    visible.forEach(tree => {
      const button = document.createElement("button");
      button.type = "button"; button.className = "btn ghost";
      button.textContent = tree + (tree === K.current.executionTree ? " · Current" : "");
      const folder = document.createElement("small"); folder.className = "worktree-folder";
      folder.textContent = "ketplus-" + tree.replaceAll("/", "-"); button.append(folder);
      const badge = activityBadge(tree); if (badge) button.insertBefore(badge, folder);
      button.setAttribute("aria-pressed", String(tree === K.current.executionTree));
      button.disabled = blocked() || state === "offline" || (state === "switching" || state === "stale" || state === "denied") || tree === K.current.executionTree;
      button.onclick = () => {
        if (state === "stale" || state === "denied") { render(); return; }
        state = "switching"; render();
        status.focus();
        setTimeout(() => {
          if (state !== "switching") return;
          K.current.executionTree = tree; K.current.contextRevision++; state = "ready";
          const chip = document.querySelector('.execution-worktree-button');
          if (chip) chip.textContent = tree + " ▾";
          K.query.set("executionTree", tree); K.query.set("contextSwitch", "switched");
          history.replaceState(null, "", K.url("workspace", { contextSwitch: "switched", executionTree: tree }));
          let note = document.querySelector('.execution-context-note');
          if (!note) { note = document.createElement("p"); note.className = "execution-context-note"; document.querySelector('.chat-scroll')?.append(note); }
          note.textContent = "Next message runs in " + tree + ". Earlier file links and running Atlases keep their source worktree.";
          dialog.close();
        }, 350);
      };
      options.append(button);
    });
    status.textContent = blocked() ? "Finish or stop the current turn before switching." : state === "offline" ? "Mac offline. Reconnect before switching." : state === "stale" ? "The execution worktree changed on another device. Refresh before choosing again." : state === "denied" ? "Access to the selected worktree was revoked." : state === "switching" ? "Switching worktree…" : "The workspace browser and any running Atlas keep their own worktree. This changes only the next turn's execution context.";
    closeButton.disabled = state === "switching";
    refresh.hidden = state !== "stale" && state !== "denied";
  }
  refresh.onclick = () => { state = "ready"; K.current.contextRevision++; render(); };
  closeButton.onclick = () => { if (state !== "switching") dialog.close(); };
  dialog.addEventListener("cancel", event => { if (state === "switching") event.preventDefault(); });
  function attach() {
    const old = document.querySelector('.agent-scope-chip:not(.execution-worktree-button)');
    if (!old) return;
    const chip = document.createElement("button"); chip.type = "button";
    chip.className = "agent-scope-chip execution-worktree-button";
    chip.setAttribute("aria-label", "Change execution worktree");
    chip.textContent = (K.current.executionTree || K.tree) + " ▾";
    chip.onclick = () => { render(); dialog.showModal(); };
    old.replaceWith(chip);
  }
  attach();
  new MutationObserver(attach).observe(document.getElementById('app'), { childList: true, subtree: true });
  if (K.query.has("contextSwitch") && state !== "switched" && state !== "current") { render(); dialog.showModal(); }
})();
