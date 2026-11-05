/* ADR 0027 agent messaging: numbered conversations message each other. Synthetic prototype state only. */
// Sample exchange: "Refine navigation" asked "Review the startup flow" for findings, which started its turn.
K.agentPeers = (scope = K.scope) => {
    const sender = scope.conversations.navigation, receiver = scope.conversations.startup;
    return sender && receiver ? { sender, receiver } : null;
};
K.agentNamed = (record, scope = K.scope) => `@agent/${K.conversationNumber(scope, record)} · ${record.title}`;
K.agentRequestText = "Review the desktop startup flow and tell me whether conversation activity is restored before the sidebar paints. Reply with your findings.";
K.agentReplyText = "Activity is restored before the sidebar paints: the journal page loads first and the rows render from it. The only gap is the unread cursor, which arrives one frame later.";
K.agentBubble = (label, text) =>
    `<article class="user-message agent-message"><div class="agent-bubble"><div class="agent-origin">${K.icon("messages-square")}<span>${K.escape(label)}</span></div><div class="agent-bubble-text">${K.escape(text)}</div></div></article>`;
K.agentNote = (text, held = false) =>
    `<div class="agent-note${held ? " held" : ""}" role="status">${K.icon(held ? "pause-circle" : "send")}<span><b>Agent message</b>${K.escape(text)}</span></div>`;
K.agentToolRow = number =>
    `<div class="activity-row agent-tool-row"><span class="activity-prefix">Called</span><code>ketplus_agents · agents_send</code><span class="agent-tool-target">@agent/${K.escape(number)}</span></div>`;

