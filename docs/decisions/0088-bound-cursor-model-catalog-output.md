# 0088: Bound Cursor model-catalog process output

Status: accepted implementation; independent review pending.

## Context

The `cursor-agent models` process had a 15-second deadline but retained both
QProcess channels until exit. A noisy or broken executable could therefore grow
daemon memory throughout that interval. The successful-looking stdout prefix of
a nonzero exit was also accepted as a model catalog.

## Decision

Read stdout and stderr on their ready-read signals, in chunks of at most 64 KiB.
The process has a shared 1-MiB output budget: stdout is retained for parsing and
stderr is drained without retention, while both consume the budget. At the first
excess byte, discard the retained stdout, close both read channels and kill the
process. Exact-budget output remains valid.

Only normal exit with code zero and no overflow may publish a parsed catalog.
Overflow, timeout, launch failure and unsuccessful exit settle the waiting callers
with the existing built-in catalog. Existing waiter coalescing and the 60-second
catalog cache remain unchanged. Close the unused stdin on startup. Completion is
bound to the specific process instance and disconnects its signals before settling
callbacks, so stale completion cannot consume another process's waiters.

## Verification and limits

The Qt test executable acts as the CLI fixture. Tests cover stdout/stderr budget
boundaries, excess output, unsuccessful exit, pre-exit channel draining, shared
callers/cache, timeout after actual output, and failed launch. No provider is used.
The same fixture is run against the previous production source as a regression
control; run results are recorded in the backend-hardening plan.

The byte budget bounds retained adapter output and total accepted catalog bytes.
It is not a proof of a universal RSS bound inside QProcess or Qt's text/model
parser. Catalog waiter admission, executable attestation and malformed JSONL
response lifetimes are separate from this one-shot catalog path. No UI, Windows,
live-provider, installed-app or deployment verification is claimed.
