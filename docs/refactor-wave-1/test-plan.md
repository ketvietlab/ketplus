# Test plan

Wave 1 test posture:

- Verify implemented public capabilities with generic synthetic fixtures.
- Do not add private product fixtures.
- Do not execute destructive worktree operations against real worktrees.
- Do not bless visual screenshot baselines without Integration/QA approval.

## Implemented synthetic harness

`tests/RefactorWave1ContractTest.cpp` provides an isolated contract harness for:

- settings extension pages with stable ids, local dirty state, Save/Cancel/Reset
  participation, and no private storage;
- public KDS token reference sanity through existing `ThemeManager` palette
  fields for light/dark snapshots;
- linked-worktree parser guard fixtures for locked, bare, prunable, detached, and
  current worktrees without mutating a repository.

These tests exercise current public behavior and proposed guard vocabulary. They
do not implement the proposed production APIs.

## Applicable public tests

Run from the public CM worktree:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
git diff --check
```

Optional focused runs:

```sh
ctest --preset dev -R ketplus_refactor_wave_1_tests --output-on-failure
ctest --preset dev -R ketplus_settings_tests --output-on-failure
ctest --preset dev -R ketplus_git_tests --output-on-failure
```

## Future tests before implementation approval

Settings host:

- duplicate page id rejection;
- select page by stable id before/after registration;
- extension page composition order;
- Save applies all dirty pages exactly once;
- Cancel restores built-in theme preview and leaves extension storage untouched;
- Reset updates buffered state without saving.

Token/metrics:

- light/dark token snapshot parity with KDS source revision;
- platform font fallback checks;
- reduced-motion effective state once animations exist;
- visual measurements only after QA-approved reference data is available.

Linked worktree operation:

- disposable repository fixtures for clean, dirty tracked, untracked, locked,
  bare, prunable, detached, current, and main worktrees;
- cancellation before execution;
- branch/head/path revalidation failure after preflight;
- no shell expansion in Git invocations;
- no checkout, delete, prune, branch creation, commit, rebase, or remote mutation
  in default public CM flows.
