import { redactText } from "./redaction.mjs";
import { LIMITS } from "./protocol.mjs";

// browser.inspect reads named fields of elements the observation does not carry: attributes
// (data-*, aria-busy), scroll position, form values and chosen computed styles. It is a fixed
// read-only function, not arbitrary script, and it can address <html> and other elements outside
// an observation root.

const SECRET_INPUTS = [
  "input[type='password' i]",
  "input[autocomplete~='current-password' i]",
  "input[autocomplete~='new-password' i]",
  "input[autocomplete~='one-time-code' i]",
  "input[autocomplete^='cc-' i]",
].join(",");

// Runs in the page; Playwright serializes it by source, so it stays self-contained.
function readElements(elements, options) {
  const round = (number) => Math.round(number * 100) / 100;
  const clip = (value, length) => {
    const text = String(value ?? "");
    return text.length > length ? `${text.slice(0, length)}…` : text;
  };
  const kebab = (name) => (name.startsWith("--") ? name : name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`));
  return elements.slice(0, options.maxElements).map((element, index) => {
    const secret = element.matches(options.secretSelector);
    const tag = element.tagName.toLowerCase();
    const result = { index, tag, ...(element.id ? { id: element.id } : {}) };
    for (const field of options.fields) {
      switch (field) {
        case "attributes": {
          const attributes = {};
          for (const attribute of [...element.attributes].slice(0, options.maxAttributes)) {
            if (secret && attribute.name === "value") continue;
            attributes[attribute.name] = clip(attribute.value, options.maxAttributeChars);
          }
          result.attributes = attributes;
          if (element.attributes.length > options.maxAttributes) result.attributesTruncated = true;
          break;
        }
        case "scroll": {
          // The document's scroll position belongs to <html> (the scrolling element).
          const scroller = element === document.documentElement ? document.scrollingElement ?? element : element;
          result.scroll = {
            top: round(scroller.scrollTop),
            left: round(scroller.scrollLeft),
            width: scroller.scrollWidth,
            height: scroller.scrollHeight,
            clientWidth: scroller.clientWidth,
            clientHeight: scroller.clientHeight,
          };
          break;
        }
        case "value": {
          if (!["input", "textarea", "select"].includes(tag)) {
            result.value = null;
          } else if (secret) {
            result.value = "[redacted]";
          } else {
            result.value = clip(element.value, options.maxValueChars);
            if (tag === "input" && ["checkbox", "radio"].includes(element.type)) result.checked = element.checked;
            if (tag === "select" && element.multiple) {
              result.values = [...element.selectedOptions].map((option) => clip(option.value, options.maxValueChars));
            }
          }
          break;
        }
        case "bounds": {
          const rect = element.getBoundingClientRect();
          result.bounds = { x: round(rect.x), y: round(rect.y), width: round(rect.width), height: round(rect.height) };
          break;
        }
        case "text":
          result.text = ["input", "textarea", "select"].includes(tag)
            ? ""
            : clip(String(element.innerText ?? element.textContent ?? "").replace(/\s+/g, " ").trim(), options.maxTextChars);
          break;
      }
    }
    if (options.styleProperties.length > 0) {
      const style = getComputedStyle(element);
      result.computedStyle = Object.fromEntries(
        options.styleProperties.map((name) => [name, style.getPropertyValue(kebab(name))]),
      );
    }
    return result;
  });
}

export function inspectionOptions(fields, maxElements) {
  const styleProperties = [];
  const named = [];
  for (const field of fields) {
    if (field.startsWith("computedStyle:")) styleProperties.push(field.slice("computedStyle:".length));
    else if (!named.includes(field)) named.push(field);
  }
  return {
    fields: named,
    styleProperties: [...new Set(styleProperties)],
    maxElements,
    secretSelector: SECRET_INPUTS,
    maxAttributes: LIMITS.inspectMaxAttributes,
    maxAttributeChars: LIMITS.inspectAttributeChars,
    maxValueChars: LIMITS.inspectValueChars,
    maxTextChars: LIMITS.inspectTextChars,
  };
}

// URLs inside attribute values, text and values go through the same redaction as every other
// URL the protocol returns.
function redactElement(element) {
  for (const key of ["attributes", "computedStyle"]) {
    if (!element[key]) continue;
    for (const [name, value] of Object.entries(element[key])) element[key][name] = redactText(value, Infinity);
  }
  if (typeof element.text === "string") element.text = redactText(element.text, Infinity);
  if (typeof element.value === "string") element.value = redactText(element.value, Infinity);
  return element;
}

export async function inspectLocator(locator, options) {
  const matchCount = await locator.count();
  const elements = matchCount === 0 ? [] : await locator.evaluateAll(readElements, options);
  return {
    matchCount,
    elements: elements.map(redactElement),
    truncated: matchCount > elements.length,
  };
}

// One element from a snapshot ref. The reader is embedded by source, as in observation.mjs.
const READ_ONE = new Function(
  "element",
  "options",
  `const read = ${readElements.toString()};
  return read([element], options);`,
);

export async function inspectHandle(handle, options) {
  const elements = await handle.evaluate(READ_ONE, options);
  return { matchCount: elements.length, elements: elements.map(redactElement), truncated: false };
}
