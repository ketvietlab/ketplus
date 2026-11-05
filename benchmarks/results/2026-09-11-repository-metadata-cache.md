# Repository metadata lookup cost

Measured 2026-09-11 on Apple M1 Pro, 32 GiB RAM, macOS 26.2, Qt 6.11.1,
Release build. Source: working-tree changes on base `40f0e4d`.

Command: `bash benchmarks/run-repository-metadata.sh`.
Each run measures 100 warm SQLite reads (opening/closing the connection for each
read) and 100 Git identity subprocess calls in the same temporary repository.
No provider calls or user repositories are involved. This is not a startup or
first-frame benchmark, and no claim is made about end-to-end UI speedup.

Raw output:

```text
RUN=1
LOOKUPS=100
CACHE_READ_TOTAL_MS=65.837
GIT_LOOKUP_TOTAL_MS=1885.727
RUN=2
LOOKUPS=100
CACHE_READ_TOTAL_MS=44.158
GIT_LOOKUP_TOTAL_MS=2206.411
RUN=3
LOOKUPS=100
CACHE_READ_TOTAL_MS=45.825
GIT_LOOKUP_TOTAL_MS=1790.669
```

Median totals: 45.825 ms for cached reads and 1885.727 ms for Git calls.
Background Git validation still takes place; the cache removes it from the
initial display path rather than eliminating it entirely.
