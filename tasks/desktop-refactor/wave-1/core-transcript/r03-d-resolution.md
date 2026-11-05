# R03-D Review Resolution

Reviewed head: `b3c75eb256e767e353334c1ac54a23fa377254d5`.
Current base: `0d789192596d01a387d9dc69efae1ec53de13b25` (PR #19 squash merge).
Develop was fetched and merged normally; no force-push/cherry-pick. Diff from
develop contains characterization/test evidence and plan changes only, not #19 again.

| Finding | Resolution | Evidence |
| --- | --- | --- |
| Failed test can publish baseline | Publication now runs after qExec; any nonzero test result yields failed/baselineValid=false and nonzero process exit | C++ failedRun plus real executable failed-run negative test |
| Partial/filtered run can publish baseline | Require all four characterization slots plus exactly three unique samples (0,1,2) of each items-1k/items-10k/giant-turn | Missing slot/row, duplicate, unknown scenario, fractional/string sample tests; real filtered-slot run exits nonzero with partial |
| Write >0 treated as complete | writeAll requires every byte; QSaveFile has direct-write fallback disabled and commits atomically | Exact serialized bytes readback, zero-progress writer and missing/directory output failures |
| Artifact diagnostics unclear | status=complete/failed/partial, baselineValid, testExitCode and publishedAt are explicit; failed/partial diagnostics may be atomically written but cannot be successful baseline | Real subprocess assertions |
| Configure-time clean HEAD misleading | Build-time provenance target reads current HEAD, worktree dirty state and deterministic source/header digest before compilation; unchanged header is not rewritten | Synthetic Git repo test changes source under same HEAD and asserts dirty flag/new digest |
| Old base/build SHA conflated | Historical r03-c-raw-release.json keeps original 56749d0 build SHA; current base recorded separately | production-characterization.md and this handoff |
| Green hosted checks treated as dedicated target coverage | Existing root workflow invokes root build/CTest only; isolated project is not included there | Run34681933580 at b3c75eb lists generic build/Test steps; no isolated target registration |
| Paging readiness overstated | Backend e2a0844 proposal read-only; decisions remain BLOCKED and concrete options recorded | r03-e-implementation-plan.md includes current/proposed/owner/prerequisite/evidence/status, T1-T8, execution prompt |

## Commands And Artifact Rules

```sh
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs --materialize /tmp/r03-fixtures
cmake -S tests/daemon/characterization -B /tmp/r03-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build /tmp/r03-release
/tmp/r03-release/r03_metrics_tests
R03_TEST_EXECUTABLE=/tmp/r03-release/r03_characterization R03_FIXTURES=/tmp/r03-fixtures node --test tests/daemon/characterization/metrics-run.test.mjs
R03_FIXTURES=/tmp/r03-fixtures R03_METRICS=/tmp/r03-metrics.json /tmp/r03-release/r03_characterization
git diff --check
```

Always build before running; provenance is captured at build time, not rewritten
at execution to masquerade as a newer revision. Do not edit source during a build.
sourceDigest hashes sorted repository-relative source paths + per-file SHA-256,
covering journal/projector/protocol sources/headers and files in the isolated test
directory. Qt/compiler/platform are recorded separately. sourceDirty covers the
whole worktree, including untracked files; a dirty run can finish but is explicitly
marked dirty, not attributed solely to clean HEAD. The build tree should stay outside
the source tree. No absolute source paths are written into the metrics artifact.

When R03_METRICS is supplied, incomplete or failed runs exit nonzero even if QtTest
accepted a filtered test selection. A publication failure exits nonzero and leaves
an existing destination unchanged through atomic-save semantics. Consumers must
check process exit, status/baselineValid and publishedAt; never reuse an older artifact
after a failed invocation. A crash/signal may leave no new artifact. There is no claim
that partial diagnostics constitute a successful baseline.

The original giant rejection/truncation reproducer was superseded by ADR 0006. The
characterization now requires bounded persistence and exact reconstruction of the
normalized >=8 MiB payload; a terminal manifest remains partial until verified.
Native frame/layout/WebView/RSS remain not_measured;
encryption/upload remains source-traced rather than dynamically verified.

## Integration Registration Request

Owner: Integration, related CI PR #22. Apply serially after #21 source is available
in the CI checkout. Do not conditionally skip when the target directory is missing.
Keep root CMake/workflow changes outside this PR. Add a distinct required step/job
on the existing macOS/Linux Qt6.11.1 Release matrix, with Node22+ and CMake/Ninja.
Use the commands below in order (shell fails on first unexpected nonzero exit):

```yaml
- name: R03 isolated characterization
  shell: bash
  run: |
    set -euo pipefail
    node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs --materialize "$RUNNER_TEMP/r03-fixtures"
    cmake -S tests/daemon/characterization -B "$RUNNER_TEMP/r03-release" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$RUNNER_TEMP/r03-release"
    "$RUNNER_TEMP/r03-release/r03_metrics_tests"
    R03_TEST_EXECUTABLE="$RUNNER_TEMP/r03-release/r03_characterization" R03_FIXTURES="$RUNNER_TEMP/r03-fixtures" node --test tests/daemon/characterization/metrics-run.test.mjs
    R03_FIXTURES="$RUNNER_TEMP/r03-fixtures" R03_METRICS="$RUNNER_TEMP/r03-metrics.json" "$RUNNER_TEMP/r03-release/r03_characterization"
- uses: actions/upload-artifact@v4
  if: success()
  with:
    name: r03-characterization-${{ runner.os }}-${{ github.sha }}
    path: ${{ runner.temp }}/r03-metrics.json
    if-no-files-found: error
```

The existing Qt SDK step supplies its CMake prefix. Preserve failed-run logs, but
do not publish them under a successful baseline name. Hosted acceptance requires
exact checkout/build SHA, named step log, nine-row complete artifact and successful
negative checks. Generic green build checks are insufficient.

## Recorded R03-D Checks

Clean Release build SHA: `7caa4094aaa78b764a531b2b96e894d6ce552fd9`.
New artifact: [r03-d-raw-release.json](r03-d-raw-release.json), status=complete,
baselineValid=true, sourceDirty=false, exactly9 unique scenario/sample pairs and
all four required test slots. Source digest:
`d3951e9a0956991bbc40d9aa0297b000ab8f659de6a8b24555d70ee90b1badf1`.
Artifact inspection independently checked status, clean flag, build SHA and each
scenario's sample indices `[0,1,2]`.

- Release characterization:6 QtTest entries PASS, zero failed/skipped (includes init/cleanup).
- MetricsArtifactTest:8 entries PASS, zero failed/skipped (includes init/cleanup).
- metrics-run.test.mjs:4 Node tests PASS, including real failed/filtered/write-failure
  subprocess runs and dirty-source provenance under unchanged Git HEAD.
- `git diff --check`: PASS; vendor gitlink unchanged. No runtime paging changed.

Compiler AppleClang21.0.0.21000101, Qt6.11.1, macOS26.2 arm64. The raw artifact
contains all elapsed/count/byte values, fixture seed and hashes; no percentiles.
This follow-up evidence commit does not alter measured test or production sources.

Hosted isolated characterization evidence: **not available** at handoff. The reviewed
head's generic jobs at
[run34681933580](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34681933580)
passed, but used root configure/build/CTest. Integration PR #22 at
`bfbdf95b8096ba5409c90bbc17133a28aca2cf4b` does not register this isolated target.
The serial patch request above is directed to that PR; registration and a named run
with its complete artifact remain Integration's acceptance work.
