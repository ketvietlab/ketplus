// ADR 0127: native Claude task ownership is drain-only. New work belongs to
// the daemon registry, independently of the lifetime of an SDK query.
const nativeAgentTools = ["Agent", "Task", "Workflow"];
const refusal = "Native Claude background tasks and subagents are disabled. Use ketplus_agents background_task_start with kind process or agent; existing permissions still apply.";

export function backgroundDecision(tool, input) {
  if (nativeAgentTools.includes(tool) || input?.run_in_background)
    return { behavior: "deny", message: refusal };
  return null;
}

export function backgroundOptions(env = process.env, hooks = {}, settings = {}) {
  const disabled = { CLAUDE_CODE_DISABLE_BACKGROUND_TASKS: "1",
    CLAUDE_CODE_DISABLE_MCP_TASK_BACKGROUND: "1" };
  return {
    // SDK 0.3.322's CLI gates explicit/automatic Bash backgrounding and native
    // agent backgrounding with this flag, including control backgroundTasks().
    env: { ...env, ...disabled },
    // Project settings.env can overwrite process.env after CLI startup. The
    // host settings overlay must pin both flags too, without replacing auth.
    settings: { ...settings, env: { ...settings.env, ...disabled } },
    // Agent defaults to background in this SDK. Block the whole native entry,
    // including foreground/resume/remote variants and workflow-spawned agents.
    disallowedTools: [...nativeAgentTools],
    hooks: {
      ...hooks,
      PreToolUse: [{ onFailure: "block", hooks: [async (event) => {
        const decision = backgroundDecision(event.tool_name, event.tool_input);
        return decision ? { hookSpecificOutput: { hookEventName: "PreToolUse",
          permissionDecision: "deny", permissionDecisionReason: decision.message } } : {};
      }] }, ...(hooks.PreToolUse || [])],
    },
  };
}
