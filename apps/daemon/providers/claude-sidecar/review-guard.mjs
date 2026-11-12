// ADR 0067 and ADR 0114: the two profiles a review turn runs under.
//
// "read" (a review of another conversation's turn, ADR 0067): plan mode, only the agents
// server, every other tool refused without a prompt.
//
// "verify" (a team's review round, ADR 0114 §2): the reviewer reads, builds, runs tests and
// uses the browser without a prompt, writes only inside its scratch directory and the temporary
// directory, and cannot change, commit or publish the work. Writes by commands are bounded by
// the provider's command sandbox; the command checks below are a second, best-effort layer.
import { realpathSync } from "node:fs";
import os from "node:os";
import path from "node:path";

export const readRefusal = "A review is read-only: reply with your findings instead.";

// The profile a turn asks for; "" for a turn that is not a review.
export function reviewProfile(params) {
  if (params.reviewGuard !== true) return "";
  return params.reviewProfile === "verify" && typeof params.scratchDirectory === "string" &&
    path.isAbsolute(params.scratchDirectory) ? "verify" : "read";
}

const readTools = new Set(["Read", "Glob", "Grep", "LS", "NotebookRead", "TodoWrite", "TodoRead",
  "WebFetch", "WebSearch", "BashOutput", "KillShell", "KillBash", "ListMcpResourcesTool",
  "ReadMcpResourceTool", "ToolSearch"]);
const editTools = new Set(["Edit", "MultiEdit", "Write", "NotebookEdit"]);
const modeTools = new Set(["ExitPlanMode", "EnterPlanMode"]);

// Git subcommands that change a checkout, its history or a remote.
const gitWrites = new Set(["commit", "push", "merge", "rebase", "reset", "switch", "checkout",
  "restore", "clean", "stash", "cherry-pick", "revert", "am", "apply", "pull", "tag",
  "update-ref", "symbolic-ref", "filter-branch", "replace", "gc", "prune", "notes", "rm", "mv"]);
// gh pull-request and repository actions that publish, approve or remove.
const ghWrites = new Set(["merge", "review", "close", "reopen", "ready", "edit", "comment",
  "create", "lock", "unlock", "delete", "archive", "rename"]);

function refusal(what) {
  return `A review cannot ${what}: report it in your findings instead.`;
}

// Words of one simple command, quotes removed; enough to name its program and subcommand.
function words(command) {
  return (command.match(/"[^"]*"|'[^']*'|\S+/g) || []).map((word) => word.replace(/^["']|["']$/g, ""));
}

function gitRefusal(argv) {
  let i = 1;
  // Global options before the subcommand, some with a value: git -C dir -c k=v push.
  while (i < argv.length && argv[i].startsWith("-")) i += ["-C", "-c", "--git-dir", "--work-tree"].includes(argv[i]) ? 2 : 1;
  const sub = argv[i] || "";
  const rest = argv.slice(i + 1);
  if (gitWrites.has(sub)) return refusal(`run git ${sub}`);
  if (sub === "worktree" && ["remove", "prune", "move", "add", "repair", "lock", "unlock"].includes(rest[0]))
    return refusal(`run git worktree ${rest[0]}`);
  if (sub === "branch" && rest.some((word) => /^(-[dDmMcCf]|--delete|--move|--copy|--force|--set-upstream-to.*|-u)$/.test(word)))
    return refusal("delete, move or change a branch");
  if (sub === "remote" && ["add", "remove", "rm", "rename", "set-url", "set-head", "prune"].includes(rest[0]))
    return refusal("change a remote");
  if (sub === "config" && !rest.some((word) => /^(--get|--get-all|--list|-l|--get-regexp)$/.test(word)))
    return refusal("change git configuration");
  return "";
}

function ghRefusal(argv) {
  const [, area, action] = argv;
  if ((area === "pr" || area === "repo" || area === "release" || area === "issue") && ghWrites.has(action))
    return refusal(`run gh ${area} ${action}`);
  if (area !== "api") return "";
  for (let i = 2; i < argv.length; ++i) {
    const word = argv[i];
    const method = /^(-X|--method)$/.test(word) ? argv[i + 1] || ""
      : (word.match(/^(?:--method=|-X)(.+)$/) || [])[1];
    // A field turns gh api's request into a POST.
    if ((method !== undefined && !/^get$/i.test(method)) || /^(-f|-F|--field|--raw-field|--input)$/.test(word))
      return refusal("send a write request with gh api");
  }
  return "";
}

