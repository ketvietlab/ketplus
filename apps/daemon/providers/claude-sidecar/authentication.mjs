// The daemon supplies credentials over private stdio, never through session commands/history.

export function redact(value) {
  return JSON.parse(JSON.stringify(value, (_key, entry) => {
    if (typeof entry !== "string") return entry;
    return entry.replace(/sk-ant-[A-Za-z0-9_-]+/g, "[redacted]");
  }));
}

export function authenticationOptions(config = {}, inherited = process.env) {
  if (!config.enabled) return {};
  if (typeof config.key !== "string" || config.key.startsWith("sk-ant-admin01-") || config.key.length > 512 || config.key.trim() !== config.key || !/^sk-ant-[A-Za-z0-9_-]+$/.test(config.key))
    throw new Error("The saved Anthropic API key is invalid. Update Settings → Providers.");
  // Scope to this SDK child. Never change the sidecar's shared process.env.
  const authEnv = {
    ANTHROPIC_API_KEY: config.key,
    // Keep user/project settings from replacing the host's credentials or endpoint.
    // Do not put the key in options.settings: the SDK serializes that into argv.
    CLAUDE_CODE_PROVIDER_MANAGED_BY_HOST: "1",
    ANTHROPIC_BASE_URL: "https://api.anthropic.com",
    ANTHROPIC_AUTH_TOKEN: "",
    CLAUDE_CODE_OAUTH_TOKEN: "",
    ANTHROPIC_CUSTOM_HEADERS: "",
    CLAUDE_CODE_USE_BEDROCK: "0",
    CLAUDE_CODE_USE_VERTEX: "0",
    CLAUDE_CODE_USE_FOUNDRY: "0",
    CLAUDE_CODE_USE_ANTHROPIC_AWS: "0",
    CLAUDE_CODE_USE_MANTLE: "0",
  };
  return {
    env: { ...inherited, ...authEnv },
    settings: { apiKeyHelper: "" },
    ...(config.budget_usd > 0 ? { maxBudgetUsd: config.budget_usd } : {}),
  };
}

export function authenticationChangedWhileBusy(previous, config) {
  return previous?.tasks.hasLive() && previous.authenticationRevision !== (config?.revision || "");
}
