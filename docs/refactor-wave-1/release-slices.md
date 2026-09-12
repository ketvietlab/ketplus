# Review and release slices

This plan separates verified evidence from proposed contracts and runtime work.
No proposal is approved merely by landing this documentation slice.

## A. Source and baseline verified in R01-B

- KDS package/revision/source file and content hashes are recorded.
- `--kv-text-xl` is 18px and `--kv-text-2xl` is 22px at the 16px root.
- Canonical source values, native observations, adaptations, unresolved drift,
  and QA approval are distinct evidence classes.
- Focused current SettingsDialog behavior is covered with isolated synthetic
  persistence: Cancel, Reset-before-Save, ThemeManager preview rollback, and
  apply-all across multiple pages.
- Current palette values and parser flags are baseline observations only.

## B. API proposal pending approval

- Native source-pinned token snapshot and resolver contract.
- Generic settings registration results, identity, ordering, ownership,
  selection, and compatibility semantics.
- Linked-worktree observation, per-operation policy, Git identity, request,
  lifecycle, cancellation, timeout, and terminal outcome contracts.

Approval must explicitly settle the remaining choices listed in the handoff;
documentation merge alone does not settle them.

## C. Native visual measurements pending QA

- Cross-platform typography/font metrics and accessibility scaling.
- Control, navigation, table, padding, radius, focus, and dense-view adaptations.
- Light/dark drift decisions and any pixel/screenshot references.

There is no screenshot evidence or pixel-perfect gate in this slice. Offscreen
behavioral tests do not satisfy this category.

## D. Runtime implementation not started

No production token resolver, settings host, or worktree operation API is added
here. Default behavior stays read-only, and no delete, checkout, prune, branch
creation, commit, rebase, or remote mutation is introduced.

## Smallest next runtime slice

Implement **settings registration validation only**, while preserving current
Save/apply-all and dialog-wide Reset behavior.

Allowed files:

- `src/app/SettingsDialog.h`
- `src/app/SettingsDialog.cpp`
- `tests/SettingsDialogTest.cpp` or a single focused settings-host test file
- minimal `CMakeLists.txt` registration only if a new test target is used
- `docs/refactor-wave-1/**` for accepted-contract updates

Required tests:

- valid registration and descriptor source-of-truth;
- invalid/duplicate id, same pointer, null, and already-parented failure results;
- stable order ties;
- unknown and pre-registration selection rejection;
- QObject ownership/destruction and signal disconnection;
- compatibility adapter retains existing constructors/signals, apply-all Save,
  dialog-wide buffered Reset, Cancel, and ThemeManager preview rollback.

Dependencies: Qt Core/Widgets/Test and existing `KetPlusCM::settings_ui` only.
No token resolver or Git operation dependency should enter this slice.

## Later independent slices

1. Source-pinned native token resolver plus mapping fixture (no visual claims).
2. Read-only linked-worktree observation/preflight using disposable repositories
   and Git common-dir identity; open/select policy only after API approval.
3. Native visual adaptation after Integration/QA supplies approved evidence.
4. Optional installable SDK hardening if install/export support is requested.

Installable SDK work is not a blocker for current build-tree consumers of
exported `KetPlusCM::*` targets.

## Release gate

A runtime consumer may pin a new implementation only after its public API slice
is approved, implemented and tested; the relevant visual gate is satisfied where
applicable; a public release is cut through the normal process; and the consumer
pins that exact release. This task does not merge, tag, release, deploy, or
upgrade any external gitlink.
