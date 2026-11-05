K.advanceOperation();
K.mount();
if (K.page === "atlas-builder" && K.query.get("dialog") === "model")
    document.body.insertAdjacentHTML("beforeend", K.modelDialog());
if (K.page === "atlas-builder" && K.query.get("dialog") === "permissions")
    document.body.insertAdjacentHTML(
        "beforeend",
        K.modal(
            "Agent permissions",
            `<p class="help">Choose permissions for the next Atlas generation turn.</p><div class="permission-options">${[
                ["read-only", "Read-only"],
                ["workspace-write", "Workspace"],
                ["full-access", "Full access"],
            ]
                .map(
                    ([value, title]) =>
                        `<label class="choice"><input type="radio" name="permission" value="${value}" ${(K.query.get("permission") || "workspace-write") === value ? "checked" : ""}><span><b>${title}</b></span></label>`,
                )
                .join("")}</div>`,
            K.button("Apply", "apply-permission", "check", "primary"),
        ),
    );
const pageState = () => ({
    state: K.state === "ready" ? null : K.state,
    panel: K.query.get("panel"),
    step: K.query.get("step"),
});
const navigateDialog = (dialog) => K.go(K.page, { ...pageState(), dialog });
const runDemo = () => K.submitPrompt();
const handlers = {
    "toggle-user-message": (button) => {
        const expanded = button.closest(".user-bubble").classList.toggle("expanded");
        button.textContent = expanded ? "Show less" : "Show more";
        button.setAttribute("aria-expanded", String(expanded));
    },
    "new-chat": () => K.newConversation(),
    "new-worktree": () => K.go("worktrees", { dialog: "create" }),
    repository: () => K.toast("Choose a folder to add a repository."),
    "expand-repositories": () =>
        K.go(K.page, {
            repositories: K.query.get("repositories") === "expanded" ? null : "expanded",
        }),
    "user-menu": () =>
        K.go(K.page, {
            userMenu: K.query.get("userMenu") === "open" ? null : "open",
        }),
    terminal: () =>
        K.go(["workspace", "atlas-builder"].includes(K.page) ? "workspace" : "editor", {
            panel: "terminal",
            state: K.page === "workspace" && K.state !== "ready" ? K.state : null,
        }),
    "close-panel": () =>
        K.go(K.page, {
            state: K.state === "ready" ? null : K.state,
            preview: K.query.get("preview"),
            file: K.query.get("file"),
        }),
    theme: () => K.go("settings"),
    models: () => navigateDialog("model"),
    permissions: () => navigateDialog("permissions"),
    "close-dialog": () => K.go(K.page, { ...pageState(), dialog: null }),
    "apply-model": () =>
        K.go(K.page, {
            ...pageState(),
            model: document.querySelector("input[name=model]:checked")?.value || "GPT-5.6 Luna",
            effort: document.querySelector("#effort-output").value,
        }),
    "reset-model": () => {
        document.querySelector("input[name=model]").checked = true;
        document.querySelector("#effort").value = 1;
        document.querySelector("#effort-output").value = "Medium";
    },
    "apply-permission": () =>
        K.go(K.page, {
            ...pageState(),
            permission: document.querySelector("input[name=permission]:checked").value,
        }),
    attach: () => document.querySelector("#attachment-input")?.click(),
    "remove-attachment": (button) => {
        K.current.attachment = null;
        button.closest(".attachment-chip").remove();
        K.syncComposerSend();
        if (document.querySelector("#attachment-input"))
            document.querySelector("#attachment-input").value = "";
    },
    suggest: (button) => {
        const text = button.dataset.prompt;
        document.querySelector("#prompt").value = text;
        document.querySelector("#prompt").focus();
    },
    stop: () => {
        clearTimeout(K.demoTimer);
        K.go(K.page, {
            state: K.page === "atlas-builder" ? "error" : "stopped",
        });
    },
    retry: runDemo,
    "copy-response": () => {
        K.toast("Response selected for copying.");
        const range = document.createRange();
        range.selectNodeContents(document.querySelector(".final-answer, .assistant-body"));
        const selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
        try {
            if (document.execCommand("copy")) K.toast("Response copied.");
        } catch (_) {}
    },
    "expand-activity-images": (button) => {
        button.parentElement.querySelectorAll("[hidden]").forEach(image => image.hidden = false);
        button.remove();
    },
    "preview-activity-image": (button) => {
        const label = button.dataset.label || "Activity image";
        const body = `<img src="${K.escape(button.dataset.src)}" alt="${K.escape(label)}">`;
        document.body.insertAdjacentHTML(
            "beforeend",
            K.modal(label, body, "", "activity-preview").replace(
                'data-action="close-dialog"',
                'data-action="close-activity-preview"',
            ),
        );
        K.focusDialog();
    },
    "close-activity-preview": (button) => button.closest(".modal-backdrop")?.remove(),
    "open-activity-site": () =>
        K.toast("Website activity opened: doc.qt.io/qt-6/qml-qtquick-listview.html"),
    refresh: () => K.toast("Demo snapshot refreshed."),
    "atlas-grid": (button) => {
        document.querySelector("#atlas-collection").classList.remove("list-mode");
        button.parentElement
            .querySelectorAll("button")
            .forEach((b) => b.classList.toggle("active", b === button));
    },
    "atlas-list": (button) => {
        document.querySelector("#atlas-collection").classList.add("list-mode");
        button.parentElement
            .querySelectorAll("button")
            .forEach((b) => b.classList.toggle("active", b === button));
    },
    "clear-atlas-search": () => {
        const input = document.querySelector("#atlas-search");
        input.value = "";
        input.dispatchEvent(new Event("input", { bubbles: true }));
    },
    "onboarding-next": () =>
        K.query.get("step") === "complete"
            ? K.go("workspace", { state: "empty" })
            : K.go("atlas-preview", { step: "complete" }),
    "next-screen": () => K.go("atlas-preview", { step: "complete" }),
    "previous-screen": () => K.go("atlas-preview"),
    "choose-cli": () => {
        K.go("atlas-preview", { saved: "cli" });
    },
    "install-cli": () => {
        K.toast("CLI installation simulated for this preview.");
        setTimeout(() => K.go("atlas-preview"), 800);
    },
    "pair-device": () => navigateDialog("pair"),
    "simulate-scan": () => navigateDialog("confirm"),
    "confirm-device": () => K.go("connections", { state: "paired" }),
    "expire-pair": () => navigateDialog("expired"),
    "new-pair-code": () => navigateDialog("pair"),
    "revoke-prompt": () => K.go("connections", { state: "paired", dialog: "revoke" }),
    "revoke-device": () => K.go("connections", { state: "revoked" }),
    reconnect: () => K.go("connections", { saved: "connection" }),
    signout: () => K.go("connections", { state: "signed-out" }),
    "iphone-details": () => {
        document.body.insertAdjacentHTML(
            "beforeend",
            K.modal(
                "iPhone 16",
                `<div class="device-facts"><span>App</span><b>KetPlus App · iOS</b><span>Access</span><b>Paired</b><span>Last active</span><b>Yesterday, 18:42</b></div>`,
                K.button("Close", "close-dialog"),
            ),
        );
        K.focusDialog();
    },
    "reset-appearance": () =>
        K.go("settings", {
            theme: "light",
            system: "false",
            fontSize: 15,
            editorSize: 13,
            terminalSize: 13,
            markdownSize: 14,
        }),
    "save-appearance": () =>
        K.go("workspace", {
            theme: window.KetPlusTheme.preference,
            conversationWidth: document.querySelector("#conversation-width")?.value || K.query.get("conversationWidth"),
            fontSize: document.querySelector("#ui-size")?.value || K.query.get("fontSize") || 15,
            editorSize:
                document.querySelector("#editor-size")?.value.replace(" px", "") ||
                K.query.get("editorSize") ||
                13,
            terminalSize:
                document.querySelector("#terminal-size")?.value.replace(" px", "") ||
                K.query.get("terminalSize") ||
                13,
            markdownSize:
                document.querySelector("#markdown-size")?.value.replace(" px", "") ||
                K.query.get("markdownSize") ||
                14,
            editorFont:
                document.querySelector("#editor-font")?.value ||
                K.query.get("editorFont") ||
                "SF Mono",
            saved: "appearance",
        }),
    "close-search": () => K.go("workspace"),
    "clear-search": () => {
        const input = document.querySelector("#spotlight-input");
        input.value = "";
        input.dispatchEvent(new Event("input", { bubbles: true }));
        input.focus();
    },
};
document.addEventListener("click", (event) => {
    const action = event.target.closest("[data-action]");
    if (action && !action.disabled) {
        const handler = handlers[action.dataset.action];
        if (handler) {
            event.preventDefault();
            handler(action);
        }
    }
});
document.addEventListener("submit", (event) => {
    event.preventDefault();
    const form = event.target;
    if (form.id === "prompt-form") {
        if (K.state === "missing" || K.state === "restore") return;
        const prompt = document.querySelector("#prompt");
        if (!prompt.value.trim() && !document.querySelector(".attachment-chip")) {
            K.toast("Enter a request or attach a file to get started.");
            prompt.focus();
            return;
        }
        runDemo();
    }
    if (form.id === "question-form") K.submitAnswer();
    if (form.id === "worktree-form") K.createWorktree(form);
    if (form.id === "signin-form") K.signIn(form);
    if (form.id === "terminal-form") {
        const input = document.querySelector("#terminal-command"),
            command = input.value.trim();
        const output = document.querySelector("#terminal-output");
        const responses = {
            help: "Demo commands: help, pwd, git status, git status --short, git diff --check, clear",
            pwd: "./ketplus-desktop",
            "git status":
                "On branch develop\nChanges not staged for commit:\n  modified: NavigationRail.qml\n  modified: WorktreeItem.qml\nChanges to be committed:\n  modified: ThemeManager.cpp",
            "git status --short": " M NavigationRail.qml\n M WorktreeItem.qml\nM  ThemeManager.cpp",
            "git diff --check": "✓ No whitespace errors.",
        };
        if (command === "clear") output.innerHTML = "";
        else if (command) {
            const row = document.createElement("div");
            const prompt = document.createElement("p");
            prompt.className = "terminal-prompt";
            prompt.textContent = "➜ " + command;
            const result = document.createElement("p");
            result.textContent =
                responses[command] ||
                "This is a demo terminal. Type help to see the available commands.";
            row.append(prompt, result);
            output.append(row);
        }
        input.value = "";
        input.scrollIntoView({ block: "nearest" });
    }
    if (form.id === "search-form") document.querySelector(".spotlight-result.highlighted")?.click();
});
document.addEventListener("input", (event) => {
    const input = event.target;
    if (input.id === "effort")
        document.querySelector("#effort-output").value = ["Low", "Medium", "High", "Max"][
            Number(input.value)
        ];
    if (input.id === "model-search")
        document
            .querySelectorAll(".model-option")
            .forEach(
                (row) =>
                    (row.hidden = !row.textContent
                        .toLowerCase()
                        .includes(input.value.toLowerCase())),
            );
    if (input.id === "spotlight-input")
        document.querySelector("#spotlight-results").innerHTML = K.searchResults(input.value);
    if (input.id === "atlas-search") {
        let count = 0;
        document.querySelectorAll(".atlas-card").forEach((card) => {
            card.hidden = !card.textContent.toLowerCase().includes(input.value.toLowerCase());
            if (!card.hidden) count++;
        });
        document.querySelector("#atlas-count").textContent =
            `${count} ${count === 1 ? "Atlas" : "Atlases"}`;
        document.querySelector("#atlas-no-results").hidden = count > 0;
    }
    if (input.id === "branch") {
        document.querySelector(".branch-preview span").textContent =
            "ketplus-desktop-" +
            (input.value || "feature/navigation").replace(/[^a-zA-Z0-9-]+/g, "-");
        document.querySelector("#branch-error").textContent = "";
        input.removeAttribute("aria-invalid");
    }
});
document.addEventListener("change", (event) => {
    const input = event.target;
    if (input.id === "attachment-input" && input.files[0]) {
        const file = input.files[0];
        const image = file.type.startsWith("image/");
        if (file.size > (image ? 20 * 1024 * 1024 : 512 * 1024)) {
            K.toast(
                image
                    ? "Images must be smaller than 20 MiB."
                    : "Text files must be smaller than 512 KiB.",
            );
            input.value = "";
            return;
        }
        if (!image && !/\.(txt|md|js|ts|json|css|html)$/i.test(file.name)) {
            K.toast("Choose an image or a supported text file.");
            input.value = "";
            return;
        }
        K.current.attachment = { name: file.name, image, size: file.size };
        K.syncComposerSend();
        document.querySelector("#attachment-chips").innerHTML =
            `<span class="attachment-chip">${K.icon(image ? "image" : "file-text")}${K.escape(file.name)}<button type="button" data-action="remove-attachment" aria-label="Remove attachment">${K.icon("x")}</button></span>`;
    }
    if (input.id === "follow-system") window.KetPlusTheme.setFollowSystem(input.checked);
    if (input.id === "ui-size")
        document.querySelector("#font-ui-preview").style.fontSize = input.value + "px";
    if (input.id === "editor-size")
        document.querySelector("#font-editor-preview").style.fontSize = input.value;
    if (input.id === "terminal-size")
        document.querySelector("#font-terminal-preview").style.fontSize = input.value;
    if (input.id === "markdown-size")
        document.querySelector("#font-markdown-preview").style.fontSize = input.value;
    if (input.id === "editor-font")
        document.querySelector("#font-editor-preview").style.fontFamily =
            input.value === "SF Mono" ? "SFMono-Regular, monospace" : input.value;
});
K.focusDialog = () => {
    const dialog = document.querySelector("[role=dialog]");
    if (dialog)
        setTimeout(() => {
            const target =
                dialog.querySelector("input:not([type=radio]):not([type=range]),select,textarea") ||
                dialog.querySelector("button,a");
            target?.focus();
        }, 30);
};
K.focusDialog();
if (document.querySelector("[role=dialog]"))
    document
        .querySelectorAll(".view,.sidebar-inner,.statusbar")
        .forEach((element) => (element.inert = true));
