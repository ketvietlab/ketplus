/* Model-picker prototype only. No provider calls or cache inference. */
K.compactWindow = {
    minimum: 100000,
    maximum: 1000000,
    presets: [["auto", "Auto"], ["272000", "272k"], ["500000", "500k"], ["1000000", "1M"], ["custom", "Custom"]],
    format(value) { return value == null ? "Auto" : value === 1000000 ? "1M" : `${Number(value).toLocaleString("en-US")} tokens`; },
    validate(value, capacity = 1000000) {
        if (value === "auto") return { valid: true, value: null };
        const tokens = Number(value);
        const maximum = Math.min(capacity || this.maximum, this.maximum);
        return /^\d+$/.test(String(value)) && Number.isSafeInteger(tokens) && tokens >= this.minimum && tokens <= maximum
            ? { valid: true, value: tokens }
            : { valid: false, error: `Enter a whole number from 100,000 to ${maximum.toLocaleString("en-US")} tokens.` };
    },
    capacity(model) { return model === "Opus 5.5" ? 1000000 : null; },
    modelBlocked() { return K.configurationLocked() || ["denied", "offline", "background", "running", "saving", "loading"].includes(K.query.get("compactState")) || K.query.get("bg") === "running"; },
    blocker(provider) {
        const state = K.query.get("compactState");
        if (state === "denied") return "You have read-only access to this conversation.";
        if (state === "offline" || K.current.connection !== "connected") return "Reconnect to change these settings. Your selection stays here.";
        if (state === "background" || K.query.get("bg") === "running") return "Wait for background tasks to finish before changing these settings.";
        if (state === "running" || K.isActiveTurn()) return "Wait for this turn to finish before changing these settings.";
        if (state === "saving") return "Applying settings…";
        if (state === "loading") return "Loading conversation settings…";
        if (provider === "Cursor" || state === "unsupported") return "This provider does not support a custom compact window.";
        if (!K.current.available) return "This conversation is unavailable. Reconnect to try again.";
        return "";
    },
};
if (K.query.has("compactWindow")) {
    const value = K.query.get("compactWindow");
    const resolved = K.compactWindow.validate(value);
    if (resolved.valid) K.current.compactWindow = resolved.value;
}
K.compactDraft = K.current.compactWindow == null ? "auto" : String(K.current.compactWindow);
if (K.query.get("compactState") === "invalid") K.compactDraft = "99000";
K.compactWindowMarkup = (provider, model) => {
    const policy = K.compactWindow;
    const capacity = policy.capacity(model);
    const blocker = policy.blocker(provider);
    const draft = provider === "Cursor" ? "auto" : K.compactDraft;
    const custom = !policy.presets.some(([value]) => value === draft);
    const validation = policy.validate(draft, capacity);
    const state = K.query.get("compactState");
    return `<section class="compact-control" aria-labelledby="compact-heading">
        <div class="compact-heading"><h3 id="compact-heading">Auto-compact</h3><span class="model-capacity" id="model-capacity">${capacity ? "1M model capacity" : "Model capacity not reported"}</span></div>
        <p class="compact-description" id="compact-help">Summarize earlier messages at this threshold. The model keeps its full context capacity.</p>
        <fieldset class="compact-presets" aria-label="Auto-compact threshold" ${blocker ? "disabled" : ""}>${policy.presets.map(([value, label]) => `<label class="compact-preset"><input type="radio" name="compact-window" value="${value}" ${value === draft || (value === "custom" && custom) ? "checked" : ""} aria-describedby="compact-help"><span>${label}</span></label>`).join("")}</fieldset>
        <div class="compact-custom" ${custom ? "" : "hidden"}><label for="compact-tokens">Token threshold</label><div class="compact-input"><input id="compact-tokens" type="text" inputmode="numeric" autocomplete="off" value="${custom ? K.escape(draft) : "272000"}" aria-describedby="compact-error compact-range" aria-invalid="${!validation.valid}" ${blocker ? "disabled" : ""}><span>tokens</span></div><p class="help" id="compact-range">100,000–${(capacity || policy.maximum).toLocaleString("en-US")} tokens</p></div>
        <p class="compact-error" id="compact-error" role="alert" ${validation.valid ? "hidden" : ""}>${validation.error || ""}</p>
        ${blocker ? `<p class="compact-notice" role="status">${K.escape(blocker)}</p>` : ""}
        <p class="compact-feedback" role="alert" ${["error", "conflict"].includes(state) ? "" : "hidden"}>${state === "conflict" ? "Settings changed on another device. Your selection is kept; refresh and retry." : "Settings could not be applied. Your selection is kept; retry when ready."}</p>
        <p class="compact-scope">This conversation · applies from the next turn</p>
    </section>`;
};
K.validateCompactDraft = () => {
    const provider = K.query.get("picker") || K.current.provider;
    const selected = document.querySelector('input[name="compact-window"]:checked')?.value || "auto";
    K.compactDraft = selected === "custom" ? document.querySelector("#compact-tokens").value.trim() : selected;
    const model = document.querySelector('input[name="model"]:checked')?.value;
    const result = K.compactWindow.validate(K.compactDraft, K.compactWindow.capacity(model));
    const error = document.querySelector("#compact-error");
    if (error) { error.textContent = result.error || ""; error.hidden = result.valid; }
    document.querySelector("#compact-tokens")?.setAttribute("aria-invalid", String(!result.valid));
    const apply = document.querySelector('[data-action="apply-provider"]');
    if (apply) apply.disabled = K.compactWindow.modelBlocked() || !result.valid;
    return result;
};
document.addEventListener("change", event => {
    if (event.target.name === "compact-window") {
        const custom = event.target.value === "custom";
        document.querySelector(".compact-custom").hidden = !custom;
        if (custom) document.querySelector("#compact-tokens").focus();
        K.validateCompactDraft();
    }
    if (event.target.name === "model") {
        const capacity = K.compactWindow.capacity(event.target.value);
        document.querySelector("#model-capacity").textContent = capacity ? "1M model capacity" : "Model capacity not reported";
        K.validateCompactDraft();
    }
});
document.addEventListener("input", event => {
    if (event.target.id === "compact-tokens") K.validateCompactDraft();
    if (event.target.id === "model-search") {
        const empty = document.querySelector("#model-search-empty");
        if (empty) empty.hidden = [...document.querySelectorAll(".model-option")].some(row => row.textContent.toLowerCase().includes(event.target.value.toLowerCase()));
    }
});
document.addEventListener("DOMContentLoaded", () => {
    if (document.querySelector(".compact-control")) K.validateCompactDraft();
});
