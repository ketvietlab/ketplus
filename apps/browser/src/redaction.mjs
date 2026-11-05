const SENSITIVE_PARAM =
  /token|key|secret|pass|auth|code|session|sid|sig|jwt|state|nonce|otp|ticket|cred|saml|assertion/i;
const OPAQUE_PATH_SEGMENT = /^[A-Za-z0-9_\-.~%]{32,}$/;
const URL_IN_TEXT = /\b(?:https?|wss?):\/\/[^\s"'<>`]+/gi;

export function safeUrl(value) {
  try {
    const url = new URL(value);
    url.username = "";
    url.password = "";
    url.hash = "";
    for (const key of new Set(url.searchParams.keys())) {
      if (SENSITIVE_PARAM.test(key)) url.searchParams.set(key, "redacted");
    }
    if (["http:", "https:", "ws:", "wss:"].includes(url.protocol)) {
      url.pathname = url.pathname
        .split("/")
        .map((segment) => (OPAQUE_PATH_SEGMENT.test(segment) ? "redacted" : segment))
        .join("/");
    }
    return url.toString();
  } catch {
    return "[invalid URL]";
  }
}

export function redactText(text, maxLength = 500) {
  return String(text ?? "").replace(URL_IN_TEXT, (match) => safeUrl(match)).slice(0, maxLength);
}
