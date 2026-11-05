import { BrowserApiError } from "./errors.mjs";

const INTERESTING_SELECTOR = [
  "a",
  "area",
  "button",
  "input",
  "select",
  "option",
  "textarea",
  "[role]",
  "[contenteditable='true']",
  "[tabindex]",
  "h1",
  "h2",
  "h3",
  "h4",
  "h5",
  "h6",
  "p",
  "label",
  "ul",
  "ol",
  "li",
  "img",
  "nav",
  "main",
  "aside",
  "header",
  "footer",
  "section",
  "article",
  "dialog",
  "details",
  "summary",
  "form",
  "table",
].join(",");

const SECRET_INPUTS = [
  "input[type='password' i]",
  "input[autocomplete~='current-password' i]",
  "input[autocomplete~='new-password' i]",
  "input[autocomplete~='one-time-code' i]",
  "input[autocomplete^='cc-' i]",
].join(",");

// Runs in the page. It must stay self-contained because Playwright serializes
// the function source. The accessible-name order follows the broad shape of
// accname 1.2 (labelledby, aria-label, native label, content, title,
// placeholder); it is still a heuristic, not a browser accessibility tree.
export function descriptorScript(element) {
  const normalize = (value) => String(value ?? "").replace(/\s+/g, " ").trim();
  if (!element.isConnected) return null;
  if (element.closest("[aria-hidden='true'], [inert]")) return null;
  const rect = element.getBoundingClientRect();
  const style = getComputedStyle(element);
  if (rect.width <= 0 || rect.height <= 0 || style.display === "none" || style.visibility !== "visible") {
    return null;
  }
  if (
    typeof element.checkVisibility === "function" &&
    !element.checkVisibility({ opacityProperty: true, visibilityProperty: true, contentVisibilityAuto: true })
  ) {
    return null;
  }

  const tag = element.tagName.toLowerCase();
  const inputType = tag === "input" ? (element.getAttribute("type") || "text").toLowerCase() : "";
  if (inputType === "hidden") return null;
  const insideSectioning = () => Boolean(element.parentElement?.closest("article, aside, main, nav, section"));
  const hasAuthorName = () =>
    element.hasAttribute("aria-label") || element.hasAttribute("aria-labelledby") || element.hasAttribute("title");

  const implicitRole = (() => {
    switch (tag) {
      case "a":
      case "area":
        return element.hasAttribute("href") ? "link" : "generic";
      case "button":
        return "button";
      case "textarea":
        return "textbox";
      case "select":
        return element.multiple || element.size > 1 ? "listbox" : "combobox";
      case "option":
        return "option";
      case "img":
        return element.getAttribute("alt") === "" ? "presentation" : "img";
      case "h1":
      case "h2":
      case "h3":
      case "h4":
      case "h5":
      case "h6":
        return "heading";
      case "nav":
        return "navigation";
      case "main":
        return "main";
      case "aside":
        return "complementary";
      case "header":
        return insideSectioning() ? "generic" : "banner";
      case "footer":
        return insideSectioning() ? "generic" : "contentinfo";
      case "form":
        return hasAuthorName() ? "form" : "generic";
      case "section":
        return hasAuthorName() ? "region" : "generic";
      case "article":
        return "article";
      case "dialog":
        return "dialog";
      case "details":
        return "group";
      case "summary":
        // Chromium exposes a details summary as its disclosure button.
        return element.parentElement?.tagName === "DETAILS" ? "button" : "generic";
      case "table":
        return "table";
      case "ul":
      case "ol":
        return "list";
      case "li":
        return "listitem";
      case "p":
        return "paragraph";
      case "input":
        switch (inputType) {
          case "checkbox":
            return "checkbox";
          case "radio":
            return "radio";
          case "range":
            return "slider";
          case "number":
            return "spinbutton";
          case "search":
            return element.hasAttribute("list") ? "combobox" : "searchbox";
          case "button":
          case "submit":
          case "reset":
          case "image":
            return "button";
          default:
            return element.hasAttribute("list") ? "combobox" : "textbox";
        }
      default:
        return "generic";
    }
  })();

  const focusable =
    element.tabIndex >= 0 || element.matches("a[href], button, input, select, textarea, summary, [contenteditable='true']");
  let role = normalize(element.getAttribute("role")).split(" ").filter(Boolean)[0] || implicitRole;
  if (role === "none" || role === "presentation") {
    if (!focusable) return null;
    role = implicitRole === "presentation" ? "img" : implicitRole;
  }

  const rootNode = element.getRootNode();
  const lookup = (id) =>
    (typeof rootNode.getElementById === "function" ? rootNode.getElementById(id) : null) ||
    document.getElementById(id);
  const textOf = (node) => normalize(node.innerText ?? node.textContent);
  const nameFromContent = new Set([
    "button", "cell", "checkbox", "columnheader", "gridcell", "heading", "link", "menuitem",
    "menuitemcheckbox", "menuitemradio", "option", "radio", "row", "rowheader", "switch", "tab",
    "tooltip", "treeitem",
  ]);
  const nameSources = [
    () => normalize(
      normalize(element.getAttribute("aria-labelledby"))
        .split(" ")
        .filter(Boolean)
        .map((id) => {
          const node = lookup(id);
          return node ? textOf(node) : "";
        })
        .join(" "),
    ),
    () => normalize(element.getAttribute("aria-label")),
    () => {
      if (["input", "select", "textarea", "meter", "progress", "output"].includes(tag) && element.labels?.length) {
        return normalize([...element.labels].map(textOf).join(" "));
      }
      if (tag === "input" && ["button", "submit", "reset"].includes(inputType)) {
        return normalize(element.getAttribute("value") || { submit: "Submit", reset: "Reset" }[inputType] || "");
      }
      if (tag === "img" || tag === "area" || inputType === "image") return normalize(element.getAttribute("alt"));
      if (tag === "table" && element.caption) return textOf(element.caption);
      return "";
    },
    () => (nameFromContent.has(role) ? textOf(element) : ""),
    () => normalize(element.getAttribute("title")),
    () =>
      ["textbox", "searchbox", "combobox", "spinbutton"].includes(role)
        ? normalize(element.getAttribute("placeholder") || element.getAttribute("aria-placeholder"))
        : "",
  ];
  let name = "";
  for (const source of nameSources) {
    name = source();
    if (name) break;
  }

  const states = [];
  const aria = (attribute) => element.getAttribute(`aria-${attribute}`);
  states.push(element.matches(":disabled") || aria("disabled") === "true" ? "disabled" : "enabled");
  if (element.matches(":checked") || aria("checked") === "true") states.push("checked");
  if (element.indeterminate || aria("checked") === "mixed") states.push("mixed");
  const disclosure = tag === "summary" && element.parentElement?.tagName === "DETAILS" ? element.parentElement : null;
  if (aria("expanded") === "true" || disclosure?.open === true) states.push("expanded");
  if (aria("expanded") === "false" || disclosure?.open === false) states.push("collapsed");
  if (aria("selected") === "true") states.push("selected");
  if (aria("pressed") === "true") states.push("pressed");
  if (aria("current") && aria("current") !== "false") states.push("current");
  if (element.matches(":required") || aria("required") === "true") states.push("required");
  const isTextEntry =
    tag === "textarea" ||
    (tag === "input" &&
      !["button", "checkbox", "color", "file", "image", "radio", "range", "reset", "submit"].includes(inputType));
  if ((isTextEntry && element.readOnly) || aria("readonly") === "true") states.push("readonly");
  if (aria("invalid") && aria("invalid") !== "false") states.push("invalid");
  if (element.getRootNode().activeElement === element) states.push("focused");

  const isField = tag === "input" || tag === "textarea" || tag === "select";
  const round = (number) => Math.round(number * 100) / 100;
  return {
    stableId:
      element.getAttribute("data-canonical-id") ||
      element.getAttribute("data-testid") ||
      element.id ||
      null,
    tag,
    role,
    name: name.slice(0, 200),
    ...(role === "heading" ? { level: Number(aria("level")) || Number(tag.slice(1)) || null } : {}),
    // Field text is never read: it would expose typed values.
    text: isField ? "" : normalize(element.innerText).slice(0, 300),
    states,
    bounds: {
      x: round(rect.x),
      y: round(rect.y),
      width: round(rect.width),
      height: round(rect.height),
    },
    clipped:
      rect.left < 0 ||
      rect.top < 0 ||
      rect.right > window.innerWidth ||
      rect.bottom > window.innerHeight,
    styles: {
      display: style.display,
      position: style.position,
      fontFamily: style.fontFamily,
      fontSize: style.fontSize,
      fontWeight: style.fontWeight,
      lineHeight: style.lineHeight,
      letterSpacing: style.letterSpacing,
      color: style.color,
      backgroundColor: style.backgroundColor,
      borderTopWidth: style.borderTopWidth,
      borderTopColor: style.borderTopColor,
      borderRadius: style.borderRadius,
      opacity: style.opacity,
      overflowX: style.overflowX,
      overflowY: style.overflowY,
    },
  };
}

