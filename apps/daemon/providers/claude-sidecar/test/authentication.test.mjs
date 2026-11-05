import test from "node:test";
import assert from "node:assert/strict";
import { authenticationOptions, authenticationChangedWhileBusy, redact } from "../authentication.mjs";

test("BYOK is scoped per query, overrides conflicting providers, preserves process environment", () => {
  const inherited = { PATH: "/test/bin", ANTHROPIC_AUTH_TOKEN: "old-token", CLAUDE_CODE_USE_BEDROCK: "1" };
  const config = { enabled: true, key: "sk-ant-test-key-one", budget_usd: 1.25 };
  const options = authenticationOptions(config, inherited);
  assert.equal(options.env.ANTHROPIC_API_KEY, config.key);
  assert.equal(options.env.ANTHROPIC_AUTH_TOKEN, "");
  assert.equal(options.env.CLAUDE_CODE_USE_BEDROCK, "0");
  assert.equal(options.env.PATH, inherited.PATH);
  assert.equal(options.env.ANTHROPIC_BASE_URL, "https://api.anthropic.com");
  assert.equal(options.env.CLAUDE_CODE_PROVIDER_MANAGED_BY_HOST, "1");
  assert.equal(options.env.ANTHROPIC_CUSTOM_HEADERS, "");
  assert.equal(options.settings.apiKeyHelper, "");
  assert.equal(JSON.stringify(options.settings).includes(config.key), false);
  assert.equal(options.maxBudgetUsd, 1.25);
  assert.equal(inherited.ANTHROPIC_AUTH_TOKEN, "old-token");
  const other = authenticationOptions({ ...config, key: "sk-ant-test-key-two" }, inherited);
  assert.equal(options.env.ANTHROPIC_API_KEY, config.key);
  assert.notEqual(other.env.ANTHROPIC_API_KEY, options.env.ANTHROPIC_API_KEY);
});

test("disabled BYOK leaves all runtime authentication methods available", () => {
  assert.deepEqual(authenticationOptions({ enabled: false, key: "saved-but-disabled" }), {});
  assert.deepEqual(authenticationOptions(), {});
  assert.throws(() => authenticationOptions({ enabled: true, key: "bad\nkey" }), /invalid/);
  assert.throws(() => authenticationOptions({ enabled: true, key: "sk-ant-test\n" }), /invalid/);
  assert.equal(authenticationOptions({ enabled: true, key: "sk-ant-test", budget_usd: 0 }).maxBudgetUsd, undefined);
});

test("rotation is fenced while background work is live, including disabling/removing a key", () => {
  const run = { authenticationRevision: "a", tasks: { hasLive: () => true } };
  assert.equal(authenticationChangedWhileBusy(run, { revision: "a" }), false);
  assert.equal(authenticationChangedWhileBusy(run, { revision: "b" }), true);
  assert.equal(authenticationChangedWhileBusy(run, {}), true);
  run.tasks.hasLive = () => false;
  assert.equal(authenticationChangedWhileBusy(run, { revision: "b" }), false);
});

test("provider errors and tool results cannot echo a configured API key into stdout", () => {
  authenticationOptions({ enabled: true, key: "sk-ant-test-sensitive" });
  const result = redact({ error: { message: "Rejected sk-ant-test-sensitive" }, content: ["sk-ant-other-secret"] });
  assert.equal(JSON.stringify(result).includes("sk-ant-"), false);
  assert.equal(result.error.message, "Rejected [redacted]");
});
