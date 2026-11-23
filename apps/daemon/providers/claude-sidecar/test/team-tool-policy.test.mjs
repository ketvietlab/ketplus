import assert from "node:assert/strict";
import { test } from "node:test";
import { teamDisallowedTools } from "../background-policy.mjs";

test("Claude team roles exclude only their restricted tools", () => {
  assert.deepEqual(teamDisallowedTools("claude", "developer"), ["ReportFindings", "AskUserQuestion", "ScheduleWakeup"]);
  assert.deepEqual(teamDisallowedTools("claude", "reviewer"),
    ["ReportFindings", "AskUserQuestion", "ScheduleWakeup"]);
  assert.deepEqual(teamDisallowedTools("claude", "leader"), ["ReportFindings", "ScheduleWakeup"]);
  assert.deepEqual(teamDisallowedTools("claude", ""), []);
  assert.deepEqual(teamDisallowedTools("claude", "constructor"), []);
});

test("other providers do not inherit Claude tool names", () => {
  for (const provider of ["codex", "cursor", "copilot"]) {
    for (const role of ["developer", "reviewer", "leader"]) {
      assert.deepEqual(teamDisallowedTools(provider, role), []);
    }
  }
});
