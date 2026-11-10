# Claude API key on the execution host

Status: implemented for macOS; inherited Claude runtime authentication remains available.

## Ownership and data flow

Desktop Settings → Providers configures one Anthropic API key per daemon state path.
The private daemon stores the key, selected authentication mode, query budget, and an
opaque configuration revision together in macOS Keychain. Profiles are separated by
a hash of the absolute daemon state-file path. The public CM submodule is unchanged.
Other platforms keep inherited runtime authentication and report credential storage
as unavailable; they do not silently fall back to plaintext files.

Only authenticated local IPC exposes `provider.claude.credentials.status`, `.save`,
`.remove`, and `.check` (feature `provider.claude.credentials/1`). These commands are
handled before SessionHost and are not persisted in command history or the journal.
The relay command allowlist does not expose them. The UI receives configuration
metadata only, never a saved key. Removing a key keeps only disabled configuration
metadata, so the next query returns to inherited runtime authentication.
Successful credential IPC responses include `ok: true`, as required by the desktop
client, in addition to operation-specific metadata. Failures retain `ok: false`
and the sanitized error message. A real desktop-client/local-server regression
covers status, save, check and remove with in-memory storage and fake networking.
Credential commands interrupted by disconnect are not automatically replayed: the
user refreshes status before retrying an operation with an uncertain outcome.

The iPhone continues to send existing session commands through the paired connection.
Claude executes on its host using that host's selected authentication, whether a turn
originated on desktop or phone. This does not alter KétViệt identity, pairing grants,
the relay endpoint, conversation history, or database ownership.

## Query lifecycle and authentication

Immediately before each `turn/start`, the adapter reads the credential configuration
and sends it through the daemon-owned sidecar's private stdin. It is not part of the
stored conversation options. BYOK supplies the key through the official Agent SDK's
per-query `env`, never its programmatic settings (which the SDK serializes into argv).
Shared `process.env` and user configuration files are not modified. The Anthropic
endpoint is pinned to `https://api.anthropic.com`. The documented
`CLAUDE_CODE_PROVIDER_MANAGED_BY_HOST` mode prevents settings files from replacing
host-supplied provider/authentication/endpoint variables. This mode also gives the
host's model selection precedence over managed model pins; managed model allowlists
still apply. Programmatic settings disable only `apiKeyHelper` and contain no key.
Competing bearer/OAuth tokens, cloud-provider flags and the API-key helper are
overridden for the explicitly selected BYOK query only. Disabling BYOK restores
normal SDK authentication selection; the bundled Anthropic binary is unmodified.

The configuration revision participates in the sidecar launch identity. A running
query retains its captured configuration. A subsequent turn that would change it
while background tasks are live is refused with a recovery message; it does not
terminate the tasks or silently reuse the old billing source. Once work finishes,
the next query resumes the same Claude session with the new configuration.

SDK initialization reports `apiKeySource`. Settings shows the last observed source
separately from the selected mode. `none` means no API key source was reported; it
must not be presented as proof of subscription authentication. Provider auth/cost
observations are whitelisted in memory and do not enter the transcript journal.

## Storage and security limits

The macOS backend uses Security.framework directly, with a dedicated Keychain service
and no shell arguments containing secrets. Locked, denied or corrupt storage fails
closed. Remove is the exception for corrupt or invalid entries: it overwrites the API
or Admin entry with a disabled, keyless configuration without requiring a successful
read, so a broken
entry that fails every Claude turn can be cleared from Settings. A locked or denied
keychain still refuses. Background reads do not open a hidden authorization dialog. A stable signed
daemon identity must retain access across app replacement; if access fails, Settings
instructs the user to unlock/authorize the login keychain. There is no migration of
relay/device credentials or fallback to their existing owner-only JSON store.

The SDK child necessarily receives the API key. This is not a credential isolation
boundary against arbitrary code running as the same OS user, including full-access
agent tools. A future sandbox/proxy design would be required for that guarantee.
The sidecar redacts Anthropic key-shaped strings from normalized output. Do not enable
provider debug logging with real keys or copy process environments into diagnostics.
Neither a user-entered key nor provider error bodies are displayed in Settings.

