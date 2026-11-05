import test from 'node:test';
import assert from 'node:assert/strict';
import {sessionUsage} from '../../../apps/daemon/providers/claude-sidecar/session-usage.mjs';
const auth = {enabled: true, budget_usd: 5, key: 'sk-ant-secret'};
const model = {inputTokens: 20, outputTokens: 3, cacheReadInputTokens: 100, cacheCreationInputTokens: 40};
test('SDK running totals are replaced, include model tree, never divided by query budget', () => {
  const a = sessionUsage(auth, {total_cost_usd: 9, modelUsage: {a: model, b: model}}, true);
  assert.deepEqual(a, {mode:'api_key', budgetUsd:5, estimatedCostUsd:9,
    inputTokens:40, outputTokens:6, cacheReadInputTokens:200, cacheCreationInputTokens:80});
  assert.equal(sessionUsage(auth, {total_cost_usd: 10}, true).estimatedCostUsd, 10);
  assert.equal(sessionUsage(auth, {total_cost_usd: 0, modelUsage:{a:model}}, true).estimatedCostUsd, 0);
});
test('startup/crash errors do not manufacture zero spent; invalid/missing tokens stay unavailable', () => {
  assert.deepEqual(sessionUsage(auth, {total_cost_usd:0, is_error:true}, true), {mode:'api_key',budgetUsd:5});
  assert.deepEqual(sessionUsage(auth, {total_cost_usd:null, modelUsage:{a:{...model,inputTokens:-1}}}, true), {mode:'api_key',budgetUsd:5});
  assert.deepEqual(sessionUsage({}, {total_cost_usd:Infinity}), {mode:'runtime',budgetUsd:0});
  assert.equal(sessionUsage(auth, {total_cost_usd:1, model_usage:{a:model}}).inputTokens, 20);
});
