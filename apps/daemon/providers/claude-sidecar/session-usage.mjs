// Only numeric session accounting and the captured billing mode leave the SDK.
// SDK totals include restored history; they are not the current query's budget spend.
export function sessionUsage(authentication, session, result = false) {
  const snapshot = { mode: authentication?.enabled === true ? "api_key" : "runtime",
    budgetUsd: authentication?.enabled === true && Number.isFinite(authentication.budget_usd)
      ? Math.max(0, authentication.budget_usd) : 0 };
  if (!session || (result && session.is_error)) return snapshot;
  const cost = session.total_cost_usd;
  if (typeof cost === "number" && Number.isFinite(cost) && cost >= 0)
    snapshot.estimatedCostUsd = cost;
  const models = result ? session.modelUsage : session.model_usage;
  const fields = ["inputTokens", "outputTokens", "cacheReadInputTokens", "cacheCreationInputTokens"];
  const rows = models && typeof models === "object" ? Object.values(models) : [];
  if (rows.length && rows.every(row => row && fields.every(field =>
      Number.isSafeInteger(row[field]) && row[field] >= 0))) {
    const total = fields.map(field => rows.reduce((sum, row) => sum + row[field], 0));
    if (total.every(Number.isSafeInteger)) fields.forEach((field, i) => snapshot[field] = total[i]);
  }
  return snapshot;
}
