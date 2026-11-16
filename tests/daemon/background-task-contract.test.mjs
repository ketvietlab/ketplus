// This checks the design fixture, not executor/provider implementation behavior.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const fixture = JSON.parse(readFileSync(new URL("./fixtures/background-tasks-v1.json", import.meta.url)));
const nonterminal = new Set(fixture.states.nonterminal);
const terminal = new Set(fixture.states.terminal);

test("API, identity and admission constraints preserve the shared contract", () => {
  for (const action of ["start", "list", "get", "stop", "output"])
    assert.equal(fixture.api[`background_task_${action}`], `session.task.${action}`);
  assert.deepEqual(fixture.kinds, ["process", "agent"]);
  assert.equal(fixture.authority, "durable_daemon_registry");
  assert.deepEqual(fixture.start.unique_key, ["owner_session_id", "owner_generation", "client_request_id"]);
  assert.equal(fixture.start.accepted_requires, "intent_committed");
  assert.deepEqual(fixture.identity_requires, ["process_birth_identity", "executor_lease_identity"]);
  assert.deepEqual(fixture.limits, {
    ui_projection_rows: 16, output_page_bytes: 8192, output_spool_bytes: 10 * 1024 * 1024,
    terminal_retention_days: 7, active_per_session: 4, active_global: 16,
    queued_global: 100, admission_error: "resource_exhausted",
  });
});

test("transition vectors never leave a terminal or skip reclamation for cancellation", () => {
  assert.equal(new Set([...nonterminal, ...terminal]).size, nonterminal.size + terminal.size);
  for (const transition of fixture.transitions) {
    assert.ok(nonterminal.has(transition.to) || terminal.has(transition.to));
    for (const from of transition.from) assert.ok(nonterminal.has(from), `invalid source ${from}`);
    if (terminal.has(transition.to))
      assert.ok(transition.requires.includes("terminal_and_outbox_transaction"));
    if (["cancelled", "timed_out"].includes(transition.to)) {
      assert.deepEqual(transition.from, ["stopping"]);
      assert.ok(transition.requires.includes("executor_and_descendants_reclaimed"));
    }
  }
  for (const state of [...nonterminal, ...terminal])
    assert.ok(fixture.transitions.some((t) => t.to === state || t.from.includes(state)), state);
  assert.deepEqual(fixture.transition_invariants,
    ["revision_cas", "terminal_immutable", "stop_idempotent", "one_terminal_winner"]);
});

test("completion vectors preserve durable acceptance and user fences", () => {
  assert.deepEqual(fixture.completion.event_identity, ["task_id", "terminal_revision"]);
  assert.deepEqual(fixture.completion.atomic_commit, ["terminal_record", "outbox_event"]);
  assert.deepEqual(fixture.completion.atomic_accept, ["event_id_dedup", "accept_or_enqueue_owner_turn"]);
  assert.equal(fixture.completion.delivery, "at_least_once");
  for (const flag of ["progress_wakes", "exactly_once_inference", "replay_interrupted_turn"])
    assert.equal(fixture.completion[flag], false);
  const policy = new Map(fixture.wake_policy.map(({ condition, action }) => [condition, action]));
  assert.equal(policy.get("busy"), "enqueue_after_current_turn");
  for (const condition of ["permission_pending", "user_decision_pending"])
    assert.equal(policy.get(condition), "keep_pending");
  for (const condition of ["user_stopped", "paused"])
    assert.equal(policy.get(condition), "keep_pending_until_explicit_resume");
  for (const condition of ["owner_invalid", "cancelled_by_stop_or_session_close"])
    assert.equal(policy.get(condition), "suppress_with_reason");
  assert.equal(policy.get("permission_denied"), "suppress_with_permission_denied_reason_no_wake_or_retry");
});