{
    const transcript = K.transcript;
    K.transcript = () => {
        let body = transcript();
        const peers = K.agentPeers();
        if (!peers || !["running", "ready", "stopped", "question"].includes(K.state)) return body;
        const { sender, receiver } = peers;
        const userArticle = /<article class="user-message">[\s\S]*?<\/article>/;
        if (K.current === receiver && receiver.agentFrom && K.isActiveTurn(receiver)) {
            // The receiver's opening row names its sender instead of reading as the user's text.
            const provider = sender.provider || "Codex";
            return body.replace(userArticle,
                K.agentBubble(`Message from ${K.agentNamed(sender)} · ${provider}`, K.agentRequestText));
        }
        if (K.current !== sender || K.current.latestPrompt) return body;
        const number = K.conversationNumber(K.scope, receiver);
        body = body.replace(/(<div class="user-bubble-text"[^>]*>)[\s\S]*?(<\/div>)/,
            `$1${K.escape(`Ask @agent/${number} to review the startup flow and reply with what it finds.`)}$2`);
        const fixture = K.query.get("agent");
        const note = fixture === "held"
            ? K.agentNote(`Message to @agent/${number} is held because the agents exchanged too many messages. It is delivered with that conversation's next message from you.`, true)
            : K.agentNote(`Sent to ${K.agentNamed(receiver)} · started a turn · reply requested.`);
        body = body.replace(/(<\/section>)/, `${K.agentToolRow(number)}$1${note}`);
        if (fixture === "reply") {
            const reply = `${K.agentBubble(`Reply from ${K.agentNamed(receiver)} · ${receiver.provider || "Codex"}`, K.agentReplyText)}<article class="assistant-message transcript-turn"><div class="assistant-body"><div class="answer final-answer"><p>@agent/${K.escape(number)} confirmed the restore order. I will keep the unread cursor fix in this branch.</p></div></div></article>`;
            // The reply opens the next turn, right after the turn that sent the message.
            body = body.replace(/(<article class="assistant-message[\s\S]*?<\/article>)/, `$1${reply}`);
        }
        return body;
    };
}

// Header: the @agent/ reference other conversations use, before the title.
{
    const workspace = K.views.workspace;
    K.views.workspace = () => {
        const html = workspace();
        if (K.state === "empty" || !K.current) return html;
        const number = K.conversationNumber(K.scope, K.current);
        return html.replace('<div class="agent-header-context"><h2>',
            `<div class="agent-header-context"><span class="header-number" aria-label="Agent reference @agent/${number}">@agent/${number}</span><h2>`);
    };
}

// `@agent/` in the composer completes other conversations' numbers; any other `@` completes files.
K.conversationMention = (value, cursor) => {
    let start = cursor - 1;
    while (start >= 0 && /[^\s@"$]/.test(value.charAt(start))) --start;
    if (start < 0 || value.charAt(start) !== "@" || (start > 0 && !/[\s(\[{'"]/.test(value.charAt(start - 1)))) return null;
    const typed = value.slice(start + 1, cursor);
    if (!typed.startsWith("agent/")) return null; // a plain `@` is a file mention
    let end = cursor;
    while (end < value.length && /[^\s@"$]/.test(value.charAt(end))) ++end;
    return { start, end, query: typed.slice(6).toLowerCase() };
};
K.conversationSuggestions = query => {
    const rows = [];
    for (const scope of Object.values(K.demo.scopes)) {
        for (const record of Object.values(scope.conversations || {})) {
            if (record === K.current) continue;
            const number = K.conversationNumber(scope, record);
            if (query && !number.startsWith(query) && !record.title.toLowerCase().includes(query)) continue;
            rows.push({ number, title: record.title, place: `${scope.repo} · ${scope.tree}` });
        }
    }
    return rows.slice(0, 6);
};
K.mentionMenu = (query, highlighted = 0) => {
    const rows = K.conversationSuggestions(query);
    const items = rows.length
        ? rows.map((row, index) => `<button type="button" role="option" class="mention-option${index === highlighted ? " highlighted" : ""}" aria-selected="${index === highlighted}" data-conversation-number="${row.number}"><span class="conversation-number">@agent/${row.number}</span><span class="mention-title">${K.escape(row.title)}</span><small>${K.escape(row.place)}</small></button>`).join("")
        : `<div class="mention-empty">No conversation matches @agent/${K.escape(query)}</div>`;
    return `<div class="mention-menu" role="listbox" aria-label="Conversations"><div class="mention-menu-title">Conversations</div>${items}</div>`;
};
{
    const composer = K.composer;
    K.composer = (...args) => {
        const html = composer(...args);
        if (K.query.get("mention") !== "conversation" || K.current?.draft) return html;
        return html.replace("></textarea>", ">Ask @agent/</textarea>")
            .replace('<div class="composer-wrap">', `<div class="composer-wrap">${K.mentionMenu("")}`);
    };
}
K.mentionState = null;
K.closeMentionMenu = () => { document.querySelector(".mention-menu")?.remove(); K.mentionState = null; };
K.updateMentionMenu = prompt => {
    const mention = K.conversationMention(prompt.value, prompt.selectionStart);
    if (!mention) return K.closeMentionMenu();
    K.mentionState = { ...mention, highlighted: K.mentionState?.query === mention.query ? K.mentionState.highlighted : 0 };
    document.querySelector(".mention-menu")?.remove();
    prompt.closest(".composer-wrap")?.insertAdjacentHTML("afterbegin", K.mentionMenu(mention.query, K.mentionState.highlighted));
};
K.applyMention = number => {
    const prompt = document.querySelector("#prompt");
    const mention = prompt && K.conversationMention(prompt.value, prompt.selectionStart);
    if (!mention) return K.closeMentionMenu();
    prompt.setRangeText(`@agent/${number} `, mention.start, mention.end, "end");
    prompt.dispatchEvent(new Event("input", { bubbles: true }));
    K.closeMentionMenu();
    prompt.focus();
};
document.addEventListener("input", event => {
    if (event.target.id === "prompt") K.updateMentionMenu(event.target);
});
document.addEventListener("mousedown", event => {
    const option = event.target.closest("[data-conversation-number]");
    if (option) { event.preventDefault(); K.applyMention(option.dataset.conversationNumber); }
    else if (!event.target.closest(".mention-menu, #prompt")) K.closeMentionMenu();
});
document.addEventListener("keydown", event => {
    const menu = document.querySelector(".mention-menu");
    if (!menu || event.target.id !== "prompt") return;
    const options = [...menu.querySelectorAll(".mention-option")];
    if (event.key === "Escape") { event.preventDefault(); return K.closeMentionMenu(); }
    if (!options.length) return;
    const current = Math.max(0, options.findIndex(option => option.classList.contains("highlighted")));
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        const next = (current + (event.key === "ArrowDown" ? 1 : options.length - 1)) % options.length;
        options.forEach((option, index) => {
            option.classList.toggle("highlighted", index === next);
            option.setAttribute("aria-selected", String(index === next));
        });
        if (K.mentionState) K.mentionState.highlighted = next;
    } else if (event.key === "Enter" || event.key === "Tab") {
        event.preventDefault();
        event.stopImmediatePropagation();
        K.applyMention(options[current].dataset.conversationNumber);
    }
}, true);