// Playwright serializes page functions by source, so the descriptor is
// embedded into the collector's source here in Node. Nothing is evaluated from
// strings inside the page, which keeps this working under strict page CSP.
const COLLECT_NODES = new Function(
  "root",
  "options",
  `const describe = ${descriptorScript.toString()};
  const elements = [];
  const descriptors = [];
  let visibleNodeCount = 0;
  // visibleOnly: the part of the viewport each element may paint into, after the clip of every
  // overflow ancestor between it and its containing blocks. Fixed elements escape their
  // ancestors' clips, and absolutely positioned ones escape those of non-positioned ancestors.
  const clips = new Map();
  const intersect = (a, b) => ({
    left: Math.max(a.left, b.left),
    top: Math.max(a.top, b.top),
    right: Math.min(a.right, b.right),
    bottom: Math.min(a.bottom, b.bottom),
  });
  const viewport = { left: 0, top: 0, right: window.innerWidth, bottom: window.innerHeight };
  const styles = new Map();
  const styleOf = (element) => {
    let style = styles.get(element);
    if (!style) {
      const computed = getComputedStyle(element);
      style = { position: computed.position, overflowX: computed.overflowX, overflowY: computed.overflowY };
      styles.set(element, style);
    }
    return style;
  };
  // Overflow on <html>, or on <body> while <html> has none, applies to the viewport instead.
  const html = document.documentElement;
  const htmlOverflows = styleOf(html).overflowX !== "visible" || styleOf(html).overflowY !== "visible";
  const clipsContent = (element) =>
    element !== html && (element !== document.body || htmlOverflows);
  const clipFor = (element) => {
    let area = viewport;
    let skipStatic = false;
    for (let current = element; current; current = current.parentElement ?? current.getRootNode()?.host ?? null) {
      const style = styleOf(current);
      if (current !== element) {
        const positioned = style.position !== "static";
        if (!skipStatic || positioned) {
          if ((style.overflowX !== "visible" || style.overflowY !== "visible") && clipsContent(current)) {
            let rect = clips.get(current);
            if (!rect) {
              rect = current.getBoundingClientRect();
              clips.set(current, rect);
            }
            area = intersect(area, {
              left: style.overflowX === "visible" ? -Infinity : rect.left,
              right: style.overflowX === "visible" ? Infinity : rect.right,
              top: style.overflowY === "visible" ? -Infinity : rect.top,
              bottom: style.overflowY === "visible" ? Infinity : rect.bottom,
            });
          }
          if (positioned) skipStatic = false;
        }
      }
      if (style.position === "fixed") break;
      if (style.position === "absolute") skipStatic = true;
    }
    return area;
  };
  const inView = (element, bounds) => {
    const area = clipFor(element);
    const visible = intersect(area, {
      left: bounds.x,
      top: bounds.y,
      right: bounds.x + bounds.width,
      bottom: bounds.y + bounds.height,
    });
    return visible.right > visible.left && visible.bottom > visible.top;
  };
  // Open shadow roots are included, matching the engine's CSS locator.
  const visit = (scope) => {
    for (const element of scope.querySelectorAll("*")) {
      if (element.matches(options.selector)) {
        const descriptor = describe(element);
        if (descriptor && (!options.visibleOnly || inView(element, descriptor.bounds))) {
          visibleNodeCount += 1;
          if (elements.length < options.limit) {
            elements.push(element);
            descriptors.push(descriptor);
          }
        }
      }
      if (element.shadowRoot) visit(element.shadowRoot);
    }
  };
  visit(root);
  return { elements, descriptors, visibleNodeCount };`,
);

