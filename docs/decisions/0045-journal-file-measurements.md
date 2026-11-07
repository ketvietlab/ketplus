# ADR 0045: Advisory journal file and filesystem measurements

Status: accepted; incremental #128 Storage measurements.

The local owner-only retention read/apply responses add an optional `disk_usage`
object alongside the existing SQLite page `usage`. It reports logical file lengths:
`database_file_bytes`, `wal_file_bytes`, `shared_memory_file_bytes`, their checked
sum `total_file_bytes`, and UTC `sampled_at`. It is not allocated filesystem blocks
(compression/sparse files may differ) and does not include media, backups, migration
copies or other application files. Values are sampled, not a cross-file atomic
snapshot. No content scan or checkpoint is performed to collect them.

Resolve the main database filename using SQLite's database list. Non-file journals,
missing main files, unreadable/nonregular files and out-of-range totals return a
separate `disk_usage_error`; a successful policy save still reports success.
Filesystem `filesystem_available_bytes` is present only when the volume is valid,
ready and supplies a nonnegative JSON-safe value. Missing availability means
unknown, never zero or a healthy result. No file paths are returned.

Settings Storage displays these numbers separately from page allocation and states
the excluded files. It warns when available space is below 5,000,000,000 bytes
(5 decimal GB); the exact boundary is not low. Invalid optional metrics do not
invalidate valid policy/revision acknowledgments. Old daemons without these fields
remain compatible and show unavailable/unknown metrics. The notice uses the same
accessible label text as sighted users see. Measurements and warnings refresh on
load/reload/save; there is no background monitoring or sidebar warning in this step.

This does not implement size enforcement, per-part/per-conversation accounting,
media/archive policies, Compact now, export or remote storage status. Those remain
#128 work. It does not package, deploy or install a daemon.
