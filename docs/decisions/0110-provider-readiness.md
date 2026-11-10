# ADR 0110: Report whether each provider can start a turn

Status: accepted (2026-11-10, #170).

## Context

KetPlus learned that a provider could not work only after a turn had failed. It
then showed the provider's raw text. Settings → Providers showed only the CLI
version check (ADR 0015). The provider setup guide in the mock (#169) needs to
know, before a turn starts, whether each provider is installed, current and
signed in. It also needs the commands that fix each problem.

The constraints in #170:
- reading the state must start no turn and spend no quota;
- KetPlus never runs install or login commands;
- KetPlus never reads or stores provider credentials.

## Decision

Each provider in `provider.runtime_status` gets a `readiness` object:

- `state`, in this order of precedence:
  1. `missing` from the version check; Claude reports `runtime_missing`, because its
     SDK ships inside KetPlus and a missing sidecar means a damaged install;
  2. `outdated` from the version check;
  3. a failure the provider reported: `signed_out`, `expired`, `key_rejected` or
     `limit_reached`;
  4. the sign-in probe: `ready` or `signed_out`;
  5. otherwise `unknown`.
- `since`: when the state began, in seconds since the epoch.
- `resets_at`: for `limit_reached` only.
- `message`: the provider's own text, kept verbatim.
- `account`: how the provider is signed in, such as an email or "API key". It is
  shown only on this Mac and never logged. `provider.runtime_status` remains a
  local-owner query.
- `install_command`, `login_command` and `update_command` for this platform.

The sign-in probes:
- **Codex:** `account/read` with `refreshToken: false`. An account means `ready`.
  No account while `requiresOpenaiAuth` is set means `signed_out`. With no answer
  within 5 seconds, or on an error, the probe stays silent.
- **Cursor:** `CURSOR_API_KEY`, or `authInfo.email` in `~/.cursor/cli-config.json`.
  A missing file or a file without that field means `signed_out`; an unreadable
  file says nothing.
- **Claude:** no probe. Claude Code keeps its sign-in in the Keychain, which
  KetPlus does not read. Instead, the sidecar's `provider.authentication` source is
  recorded when a query starts:
  - `none` means `signed_out`;
  - any other source means `ready`.

  A source never clears an earlier `expired` or `key_rejected`. Only a turn that
  works clears them.

A reported `signed_out` ends as soon as a probe finds an account, since the user
signed in after it. Claude has no probe, so its `signed_out` lasts until the next
query reports a source; the UI therefore never blocks sending on it.

Reported states persist in `provider-readiness.json` beside the journal. The file
is owner-only and keeps them across daemon restarts. A limit is dropped once
`resets_at` passes.

### Turn error codes

`runtime.error` carries a `code` when the error is about the provider rather
than the work:
- `auth_required` and `auth_expired`;
- `key_rejected`;
- `limit_reached`;
- `model_capacity` and `plan_blocked`;
- `provider_unavailable`.

A coded error also names the turn's `provider` and, when known, its `model`, so
its guide still names them after the conversation switches.

`message` stays verbatim.

- **Classification.** Structured fields decide first: Codex `codexErrorInfo`
  (`usageLimitExceeded`, `unauthorized`, `serverOverloaded`), kept as
  `provider_info`. The provider's text is the fallback.
- **Readiness.**
  - `auth_required`, `auth_expired`, `key_rejected` and `limit_reached` set the
    reported state.
  - A capacity or plan error concerns one model, so the provider stays ready.
  - A turn that completes clears any reported failure.
  - For a Codex limit, the reset comes from `account/rateLimits/read`: it is the
    reset of the window that is used up.
- **Notification.** Clients learn of the change from the coded `runtime.error`
  itself and read `provider.runtime_status` again.
- **Check again.** It sends `provider.runtime_status {recheck: true}`. That forgets
  what can be told again: a sign-out, which the probe checks, and a limit with no
  known reset, which nothing else would end. It then runs every probe again. An
  expired sign-in, a rejected key and a pending limit stay until a turn works or
  the credential changes. Saving or removing a Claude API key forgets Claude's
  failures.
- **Classification.** Text is read only when Codex gave no other
  `codexErrorInfo`, and only for the provider's own sentence at the start of the
  message. "MCP server github: authentication required" is about the work, not
  the account.

### The setup guide in the desktop app

- `provider_setup::guide` (modules/ai) turns a runtime_status entry, or a coded
  `runtime.error` with its model, into the mock's guide: id, badge, tone, the
  provider's own words, numbered steps with the daemon's commands, and the
  primary action. Settings, the composer and the transcript share it.
- Settings → Providers shows the guide's badge and rows, then the guide card, for
  each provider that cannot start a turn. A capacity or plan error concerns one
  model and stays in the conversation.
- A command is copied, or typed into a terminal without Return. KetPlus never
  runs it. The card's Check again sends `recheck: true`; when the same guide comes
  back, it shows the guide's `still` text.
- In the Agent Workspace, a provider that cannot start a turn (not installed, too
  old, signed out, or Claude's runtime missing: the guide's `composer`) shows its
  guide above the composer, and the model button warns. The user can hide the
  guide; the model button shows it again.
- In Model & context, a provider with a guide keeps its tab even without models.
  A banner names the problem. Without models it says they appear once the
  provider is set up, and Apply stays off, since the draft still holds another
  provider's model. When the guide blocks and models are listed, it adds that a
  model can be chosen now and turns start once the provider is set up. Show setup steps
  closes the picker and opens the guide in a dialog with Check again; the dialog
  closes once the provider has no guide.
- A blocking guide holds the message in the composer. The message sends once a
  fresh read lists the same provider without a guide, and only if the same
  conversation's draft is unchanged. A stopped daemon or a guide that merely
  stops blocking does not send it, and editing the draft or opening another
  conversation lets it go. Claude's sign-out does not block, since only a Claude
  query reports a new sign-in. Cursor's sign-out does not block either: it is
  read from the CLI's config file, and an API key may live only in the user's
  shell.
- A coded `runtime.error` row keeps the provider's own words, the code and the
  failed turn's model, beside its own text. The
  failure that ended the latest turn shows the guide (`ProviderRuntimeStatus::
  turnGuide`, with the commands the daemon last reported) in place of the danger
  notice. It offers Retry when the turn can be resent. Once a later turn starts
  (the user's or an agent's) or a newer coded failure follows, the row reads as
  a notice.
- A live coded error, and a completed turn while a guide is shown, read
  `provider.runtime_status` again; replayed history does not.

## Consequences

- Settings, the composer and the transcript can show the setup guide before a
  turn fails.
- Until Claude's first query, its sign-in state is `unknown`.
- Codex's probe starts the app server if it is not running; it starts no thread.

## Alternatives considered

- **Reading Claude Code's Keychain item.** This would mean touching provider
  credentials, which #170 forbids.
- **Running `codex login status` or `agent status`.** These are extra processes per
  query, and their output is free text that changes between versions.