const escapeRegExp = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

async function collectSecretValues(page) {
  const values = await Promise.all(
    page.frames().map((frame) =>
      frame.locator(SECRET_INPUTS).evaluateAll((elements) => elements.map((element) => element.value)).catch(() => []),
    ),
  );
  return [...new Set(values.flat().filter((value) => typeof value === "string" && value.length > 0))]
    .sort((left, right) => right.length - left.length);
}

// The engine ARIA snapshot renders textbox values, including password fields.
export function redactSecrets(text, secrets) {
  let redacted = text;
  for (const secret of secrets) {
    if (secret.length >= 4) {
      redacted = redacted.split(secret).join("[redacted]");
    } else {
      redacted = redacted.replace(
        new RegExp(`(:\\s*)"?${escapeRegExp(secret)}"?\\s*$`, "gm"),
        "$1[redacted]",
      );
    }
  }
  return redacted;
}

export async function assertValidSelector(page, selector, name) {
  const valid = await page.evaluate((value) => {
    try {
      document.createDocumentFragment().querySelector(value);
      return true;
    } catch {
      return false;
    }
  }, selector);
  if (!valid) throw new BrowserApiError("INVALID_ARGUMENT", `${name} is not a valid CSS selector`);
}

export async function observePage(page, snapshotId, { maxNodes, maxAriaChars, rootSelector, visibleOnly = false } = {}) {
  if (rootSelector !== undefined) await assertValidSelector(page, rootSelector, "rootSelector");
  const root = page.locator(`css=${rootSelector ?? "body"}`).first();
  if ((await root.count()) === 0) {
    throw new BrowserApiError("OBSERVATION_ROOT_NOT_FOUND", "Observation root was not found", {
      rootSelector,
    });
  }
  const rootBounds = await root.boundingBox();
  // Matching, visibility filtering and the node limit run in one page call, so
  // the element list is consistent and only kept nodes receive handles.
  const collectionHandle = await root.evaluateHandle(COLLECT_NODES, {
    selector: INTERESTING_SELECTOR,
    limit: maxNodes,
    visibleOnly,
  });
  const { descriptors, visibleNodeCount } = await collectionHandle.evaluate((collection) => ({
    descriptors: collection.descriptors,
    visibleNodeCount: collection.visibleNodeCount,
  }));
  const elementsHandle = await collectionHandle.getProperty("elements");
  const elementHandles = [...(await elementsHandle.getProperties()).entries()]
    .filter(([key]) => /^\d+$/.test(key))
    .sort(([left], [right]) => Number(left) - Number(right))
    .map(([, handle]) => handle);
  await Promise.all([collectionHandle.dispose(), elementsHandle.dispose()]);

  const nodes = [];
  const handles = new Map();
  const fingerprints = new Map();
  descriptors.forEach((descriptor, index) => {
    if (rootBounds) {
      descriptor.bounds.x = Math.round((descriptor.bounds.x - rootBounds.x) * 100) / 100;
      descriptor.bounds.y = Math.round((descriptor.bounds.y - rootBounds.y) * 100) / 100;
    }
    const ref = `${snapshotId}:n${index + 1}`;
    descriptor.ref = ref;
    nodes.push(descriptor);
    handles.set(ref, elementHandles[index].asElement() ?? elementHandles[index]);
    fingerprints.set(ref, { tag: descriptor.tag, role: descriptor.role, name: descriptor.name });
  });

  const secrets = await collectSecretValues(page);
  const rawAria = await root.ariaSnapshot().catch(() => null);
  const ariaSnapshotFull = rawAria === null ? null : redactSecrets(rawAria, secrets);
  return {
    nodes,
    handles,
    fingerprints,
    ariaSnapshotFull,
    ariaSnapshot: ariaSnapshotFull === null ? null : ariaSnapshotFull.slice(0, maxAriaChars),
    ariaSnapshotTruncated: (ariaSnapshotFull?.length ?? 0) > maxAriaChars,
    truncated: visibleNodeCount > nodes.length,
    visibleNodeCount,
    subframeCount: Math.max(0, page.frames().length - 1),
  };
}

export async function disposeHandles(handles) {
  await Promise.all([...handles.values()].map((handle) => handle.dispose().catch(() => {})));
}
