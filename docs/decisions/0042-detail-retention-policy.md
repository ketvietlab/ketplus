# ADR 0042: Revisioned local detail-retention preferences

Status: accepted; foundation for the Storage settings in #128.

Persist the supported detail-retention policy in a singleton SQLite table separate
from session configuration. Defaults preserve existing behavior: 30 days and
automatic detail retention enabled. Full detail can be kept for 7, 30 or 90 days,
or indefinitely (`full_detail_days: 0`). `automatic_detail_retention: false`
disables the periodic transformation/removal of replay detail. Reclaiming already
free pages and the existing startup incremental-vacuum migration are unchanged:
neither removes retained conversation content. This is not yet the mock's broader
all-compaction toggle, a size-limit policy, image policy or archive expiry.

Both fields are required and unknown fields, wrong JSON types and unsupported
values are rejected. Stored policy JSON is bounded to 4 KiB. A missing row means defaults with revision zero; a present
but malformed row fails closed and automatic detail trimming is skipped. A save
uses a transaction and expected-revision comparison, increments revision, and
restarts the bounded retention scan. Failed/stale saves leave the prior policy
intact. Revisions are nonnegative JSON-safe integers; persisted rows begin at one.
A future daemon must preserve validation or explicitly migrate this contract.

The owner-token local IPC capabilities `storage.retention.read` and
`storage.retention.apply` expose this policy. Read accepts no parameters. Apply
requires exactly `policy` and `expected_revision`. Responses include `ok`,
`revision`, and `policy`; `usage` contains advisory SQLite page allocation/free/kept
bytes, not a complete disk footprint or per-conversation accounting. A metrics
failure is separate from save success. A stale revision returns `policy_conflict`;
invalid policy returns `invalid_policy`. Clients must discover the capabilities
and refresh after conflicts rather than overwriting another Settings window.

Agent-token connections remain agents-only. Cloud-control callers are explicitly
refused before command-result-cache lookup, including when replaying a cached
local mutation ID. Applying a policy never starts provider work or changes active
turns; the existing idle, ACK, current-generation, newest-20-turn and question/
approval guards still decide whether each replay row is eligible.

The generic CLI can inspect the local policy without a Desktop UI:

```sh
ketplusctl call storage.retention.read
ketplusctl call storage.retention.apply '{"expected_revision":0,"policy":{"full_detail_days":90,"automatic_detail_retention":true}}'
```

Use the actual revision returned by read.

The native Desktop Settings → Storage page exposes these two supported fields and
advisory page usage. It uses the existing Settings shell and Qt controls (the native
Két design-system adapter), not a web shell. Choices save immediately, with controls
disabled until the response; opening or reloading never writes defaults. Failed,
conflicting or malformed responses disable edits until an explicit reload. A saved
policy remains saved even if usage metrics are unavailable. Late replies after the
page closes or after a 20-second timeout are ignored. The page hides the global
appearance save/reset footer because these local choices save immediately. Detail loss already applied cannot be reversed by Forever.

The Desktop bridge discovers owner IPC capabilities before calling either operation;
it refuses older/disconnected daemons and never forwards storage methods to a
provider. Storage requests interrupted by disconnect, including queued requests,
fail instead of being replayed after reconnect. The user reloads to resolve an
uncertain save; the daemon may have committed before the connection was lost.

Size pressure, archive/media retention, detailed storage measurements and warning
presentation remain separate #128 work. The broader Qt mock remains a design
preview and labels the implemented subset. No release or installed-journal
acceptance follows from these APIs or this Settings page.
