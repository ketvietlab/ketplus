const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { JSDOM, VirtualConsole } = require("jsdom");
const root = path.resolve(__dirname, "../../ketplus-desktop-redesign.ketatlas");
const manifest = JSON.parse(fs.readFileSync(path.join(root, "atlas.json"), "utf8"));
const openedWindows = new Set();
test.afterEach(() => {
    for (const window of openedWindows) window.close();
    openedWindows.clear();
});
function screen(relative = "screens/workspace.html", options = {}) {
    const address = new URL(relative.replace(/^\.\//, ""), "https://atlas.invalid/");
    const file = address.pathname.slice(1);
    const html = fs.readFileSync(path.join(root, file), "utf8");
    const errors = [];
    const virtualConsole = new VirtualConsole();
    virtualConsole.on("jsdomError", (error) => errors.push(error));
    const dom = new JSDOM(html, {
        url: address.href,
        runScripts: "outside-only",
        virtualConsole,
    });
    const w = dom.window;
    openedWindows.add(w);
    let time = 10000;
    const timers = [];
    w.Date.now = () => time;
    w.setTimeout = (fn, delay = 0) => {
        timers.push({ fn, due: time + delay });
        return timers.length;
    };
    w.clearTimeout = () => {};
    let systemChange;
    const media = { matches: options.systemDark || false, addEventListener(_type, fn) { systemChange = fn; } };
    w.matchMedia = () => media;
    const parentMessages = [];
    if (options.embedded) Object.defineProperty(w, "parent", { value: {
        postMessage(message, origin) { parentMessages.push({ message, origin }); },
    } });
    w.document.fonts = { ready: Promise.resolve() };
    w.HTMLElement.prototype.scrollIntoView = function () {};
    // jsdom does not implement native dialog methods; only emulate open/close
    // state here. Browser checks own focus, top-layer rendering and backdrop QA.
    w.HTMLDialogElement.prototype.showModal = function () { this.open = true; };
    w.HTMLDialogElement.prototype.close = function () {
        this.open = false;
        this.dispatchEvent(new w.Event("close"));
    };
    const scripts = [...html.matchAll(/<script src="([^"]+)"/g)].map((match) =>
        fs.readFileSync(path.resolve(root, path.dirname(file), match[1]), "utf8"),
    );
    w.eval(scripts.join("\n;\n"));
    let destination;
    w.K.go = (page, values = {}) => {
        w.K.captureDraft();
        destination = `screens/${w.K.url(page, values)}`;
    };
    return {
        w,
        K: w.K,
        document: w.document,
        errors,
        parentMessages,
        systemTheme(dark) {
            media.matches = dark;
            systemChange?.();
        },
        click(selector) {
            const node = w.document.querySelector(selector);
            assert.ok(node, selector);
            node.click();
        },
        input(selector, value) {
            const node = w.document.querySelector(selector);
            assert.ok(node, selector);
            node.value = value;
            node.dispatchEvent(new w.Event("input", { bubbles: true }));
        },
        submit(selector) {
            w.document
                .querySelector(selector)
                .dispatchEvent(new w.Event("submit", { bubbles: true, cancelable: true }));
        },
        follow() {
            assert.ok(destination, "navigation expected");
            return screen(destination, options);
        },
        url() {
            return destination;
        },
        tick(ms = 1000) {
            time += ms;
            const due = timers.filter((timer) => timer.due <= time);
            due.forEach((timer) => timers.splice(timers.indexOf(timer), 1));
            due.forEach((timer) => timer.fn());
        },
        close() {
            w.close();
        },
    };
}
module.exports = { screen, root, manifest };
