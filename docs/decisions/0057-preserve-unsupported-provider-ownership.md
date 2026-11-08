# ADR 0057: Preserve unsupported provider ownership metadata

Status: accepted; unsupported metadata cannot prove that a provider thread is unowned.

ADR 0056 distinguishes a failed catalog read from a complete catalog. Its JSON
consumer still silently converted malformed documents, wrong field types and
unknown entries into empty references. Synthetic fake-provider probes reproduced
queued thread deletion and local deletion for such unsupported values.

Cleanup now interprets ownership through one checked reader. The stored document
must be an object whose nonempty provider keys name object entries. The supported
entry fields are optional `thread_id` (string) and `retired_thread_ids` (array of
strings); absent fields and empty objects/arrays remain compatible. An unknown
entry field may carry references from a newer format, so cleanup preserves it
for recovery rather than using a partial interpretation. The ordinary handoff
reader/writer is unchanged; this is specifically the destructive cleanup boundary.

The checked catalog contains only parsed session identities and references. All
catalog records must validate before any target is considered unreferenced,
including the source record as stored in SQLite. The local source's in-memory
metadata is checked as well. A failure rejects new local deletion before journal
mutation, or leaves queued work pending before a provider request/receipt. No
metadata is normalized, truncated, rewritten or discarded. Repair followed by
maintenance/restart permits the existing shared-reference guard to proceed.

Tests exercise malformed JSON, a nonobject root/provider entry, nonstring thread
IDs, nonarray/nonstring retired IDs, unknown fields and an empty provider ID.
The queued fixture protects a historical shared reference, uses a provider-response
barrier, preserves pending work and retries after repair. Local cases cover both
unsupported source metadata and unsupported retained metadata. The original
production fails 23 of these new scenarios; one empty-provider local case was
already rejected as an unsupported provider. Focused suite passes 37/37 on the fix.

This change adds no schema, public interface, command or UI. It is deliberately
conservative: one unsupported catalog record can defer cleanup until it is repaired
or handled by a compatible version. Catalog enumeration/byte limits, unsupported
SQLite column types, handoff normalization and provider lifecycle races remain
separate hardening work. No provider quota, live journal, release, installation or
deployment is included.
