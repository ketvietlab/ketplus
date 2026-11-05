# PR #16 design review resolution

Date: 2026-09-12. Base design revision: `3abe5c4`.

## Delivered in the prototype

| Finding | Resolution |
| --- | --- |
| D01 / P1 | Scoped conversation registry, independent new IDs, explicit worktree routes, per-scope activity |
| D02 / P1 | Draft and attachment metadata survive dialogs/navigation; acknowledgment clears accepted input only |
| D03 / P1 | Separate turn/connection/availability; sending/steering/stopping/reconnecting/answer pending and failure states; expired/private/multi-client questions |
| D04 / P1 | History journey: earlier-page loading/error, in-turn continuation, retention/Mac availability, Latest, command lifecycle and lazy rich preview |
| D05 / P1 | Single Sign in entry, username/password, retained auth state, revoke every paired device, cancellation/expiry/error states and hybrid disclosure |
| D06 / P2 | Stable bundle identity through library/builder/preview; independent draft Atlas; missing/invalid and unvalidated preview states |
| D07 / P2 | Flow tag explicitly describes preferred context, not guaranteed isolation; last-valid preview independent of Agent completion |
| Additional decisions | Connections uses Lucide Unplug; async worktree creation; Archive/Delete are separate operations; restore through Settings → Archived |

Archive retains files/history and does not stop Agents. Delete requires typed confirmation,
blocks active turns, and blocks dirty/main worktrees. Worktree deletion retains its branch
and archives conversations. Settings restoration retains ownership and never reruns a turn.
Failed operations do not remove the item. See `FLOW-CONTRACT.md` for native dependencies.

## Verification

- 10 reusable screens, 10 workflows, 115 nodes and 152 edges.
- Strict KetAtlas audit: no errors/warnings; local resources only.
- 131 Node/jsdom tests pass: 108 addressable screen states and 23 focused model/DOM cases.
- JavaScript syntax checks pass. Existing Két materialized CSS/runtime/contracts retain
  their recorded SHA-256 hashes. Added action icons use official Lucide Static 1.40.0 assets.
- `git diff --check` passes. No native source or public submodule changes.

These are mock interaction checks, not browser pixel tests, native performance measurements
or production API verification. Browser access was denied; no alternate browser or permission
workaround was used. Actual **Try this screen**, keyboard/focus, viewport and native acceptance
remain outstanding. Native delivery is intentionally not marked Implemented/Verified in the
progress sidecar.
