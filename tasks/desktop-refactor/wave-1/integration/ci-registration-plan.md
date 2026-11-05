# Separate CI Registration

R00-D update: registration is implemented/tested on the exact #18+#21+#22
stack, with [hosted macOS/Linux receipts](r00-d-ci-evidence.md). Standalone #22
remains dependency-blocked until coordinator integration; the R00-C notes below
are historical. No skip or runtime-test relaxation was added.

Owner: Integration. Status: proposed; [draft PR #22](https://github.com/ketvietlab/ketplus-desktop/pull/22), dependent on #18.
No workflow/CMake/gitlink change is included in baseline PR #18.

Draft HEAD: `bfbdf95b8096ba5409c90bbc17133a28aca2cf4b`. Hosted
[run 34682605465](https://github.com/ketvietlab/ketplus-desktop/actions/runs/34682605465)
installed Node 24.14.1 and fetched the pinned plan revision successfully, then
failed with `MODULE_NOT_FOUND` for the Integration checker absent from develop.
This confirms fail-closed dependency handling, not a passing Node test result.

## First Registration

Create branch `integration-r00c-node-ci` from current `origin/develop`, adding
only `.github/workflows/design-evidence.yml`. The draft targets `develop`; its
Node job must fail if #18 files are absent. Do not copy #18 implementation onto
the CI branch or silently skip checks. After #18 merges, update the CI branch
from develop, re-review the diff, and require successful checks before readiness.

Use Node **24.14.1**, the version independently exercised for R00-C, through
`actions/setup-node@v4` with `check-latest: false`. Checkout uses the repository's
existing `actions/checkout@v4` convention and full Git history. Fetch exact
PR #17 source `37a14562a11b6ec9fb7f137e9e93c95af464bc8d` explicitly because squash
merge does not make that source revision an ancestor. There is no cached or
downloaded generated evidence, package installation or developer environment.

Always run from the checked-out repository root:

```sh
node tasks/desktop-refactor/wave-1/integration/check-integration.cjs
node --test tests/design/integration-evidence.test.cjs
node tasks/desktop-refactor/check-plan.cjs
git diff --exit-code
```

Use ordinary push/pull_request triggers, including documentation-only edits
because Markdown parity is checked. Restrict token permissions to contents:read.
Do not add conditional existence guards, continue-on-error, runtime Qt changes,
timeout increases, CMake registration, or submodule updates.

## Core Registration Included

#19 merged during R00-C: reviewed final source
`c739042143af4666f0d820b17ae36ed6b0531c5d`, squash merge
`0d789192596d01a387d9dc69efae1ec53de13b25`. The separate CI branch starts from
this develop revision and registers the following unconditionally. Integration
reran all 18 tests at the final source. No passing "optional Core tests" step.

```sh
node --test tests/fixtures/transcript-refactor/generate-transcript-workloads.test.mjs tests/fixtures/transcript-refactor/baseline-harness.test.mjs
```

These tests materialize workloads freshly in temporary directories. The legacy
daemon restore benchmark and native capture remain excluded from this lane;
they are not the production characterization adapter still owed by Core.

## Verification And Review

Local focused matrix: checker, evidence tests, plan checker, YAML syntax and
`git diff --check`. CI branch must contain only the workflow change. The existing
macOS/Ubuntu configure/build/full private CTest workflow remains intact and must
be reviewed for shared registration. No live/provider tests are enabled.
Missing #18 is a dependency BLOCKED result, not a flaky test. Existing passing
Qt runs and new Node runs retain separate revision/run IDs; historical failure
`34675651929` remains in [ci-triage-report.md](ci-triage-report.md).
