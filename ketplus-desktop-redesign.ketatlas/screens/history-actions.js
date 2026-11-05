const scroll = document.querySelector(".chat-scroll");
if (scroll) {
    document.fonts.ready.then(() => {
        scroll.scrollTop =
            K.current.reading.follow && K.current.reading.page !== "new-content"
                ? scroll.scrollHeight
                : K.current.reading.top;
        if (K.current.reading.page === "new-content") K.current.reading.follow = false;
    });
    let readerMoved = false;
    scroll.addEventListener(
        "wheel",
        () => {
            readerMoved = true;
        },
        { passive: true },
    );
    scroll.addEventListener("keydown", () => {
        readerMoved = true;
    });
    scroll.addEventListener("scroll", () => {
        K.current.reading.top = scroll.scrollTop;
        K.current.reading.follow =
            scroll.scrollHeight - scroll.clientHeight - scroll.scrollTop < 40;
        const latest = document.querySelector(".latest-bar");
        if (latest) latest.hidden = K.current.reading.follow;
        if (
            readerMoved &&
            scroll.scrollTop < 20 &&
            ["latest", "older"].includes(K.current.reading.page) &&
            K.current.reading.older < 9
        )
            handlers["load-older"]();
    });
}
Object.assign(handlers, {
    "jump-latest": () => {
        if (!scroll) return;
        K.current.reading.follow = true;
        K.current.reading.page = "latest";
        scroll.scrollTop = scroll.scrollHeight;
        document.querySelector(".latest-bar").hidden = true;
    },
    "load-older": () => {
        if (!scroll || K.current.reading.page === "loading") return;
        const oldHeight = scroll.scrollHeight;
        const controls = document.querySelector(".history-controls");
        K.current.reading.page = "loading";
        controls.outerHTML = K.historyControls();
        setTimeout(() => {
            if (K.query.get("outcome") === "error") K.current.reading.page = "page-error";
            else {
                const container = document.querySelector("#history-items");
                const count = K.current.reading.older || 0;
                if (count < 9) {
                    container.insertAdjacentHTML(
                        "afterbegin",
                        Array.from({ length: 3 }, (_, i) => K.historyItem(count + 2 - i)).join(""),
                    );
                    K.current.reading.older = count + 3;
                }
                K.current.reading.page = "older";
            }
            document.querySelector(".history-controls").outerHTML = K.historyControls();
            scroll.scrollTop += scroll.scrollHeight - oldHeight;
            K.current.reading.follow = false;
            document.querySelector(".latest-bar").hidden = false;
        }, 600);
    },
    "continue-page": () => {
        K.current.reading.page = "older";
        document.querySelector(".history-controls").outerHTML = K.historyControls();
        K.toast("Remaining items loaded. Live cursor unchanged.");
    },
    "history-reconnect": () => {
        K.current.reading.page = K.query.get("outcome") === "error" ? "retention" : "partial";
        document.querySelector(".history-controls").outerHTML = K.historyControls();
    },
    "rich-open": () => {
        const preview = document.querySelector(".rich-preview");
        preview.innerHTML = '<p role="status">Loading preview…</p>';
        setTimeout(() => {
            preview.innerHTML =
                K.query.get("outcome") === "error"
                    ? K.notice(
                          "Preview unavailable",
                          "The message is preserved.",
                          "rich-retry",
                          "Retry preview",
                          "error",
                      )
                    : "<h3>Design reference</h3><p>Bounded sample preview. Production code/diff/media renderers load details separately.</p><table><tr><th>State</th><th>Behavior</th></tr><tr><td>Reading history</td><td>Keep anchor</td></tr><tr><td>Latest</td><td>Follow live tail</td></tr></table>";
        }, 600);
    },
    "rich-retry": () => {
        K.query.delete("outcome");
        handlers["rich-open"]();
    },
});
