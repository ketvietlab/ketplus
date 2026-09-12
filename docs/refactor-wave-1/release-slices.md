# Release slices

These slices keep implementation reviewable and prevent private product UI from
driving public API shape.

## Slice 0: Audit and proposal

Status: implemented in Wave 1.

- Document current `KetPlusCM::*` targets and public headers.
- Map public KDS semantic tokens to Qt palette/QSS/native metrics.
- Document settings host and linked-worktree operation proposals.
- Add generic synthetic tests that do not change product behavior.

## Slice 1: Native design token snapshots

Proposed.

- Add a public native token snapshot type.
- Expose token source version/revision in docs or API metadata.
- Keep `ThemePalette` compatibility.
- Add unit tests for light/dark token mapping.
- Do not add screenshot baselines until Integration/QA supplies approved
  measurements.

## Slice 2: Settings host stabilization

Proposed.

- Introduce `SettingsPageDescriptor` and registration validation.
- Add stable page selection by id.
- Preserve `SettingsDialog::addPage(SettingsPage*)` compatibility.
- Add duplicate id, ordering, Save/Cancel/Reset, and theme-preview tests.
- Avoid private page ids or private storage contracts.

## Slice 3: Linked-worktree preflight

Proposed.

- Add public target identity, preflight result, guard vocabulary, and async
  outcome types.
- Implement read-only preflight against disposable repository fixtures.
- Keep existing switch-worktree behavior unless a clean target passes
  revalidation.
- Do not add destructive worktree deletion, pruning, checkout, branch creation,
  commit, rebase, or remote operations.

## Slice 4: Build/install SDK hardening

Proposed.

- Decide whether public headers move to `include/ketplus/...` or remain build
  tree only.
- Add generated CMake package exports if installable SDK support is approved.
- Document source/binary compatibility expectations per `cm-v*` release.

## Release gate

Private teams may depend on a new implementation only after:

- public API is approved;
- public CM release/tag is cut from `main`;
- private integration pins the exact public release through the integration gate;
- no private source/spec/screenshot/transcript/credential has entered public CM.
