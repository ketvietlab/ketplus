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