// Best effort: every simple command of a line, through pipes, chains, subshells and env/sudo.
export function commandRefusal(command) {
  if (typeof command !== "string") return "";
  for (const part of command.split(/&&|\|\||[;|&\n()`]|\$\(/)) {
    let argv = words(part.trim());
    while (argv.length && (/^\w+=/.test(argv[0]) || ["env", "sudo", "command", "exec", "nohup", "time", "xargs"].includes(argv[0])))
      argv = argv.slice(1);
    const program = path.basename(argv[0] || "");
    const why = program === "git" ? gitRefusal(argv) : program === "gh" ? ghRefusal(argv) : "";
    if (why) return why;
  }
  return "";
}

// The real path of a file that may not exist yet: its nearest existing ancestor, resolved.
function realTarget(file) {
  const missing = [];
  let current = path.resolve(file);
  for (;;) {
    try {
      return path.join(realpathSync(current), ...missing.reverse());
    } catch {
      const parent = path.dirname(current);
      if (parent === current) return path.resolve(file);
      missing.push(path.basename(current));
      current = parent;
    }
  }
}

function inside(file, directory) {
  const relative = path.relative(realTarget(directory), realTarget(file));
  return relative === "" || (!relative.startsWith("..") && !path.isAbsolute(relative));
}

// Where a verify review may write: its scratch directory and the temporary directories.
export function writableDirectories(scratch) {
  return [...new Set([scratch, os.tmpdir(), process.env.TMPDIR, "/tmp", "/private/tmp"]
    .filter((directory) => typeof directory === "string" && directory))];
}

// The decision for one tool call of a review turn, the same in the hook and in canUseTool.
export function reviewDecision(profile, tool, toolInput, { scratch = "", cwd = process.cwd(), agentsInjected = false, browserInjected = false } = {}) {
  const input = toolInput && typeof toolInput === "object" ? toolInput : {};
  const allow = { behavior: "allow", updatedInput: input };
  const deny = (message) => ({ behavior: "deny", message });
  if (agentsInjected && tool.startsWith("mcp__ketplus_agents__")) return allow;
  if (profile !== "verify") return deny(readRefusal);
  if (browserInjected && tool.startsWith("mcp__ketplus_browser__")) return allow;
  if (modeTools.has(tool)) return deny(refusal("change its permission mode"));
  if (tool === "AskUserQuestion") return deny(refusal("ask the user a question; nobody answers a review turn"));
  if (readTools.has(tool)) return allow;
  if (editTools.has(tool)) {
    const file = input.file_path || input.notebook_path;
    return typeof file === "string" && file && inside(path.resolve(cwd, file), scratch)
      ? allow
      : deny(refusal(`write outside its scratch directory (${scratch})`));
  }
  if (tool === "Bash") {
    if (input.dangerouslyDisableSandbox === true) return deny(refusal("run a command outside the sandbox"));
    const why = commandRefusal(input.command);
    return why ? deny(why) : allow;
  }
  return deny(refusal(`use ${tool}`));
}

// The query options of a review turn's profile.
export function reviewOptions(profile, { scratch = "", cwd = process.cwd(), agentsInjected = false, browserInjected = false } = {}) {
  if (profile === "read")
    return { permissionMode: "plan", allowedTools: agentsInjected ? ["mcp__ketplus_agents__*"] : [] };
  const context = { scratch, cwd, agentsInjected, browserInjected };
  return {
    permissionMode: "default",
    allowedTools: [...(browserInjected ? ["mcp__ketplus_browser__*"] : []),
      ...(agentsInjected ? ["mcp__ketplus_agents__*"] : [])],
    additionalDirectories: [scratch],
    // The provider's command sandbox bounds every command's writes; it fails closed.
    sandbox: {
      enabled: true,
      failIfUnavailable: true,
      autoAllowBashIfSandboxed: false,
      allowUnsandboxedCommands: false,
      // The reviewed checkout stays unwritable even where it lies in a temporary directory.
      filesystem: { allowWrite: writableDirectories(scratch), denyWrite: [cwd] },
    },
    // Runs before permission rules, so an allow rule in a settings file cannot widen the profile.
    hooks: {
      PreToolUse: [{
        onFailure: "block",
        hooks: [async (hookInput) => {
          const decision = reviewDecision(profile, hookInput.tool_name || "", hookInput.tool_input, context);
          return { hookSpecificOutput: { hookEventName: "PreToolUse",
            permissionDecision: decision.behavior,
            ...(decision.behavior === "deny" ? { permissionDecisionReason: decision.message } : {}) } };
        }],
      }],
    },
  };
}
