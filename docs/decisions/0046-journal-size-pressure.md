# ADR 0046: Bounded SQLite-page pressure policy

Status: accepted; opt-in backend for #128. ADR 0108 makes the target apply by default.

Persist a separate revisioned size policy (`journal_size_settings`) so older clients
saving the two-field detail policy cannot overwrite it. `limit_bytes` accepts 0
(no pressure), 1/2/5/10 decimal GB; default 5 GB. `warning_percent` accepts integer
1–100, default 80. Strict two-field JSON, a 4 KiB stored bound, expected-revision
CAS, transaction rollback and malformed-row fail-closed behavior match ADR 0042.
A successful policy save resets the pressure phase and bounded scan cursor.
Recent-detail pressure remains **opt-in**:
revision zero exposes the 5 GB target/status but does not shorten the configured
full-detail period. An explicit owner size-policy save enables it (unless no-limit,
Forever or automatic-off applies). This preserves the existing Settings promise
for clients that have never configured a size target. Desktop exposes an explicit Save database target action; opening or editing controls
never opts in. Default automatic activation and complete total-storage acceptance remain pending.

This is a **soft target for allocated SQLite pages**, not a hard filesystem quota.
The status explicitly reports `target_scope: sqlite_pages`. File/WAL/SHM lengths
and filesystem availability remain separate ADR 0045 measurements. Media, backups
and migration copies are not charged to this target. Protected/unsupported content
can keep the database above it; no code deletes conversation history to force a
number. Complete media retention and total-storage budgeting remain open #128 work.

After explicit configuration, at idle, reclaim a bounded free-page batch first. Above target, while at least the
existing minimum reclaim batch remains free, defer detail deletion. On entering
pressure, start a complete bounded expired-detail sweep; only after it commits and
reaches the end, sweep eligible recent detail using the current time as cutoff.
Alternate completed sweeps while over target. Returning below target restores normal
age-based scanning. A changed batch may reclaim newly freed pages afterward.

Use the existing 128-row/1 MiB candidate budget, raw-event ACK exclusion, idle session,
current generation, terminal turn, latest twenty turns, pending question/approval
and supported-item checks. Sweep order is the existing append-row order within each
age phase, not a new global timestamp sort. Unsupported manifests/resources remain.
A restart safely begins the expired phase again. Failed writes never advance a sweep.

Forever and automatic-detail-off preserve their existing promise: they disable both
age and pressure detail transformation. Free pages may still be returned. No-limit
disables pressure only; ordinary age retention remains enabled when configured.

Owner-only `storage.size.read`/`storage.size.apply` use the same request/revision shape
as retention operations. Responses include policy/revision, optional page/file metrics
and warning/over-target/pressure-enabled status. Cloud-control is refused before the
command-result cache; agent tokens remain agents-only. Interrupted storage requests
fail instead of replaying after reconnect. Saving never starts compaction or providers;
the existing idle timer performs work. Retention replies add `size_policy_supported: true`; older daemons omit it and
Desktop keeps the existing detail-only page. All storage methods stay daemon-only
and cannot fall through to a provider. Size saves validate policy and revision ACK,
and errors/conflicts/timeouts require reload. An unsaved size draft survives an
unrelated detail save; a changed size revision blocks that draft until explicit
reload instead of silently rebasing or replacing it. Size status and optional measurements
refresh after read/save; missing metrics never imply a healthy target.

Desktop and CLI can configure the backend; sidebar/remote warnings,
archive expiry, media policy, Compact now and export remain pending. No installed
journal, deployment or device-installation acceptance follows from this backend.

Example local administration (replace the expected revision with the read result):

```sh
ketplusctl call storage.size.read
ketplusctl call storage.size.apply '{"expected_revision":0,"policy":{"limit_bytes":5000000000,"warning_percent":80}}'
```

The explicit save enables eligible recent-detail pressure when the configured
full-detail policy permits it. This can remove detail earlier than its age period.
