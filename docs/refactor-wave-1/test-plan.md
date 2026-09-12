# Test plan and evidence ledger

Wave 1 verifies existing public behavior with synthetic fixtures. Tests for APIs
that do not exist are planned only; parser coverage is not operation preflight or
filesystem-safety enforcement. Offscreen Qt execution is not visual or pixel QA.

## Added focused baseline

`tests/RefactorWave1ContractTest.cpp` uses temporary INI-format `QSettings`
stores owned by each test and synthetic buffered `SettingsPage` implementations.
It covers:

- Cancel calls no extension `apply()` and leaves persisted extension state
  unchanged;
- dialog-wide Reset changes the extension buffer but does not persist until
  Save;
- ThemeManager-backed dark preview rolls back through Cancel while the committed
  mode remains light;
- multiple registered pages keep registration order and current Save behavior
  calls `apply()` once on both dirty and clean pages;
- selected hard-coded light/dark `ThemePalette` values as a **current-native
  baseline**, without claiming KDS source parity;
- `git worktree list --porcelain -z` parser handling for current, branch, locked,
  detached, prunable, bare, omitted optional head/branch, unknown optional
  fields, and a malformed record lacking a worktree path.

The temporary settings files isolate extension persistence. The test entry point
also redirects default QSettings to temporary INI user/system paths before any
ThemeManager is created, isolating its persisted theme mode from user settings.

## Existing suites used as regression evidence

- `ketplus_settings_tests`: typography edits/reset, appearance normalization,
  existing ThemeManager preview cancellation, and a single extension dirty/Save
  case.
- `ketplus_git_tests`: status parser fields, worktree parser basics, and a
  disposable Git repository exercising discovery and read-only diff requests.
- The full registered CTest suite covers the other public targets when run.

## Planned, not currently tested

Settings-host proposal:

- invalid/duplicate/reserved built-in id results;
- descriptor/page mismatch, already-parented page, and same-pointer
  re-registration;
- QObject ownership, page/host destruction, and signal lifetime;
- ordering ties, unknown selection, and selection-before-registration rejection;
- explicit window-close/Escape baseline coverage (only Cancel is tested here);
- any approved dirty-only compatibility mode;
- a future explicit validation/apply result and extension preview rollback hook,
  if those interfaces are approved.

Native token proposal:

- a source fixture pinned to KDS revision, Git blob, and SHA-256;
- recursive alias, `light-dark()`, and alpha-colour resolution;
- canonical-vs-native drift assertions for both schemes;
- logical/fractional metrics, font-scale/density inputs, and rounding;
- common snapshot consumption by Widgets and a future QML adapter.

Linked-worktree proposal:

- Git common-dir/registered git-dir identity with linked worktrees outside the
  main worktree directory;
- symlink, filesystem case behavior, missing/path reuse/repository change,
  detached and unborn HEAD, bare/prunable registrations;
- per-operation blocker/warning mapping for open/select versus future mutation;
- request/factory ownership, thread affinity, correlation, timeout,
  cancellation, exactly-once completion, and revalidation failures.

The new focused test performs no Git mutation. The existing Git regression
creates branches, commits, and a linked worktree only inside its disposable
repository fixture. No parser test should be cited as proving preflight,
TOCTOU, or command-execution safety.

## Commands and evidence categories

```sh
cmake --preset dev
cmake --build --preset dev --target ketplus_refactor_wave_1_tests
ctest --preset dev -R ketplus_refactor_wave_1_tests --output-on-failure
ctest --preset dev -R 'ketplus_settings_tests|ketplus_git_tests' --output-on-failure
ctest --preset dev --output-on-failure
git diff --check
```

Final handoff must distinguish:

- tests run locally in this worktree;
- CI checks observed for the pushed commit;
- visual QA, which is not run or implied by offscreen tests.

## Local verification on 2026-09-12

On macOS arm64 with Qt 6.11.1, the focused target built and the focused,
settings, and Git suites passed (3/3). All registered CTest targets then passed
(9/9). An earlier sandbox run failed the existing Mermaid popup test; it passed
in the final unrestricted run without production changes. This establishes the
observed outcomes, not a diagnosis of the earlier failure.

Changed C++ was formatted using Xcode clang-format with the repository config;
`git diff --check` passed. Compiler caching was disabled in the local build
configuration because sccache could not run in the earlier sandbox.
CI for the new commit must be checked separately after push. Visual QA was not
run. Current parser coverage asserts `lockReason`; `GitWorktree` has no boolean
locked field and cannot distinguish a reasonless lock from an empty reason.