document.addEventListener("keydown", (event) => {
    const key = event.key.toLowerCase();
    if ((event.metaKey || event.ctrlKey) && key === "k") {
        event.preventDefault();
        K.go("search");
        return;
    }
    if ((event.metaKey || event.ctrlKey) && key === ",") {
        event.preventDefault();
        K.go("settings");
        return;
    }
    if ((event.metaKey || event.ctrlKey) && event.shiftKey && key === "g") {
        event.preventDefault();
        K.go("changes");
        return;
    }
    if (event.key === "Escape") {
        if (K.page === "search") K.go("workspace");
        else if (document.querySelector("[role=dialog]")) handlers["close-dialog"]();
        return;
    }
    if (
        event.target.id === "prompt" &&
        event.key === "Enter" &&
        !event.shiftKey &&
        !event.isComposing
    ) {
        event.preventDefault();
        if (!["sending", "stopping", "steering", "answer-submitting"].includes(K.current.turn))
            document.querySelector("#prompt-form").requestSubmit();
    }
    if (K.page === "search" && ["ArrowDown", "ArrowUp"].includes(event.key)) {
        const rows = [...document.querySelectorAll(".spotlight-result")];
        if (!rows.length) return;
        event.preventDefault();
        let index = rows.findIndex((row) => row.classList.contains("highlighted"));
        index = (index + (event.key === "ArrowDown" ? 1 : -1) + rows.length) % rows.length;
        rows.forEach((row, i) => row.classList.toggle("highlighted", i === index));
        rows[index].scrollIntoView({ block: "nearest" });
    }
    if (event.key === "Tab") {
        const dialog = document.querySelector("[role=dialog]");
        if (!dialog) return;
        const elements = [
            ...dialog.querySelectorAll(
                "a[href],button:not([disabled]),input:not([disabled]),textarea:not([disabled]),select:not([disabled])",
            ),
        ].filter((el) => el.offsetParent !== null);
        const first = elements[0],
            last = elements.at(-1);
        if (event.shiftKey && document.activeElement === first) {
            event.preventDefault();
            last?.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
            event.preventDefault();
            first?.focus();
        }
    }
});
