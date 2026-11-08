# ADR 0072: Bound and identify provider version probes

Status: accepted for version-probe result admission; broader provider framing remains under audit.

The version probe truncated output to 4 KiB only after QProcess finished. It could
accumulate a large stdout/stderr stream, accept a version printed by a process
that exited with an error, and give every concurrent waiter the result of the
first executable. Replacing the executable during an in-flight check also joined
the stale result. The original fixture reproduces five failures; an added
same-path replacement probe reproduces a sixth.

Capture merged output incrementally, retaining at most 4 KiB plus one byte of
lookahead. On overflow, close the channel and kill the one-shot probe. A normal
exit with code zero and bounded output is required before interpreting a banner.
The existing timeout and stdin-EOF behavior remain. Unreadable, failed or excessive
output produces the existing Unknown status and is not cached, allowing retry.
Unknown still uses the existing warning/launch policy; this does not change
provider authorization or silently introduce a new launch prohibition.

Only requests for the same executable and unchanged fingerprint join an active
probe. Defer other requests, then re-enter the ordinary cache/fingerprint check
after it finishes. This preserves each executable's result, coalesces equivalent
queued requests and rechecks an on-disk replacement. Guard deferred callbacks
with QPointer so destroying the owner does not leave a callback using it.
Fingerprint identity remains path, size and modification time, not a cryptographic
attestation or an immutable snapshot of the executable.

The focused fixture passes 13/13: valid/exact-limit/EOF controls, stdout/stderr
excess, a 64-MiB flood killed before its completion marker, nonzero exit, empty
output, timeout, retry/cache behavior, distinct concurrent paths and same-path
replacement. It launches only its own local helper process. The affected adapter,
compatibility and lifecycle matrix and independent review remain merge gates.
No UI, shared header, live provider, credentials, user journal, installation or
deployment is changed. Windows execution is not claimed. Process-internal peak
memory, waiter admission bounds and broader stderr/encoding policy remain separate.
