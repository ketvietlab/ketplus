# 0067: Automatic turn review by a chosen reviewer conversation

Status: accepted (2026-11-08). Implements #120; the header picker is part of #159.

See also: ADR 0114 (agent teams) reuses this engine with a second trigger: a team's
reviewer reviews each task when it enters In review, and a member's own setting pauses.

## Context

Users already pair conversations by hand: a working agent finishes a turn, the user
asks another conversation to review it, and pastes the findings back. ADR 0027 gives
conversations numbers and an `AgentBroker` that delivers messages, wakes idle
receivers, turns a final answer into a reply and limits automatic turns. #120 asks
KetPlus to run that loop itself, with a reviewer that can never change the
repository.

## Decision

### Assignment

A conversation may name one **reviewer conversation** on the same host. The daemon
stores it in `conversation_reviewers` (working session id, reviewer session id,
`auto_fix`, `max_rounds`, update time) in the local journal. Defaults:
`auto_fix = true`, `max_rounds = 2`. Valid values are 1–3, because ADR 0027 holds
messages past hop 6. A conversation cannot review itself, and the reviewer must
be a Claude conversation, the only provider that enforces the guard below on every
tool call. Deleting either conversation removes the row.

Authenticated local Desktop IPC reads and writes the assignment
(`session.reviewer.get`, `session.reviewer.set`), advertised in `daemon.hello`
capabilities. Agent-token and cloud-control connections cannot call it.

The Desktop picker offers **No reviewer**, **New review agent** and **an existing
Claude conversation**. A new review agent (`new_provider`: `claude`) is created
when the user chooses it: a read-only conversation in the same worktree, titled
"Reviewer for #N", whose first turn only states the role and replies "Ready.".

### Read-only review turns

Every turn the broker starts for a review request runs under a **review guard**,
whatever the reviewer conversation's own mode is. Claude runs it in
`permissionMode: plan`, and the sidecar denies every tool that asks for permission
instead of forwarding a prompt. That covers `ExitPlanMode`, edits, Bash and MCP
tools other than `ketplus_agents`. Read, search and the agents server stay
available. The user therefore cannot accidentally grant a reviewer write, push,
merge or approve access from a prompt.

Cursor is not a reviewer: it forwards permission prompts.

### Codex reviews with Full access (ketplus#295, 2026-11-14)

