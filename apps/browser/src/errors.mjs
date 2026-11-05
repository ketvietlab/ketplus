import { redactText } from "./redaction.mjs";

export class BrowserApiError extends Error {
  constructor(code, message, data = undefined) {
    super(message);
    this.name = "BrowserApiError";
    this.code = code;
    this.data = data;
  }
}

export function assertApi(condition, code, message, data = undefined) {
  if (!condition) throw new BrowserApiError(code, message, data);
}

// Engine errors carry call logs and full URLs. Only a redacted first line may
// cross the protocol boundary.
export function toApiError(error, fallbackCode = "ENGINE_ERROR") {
  if (error instanceof BrowserApiError) return error;
  const firstLine = String(error?.message ?? error).split("\n")[0];
  const code = error?.name === "TimeoutError" ? "TIMEOUT" : fallbackCode;
  return new BrowserApiError(code, redactText(firstLine, 300));
}