## Validation and costs

“Check connection” calls only `GET /v1/models?limit=1` with the saved key, a 10-second
timeout, and redirects disabled. It does not invoke model inference. It establishes
access to the model catalog, not a guarantee of sufficient credits or access to every
model. Provider response bodies and headers are never returned to the UI.

An optional `maxBudgetUsd` stops an SDK query according to its client-side cost
estimate. It is not a hard account spending cap; estimates can overshoot. The scope is
one query, which can span multiple user turns while background tasks keep it alive.
Settings shows the latest SDK session's cumulative `total_cost_usd` estimate, without
summing repeated cumulative results. Anthropic Console remains billing authority.

## Verification and release

Deterministic tests cover memory-backed storage, invalid/denied/corrupt storage,
local IPC authentication and secret-free responses, endpoint/redirect policy,
network error mapping, scoped SDK options, background-task rotation fences, redaction,
and native Qt form states. They consume no model quota and do not open the user's
Keychain. A signed upgrade with a disposable Keychain item and a real paid API turn
from desktop/iPhone remain release acceptance checks, not claims of this test suite.

References (checked 2026-10-30):
- https://code.claude.com/docs/en/agent-sdk/quickstart
- https://code.claude.com/docs/en/agent-sdk/typescript
- https://code.claude.com/docs/en/agent-sdk/cost-tracking
- https://code.claude.com/docs/en/env-vars
- https://code.claude.com/docs/en/legal-and-compliance

## Separate organization analytics credential

`provider.claude.credentials.analytics.{status,save,remove,refresh}` are authenticated
local-only IPC operations under the existing non-journaled credential dispatcher.
A distinct Keychain account derived from the host profile stores the Admin key; it is
never returned by `launchConfiguration`, forwarded to the sidecar, persisted in session
options, or exposed through relay commands. Inference-key fields reject Admin keys.
Removing either key leaves the other intact. UI callbacks and diagnostic errors never
include provider response bodies or credential values.

The daemon performs only GET requests to the fixed official
`/v1/organizations/analytics/cost` and `/usage` endpoints with the
`analytics-usage-cost-2026-08-24` beta header. It queries UTC month start to the current
instant without dimensions or filters, follows bounded pagination, and publishes totals
only after both reports validate. Amount strings are cents and are converted to USD;
reports are informational, not payment calculations. Requests reject redirects, cap
response size and time, cache successful reports for five minutes and reject overlapping
refreshes. Credential changes invalidate the cache and fence old in-flight responses.
There is no inference request and no write to the organization's billing configuration.

The selected Admin organization can differ from the inference-key organization; this is
stated in the UI. The report covers all organization products and keys. It is not a
prepaid-credit balance or an enforced account budget. The documented Enterprise member
Spend Limits API is not treated as a Console organization spending limit.

References: [SDK cost semantics](https://code.claude.com/docs/en/agent-sdk/cost-tracking),
[Analytics API](https://platform.claude.com/docs/en/manage-claude/usage-cost-analytics-api).

## Paired iPhone reporting

The phone consumes canonical `usage.session` events for captured authentication mode,
SDK cumulative session cost/tokens and query budget. These values replace earlier readings;
they are never summed or divided into a budget progress percentage. API mode hides plan quotas.

An explicit `session.configuration.read` with `effective_only: true` and
`include_claude_usage: true` returns `claude_account_usage` to an authorized paired client.
It uses the existing encrypted configuration command/result and context revision fence.
The host validates session generation, lifecycle, provider and context both before and after
fetching the report. No new relay command kind or credential write capability is exposed.
The result contains only supported/configured/stale flags, sanitized failure text, and the
aggregate cost/token/period/freshness fields. It is not appended to the transcript journal.
Paired clients that can read this host's Claude session configuration can read this organization
aggregate; it covers all keys/products of the saved Admin credential's organization.
Admin save/remove and the Admin secret remain local to the host. The phone holds reports in
sheet memory, not its persistent session cache. Older hosts omit the field and the phone asks
for a host update; offline and failed reads do not present a zero balance.