test("pre-launch failure and uncertain cleanup keep ownership until reclamation", () => {
  const failedBeforeRun = fixture.transitions.find((t) => t.to === "failed" && t.from.includes("queued"));
  assert.deepEqual(failedBeforeRun.from, ["queued", "starting"]);
  assert.ok(failedBeforeRun.requires.includes("never_launched_or_executor_and_descendants_reclaimed"));
  const failedAfterCleanup = fixture.transitions.find((t) => t.to === "failed" && t.from.includes("stopping"));
  assert.ok(failedAfterCleanup.requires.includes("executor_and_descendants_reclaimed"));
  assert.ok(failedAfterCleanup.requires.includes("terminal_target_failed"));
  const uncertain = fixture.transitions.find((t) => t.requires.includes("cleanup_pending"));
  assert.equal(uncertain.to, "stopping");
  assert.ok(uncertain.requires.includes("continue_reconciliation_and_warning"));
  for (const t of fixture.transitions.filter((t) => t.to === "interrupted"))
    assert.ok(t.requires.includes("confirmed_no_live_workload_in_containment"));
});

test("response projections preserve the leader-defined UI and output schema", () => {
  const schema = fixture.response_schema;
  assert.equal(schema.state_field, "state");
  assert.deepEqual(schema.background_tasks_compatibility_alias, { status: "state" });
  assert.deepEqual(schema.list_fields, ["tasks", "next_cursor", "revision", "active_count"]);
  assert.deepEqual(schema.output_fields, ["output", "next_cursor", "total_bytes", "truncated"]);
  assert.equal(schema.completion_nonterminal, null);
  assert.deepEqual(schema.completion_terminal_fields, ["event_id", "status", "reason"]);
  assert.equal(schema.delivered_means, "wake_durably_accepted_or_enqueued");
  assert.equal(schema.stop_output_authorization, "server_owner_or_grant_check_not_provider_name");
  const examples = fixture.projection_examples;
  assert.deepEqual(Object.keys(examples.list).sort(), [...schema.list_fields].sort());
  assert.deepEqual(Object.keys(examples.output).sort(), [...schema.output_fields].sort());
  assert.equal(examples.list.tasks[0].completion, null);
  assert.equal(examples.output.total_bytes, Buffer.byteLength(examples.output.output));
  assert.equal(examples.background_tasks[0].status, examples.list.tasks[0].state);
  for (const name of ["terminal_pending", "terminal_delivered", "permission_denied"])
    assert.deepEqual(Object.keys(examples[name].completion).sort(), [...schema.completion_terminal_fields].sort());
  assert.equal(examples.terminal_pending.completion.event_id, examples.terminal_delivered.completion.event_id);
  assert.equal(examples.permission_denied.completion.status, "suppressed");
  assert.equal(examples.permission_denied.completion.reason, "permission_denied");
});

test("fault matrix retains every mandatory implementation verification gate", () => {
  const ids = new Set();
  for (const scenario of fixture.scenarios) {
    assert.ok(!ids.has(scenario.id), `duplicate scenario ${scenario.id}`);
    ids.add(scenario.id);
    for (const key of ["given", "when", "boundary"]) assert.ok(scenario[key]?.length, `${scenario.id}.${key}`);
    assert.ok(scenario.expect.length > 0, scenario.id);
  }
  for (const id of [
    "retry-start", "retry-conflict", "start-commit-failure", "permission-fence", "preaccept-refusal",
    "permission-denied", "launch-failure", "launch-failure-cleanup", "cleanup-uncertain", "response-schema",
    "crash-before-spawn", "crash-after-spawn", "pid-reuse", "descendants", "lease-loss",
    "unsupported-containment", "timeout", "stop-complete-race",
    "restart-before-terminal-commit", "restart-after-terminal-commit",
    "restart-before-accept", "restart-after-accept", "owner-busy", "owner-permission",
    "owner-decision", "owner-stop-pause", "old-generation", "session-close", "provider-exit",
    "cwd-change", "quota-queue", "output-overflow", "more-than-16", "event-gap", "retention",
    "progress-no-wake", "cancel-no-wake", "claude-native-block", "claude-legacy-drain",
    "owner-authentication", "shared-provider-api", "turn-stop-vs-job-stop", "offline-owner-ui",
    "untrusted-output",
  ]) assert.ok(ids.has(id), `missing implementation gate ${id}`);
});