Codex cannot be held read-only. Codex 0.154 launches the browser MCP server for the
whole app-server process with `default_tools_approval_mode = "approve"`, and
`turn/start` has no per-turn control over MCP servers, so a read-only sandbox would
still leave browser actions available. Instead of a guard that does not hold, a Codex
conversation may review only when its own stored `execution_policy` is `full`, and its
review turns run with that Full access (`danger-full-access`, approval policy `never`).
The review role ("build and test in a scratch copy; do not edit, commit, push, merge,
approve or comment") is in the prompt only. A new reviewer starts read-only, so it is
always Claude; a Codex reviewer is an existing Codex conversation with Full access. The
refusal reads "Codex can review only with Full access.". Claude's guard is unchanged.

The daemon checks the reviewed branch around each Codex review turn instead. Before the
turn starts it records, off the event loop, for each review request: the reviewed
worktree's HEAD, its symbolic ref and a SHA-256 of `git status --porcelain=v2
--untracked-files=all`; for a team's task also its `head_commit` and the remote branch
head (`git ls-remote origin <branch>`, best effort: a remote that does not answer is
logged and not compared). After the turn ends, with any status, it records them again.
If anything changed, the verdict does not count: the request fails with
`review_tampered`, no review is recorded and no round is used, and the working
conversation gets a `review_tampered` notice that lists each field before and after.
Nothing is reverted. If nothing changed, the verdict counts as usual.

Residual risk, accepted: the check sees only the reviewed worktree and branch. It does
not detect what the reviewer does with the user's credentials elsewhere: pushes to other
branches or repositories, `gh pr merge`, a pull request review or comment, browser
actions (`ketplus_browser` approves itself under Codex), or reading or sending secrets.
A pull request merged that way shows up later on the board, not as a tamper notice. A
change the working conversation itself makes while the review runs also voids it.

Because a conversation can hand off to another provider or lower its policy after it
was chosen, the broker checks provider and policy again when it asks for a review and
when it delivers one. A reviewer that no longer qualifies gets no review: the request
is skipped or fails with `reviewer_unsupported`.

The guard applies only to review requests, and a review request runs only in such
a turn. A queued review waits for the reviewer's next own turn rather than joining
a turn the user starts there, and a held one waits for an explicit release. The
reviewer's user-started turns keep its mode. While a review request is queued or
held, `agents_inbox` shows the reviewer its row but not its body, so a turn under
another mode cannot read the request and carry it out unguarded.

### Two review profiles

The guard has two profiles. The broker chooses one for each turn from the messages
that start it (`review_profile` next to `review_guard` in the turn's origin), and
clients can set neither:

- **read**: a review request from another conversation, the review of one finished
  turn described above. This is a reading job: plan mode, the local reading and search
  tools (Read, Glob, Grep, LS, NotebookRead, ToolSearch), the agents server, and every
  other tool (Bash, WebFetch and WebSearch included) refused without a prompt.
  The refusal is a `PreToolUse` hook, as for the verify profile: plan mode alone only
  tells the model not to act, and an allow rule in a user or project settings file
  lets a tool call past `canUseTool`, which is how Bash ran in guarded reviews before.
  A batch that holds such a request runs entirely under this profile.
- **verify**: a review request the team itself sends for a task in `in_review`
  (ADR 0114 §2). It has no sending conversation, which no conversation can imitate.
  The reviewer checks acceptance criteria that need builds, tests and screenshots.
  Claude runs in `permissionMode: default` and decides every tool call without a
  prompt. It may read and search, run commands, use `ketplus_agents` and
  `ketplus_browser`, and edit files only inside its scratch directory
  (`review-scratch/<session id>` next to the journal, removed with the session). It
  is refused, with a message that a review cannot do it, edits outside the scratch
  directory; git commands that commit, push, merge, rebase, reset, switch or check
  out, stash, clean, tag, or remove or move a worktree or branch; `gh` pull-request
  merge, review, close, edit or comment, and `gh api` writes; any other MCP server;
  sub-agents; `ExitPlanMode`, `EnterPlanMode` and `AskUserQuestion`.

How the verify profile is enforced:

- **Writes by commands** are bounded by the provider's command sandbox, not by
  reading commands: `sandbox.enabled` with `failIfUnavailable`,
  `allowUnsandboxedCommands: false` and a write allow-list of the scratch and
  temporary directories, with the reviewed worktree on the deny list even when it
  lies under a temporary directory. A platform without the sandbox (Linux without
  bubblewrap) fails the review turn instead of running it unbounded.
- **Tool calls** pass a `PreToolUse` hook that blocks on failure. It runs before
  permission rules, so an allow rule in a user or project settings file cannot widen
  the profile. `canUseTool` takes the same decision for whatever still asks.
- **Command text** is checked as a second layer only: each simple command of a line,
  through pipes, chains, subshells and `env` or `sudo` prefixes, is matched against
  the git and `gh` lists above.

Residual risk, in plain words: the command list is best effort. A command that hides
`git push` or `gh pr merge` behind a script, an alias or another program is not
recognised by the text check, and the sandbox does not stop network requests, so such
a command can still publish to a remote with the user's credentials. It cannot change
the files of the reviewed checkout or its `.git` directory, because the sandbox
refuses those writes. A build that writes outside the scratch and temporary
directories (a cache in the home directory, a build tree inside the worktree) fails,
so the verify turn sets the common cache variables (`XDG_CACHE_HOME`, `npm_config_cache`,
`CCACHE_DIR`, `PIP_CACHE_DIR`, `GOCACHE`, `CARGO_HOME`) into the scratch directory and
tells the reviewer, in its system prompt, its scratch path and where builds go: a CMake
tree with `-B <scratch>/build`, `xcodebuild -derivedDataPath <scratch>/DerivedData
-clonedSourcePackagesDirPath <scratch>/SourcePackages`, and a copy of the checkout in the
scratch directory for a script that writes into its own tree.

Each turn's profile depends only on that turn's launch. The sidecar reuses a live
query only for a launch with the same profile and scratch directory, and starts a
fresh query, resuming the same provider thread, when the profile changes. A new
session, a resumed one, one already loaded under another mode and one after a daemon
restart therefore get the same profile for the same review request.

### Trigger and request

When a turn of a conversation with a reviewer ends with status `completed`, the
broker asks for exactly one review:

- Cancelled, failed or stopped turns, and conversations without a reviewer,
  trigger nothing. Their changes stay in the next review's diff.
- A review is skipped with a `review_skipped` notice in the working conversation
  when the reviewer no longer exists (`reviewer_missing`), no longer enforces the
  guard (`reviewer_unsupported`) or is busy (`reviewer_busy`), or when the chain
  already used `max_rounds` reviews (`round_limit`).
- The turn's changes are the difference between two fixed snapshots of the
  worktree. A snapshot is the tree of every tracked and untracked, not ignored
  file. It is written by `git add -A` and `git write-tree` into a copy of the
  worktree's index, so the worktree and its index stay untouched. That includes
  edits to untracked files that already existed. One snapshot is taken when the
  reviewer is assigned and one after each completed turn, with git run off the
  event loop. A newer snapshot of the same conversation always wins over an older
  one that finishes later.
- A turn whose tree equals the baseline asks for no review and leaves a
  `no_changes` notice. If git fails, the notice says `snapshot_failed` and the
  baseline stays, so the next turn's review still covers these changes.
  Snapshots live in memory, so after a daemon restart the first comparison is
  with `HEAD`.

The request is an ordinary ADR 0027 message of kind `review` with `expect_reply`.
It names the working conversation's number and worktree, the base snapshot, the
user's request for the chain, the changed files and the diff. It also asks for a
structured answer ending in `Verdict: no findings` or `Verdict: findings`. The
whole request stays within the 32 KiB message limit: the file list is capped, the
diff keeps whole files while they fit, and a note names the file that was cut and
every file left out.

### Reply and loop

The reviewer's answer becomes the reply as in ADR 0027 and appears in the working
conversation attributed to `@agent/<reviewer number>`. Whether it arrives as the
final answer or through `agents_send` with `reply_to`, the broker reads the
verdict line:

- `findings`, `auto_fix` on and rounds remaining: the reply is delivered normally,
  so the idle working agent starts a turn to address it. That turn's completion
  asks for the next round.
- `no findings`, `auto_fix` off, or the last round: the reply is recorded with
  delivery `review_record` and shown in the working transcript as an
  `agent_message.state` notice (`review_recorded`, with its verdict and body),
  without starting a turn.

A reply without a verdict line has no verdict (`no_verdict`): it is recorded like
`no findings`, without starting a turn, and is not read as findings. A reply that only
says the reviewer cannot act (plan mode, a pause) must not send the working agent to
fix it. Revised by T-19; earlier, a missing line counted as findings.
Turns started by a review request are never reviewed themselves, even when two
conversations review each other.

ADR 0027's hop, chain and hourly limits still apply, and the user can stop a
running turn at any time.

## Consequences

- Review turns spend the reviewer provider's quota without a user prompt, bounded
  by `max_rounds` and the ADR 0027 limits. Quota-aware skipping is not part of
  this change.
- The diff is fixed when the turn ends; the reviewer reads the same worktree,
  which may have moved on if the user starts another turn meanwhile.
- Snapshots write blobs for untracked files into the repository's object store,
  as `git stash create` does for tracked ones. A very large untracked tree that
  git cannot snapshot within 10 seconds is reported as `snapshot_failed`.
- Tests use the scripted fake provider and never call a real provider.
