# ADR 0128: Authorize each Atlas renderer launch

Date: 2026-11-16. Status: accepted policy; implementation requires its own review.

## Context

An Atlas may contain `atlas.renderer.json`, whose command runs with the local
user's authority. Opening a linked or discovered bundle does not itself authorize
that program. The PS-01 audit identified implicit renderer opt-in in managed
browser viewers. The user selected per-launch consent (team decision D-2,
corresponding to D-01 in the audit disposition plan).

## Decision

Use the shared managed-viewer launch boundary for the Atlas list, transcript
result cards and CLI-install retry. For each new renderer process, show a Két
dialog with the resolved executable, argument boundaries, working directory and
contract location. Treat all project text as plain text, escaping control and
direction-format characters in command details. Focus View static by default.

Run once authorizes only that launch. View static, Escape and closing the dialog
open without `--renderer`. No prompt or verifiable command means static viewing;
unreadable, oversized or invalid command data cannot grant execution. Stop also
revokes a pending request, without launching a replacement viewer. Destroying its
window abandons the request. There is no trust setting or saved grant to reuse
after stopping, reopening or restarting. Focusing an already-running viewer does
not create a new renderer process.

Keep a bounded in-memory snapshot of manifest/sidecar bytes, canonical paths,
resolved cwd/executable and expanded arguments for the pending launch. Read only
regular files validated on the opened descriptor; Unix opens are nonblocking so
a FIFO cannot stall consent or static fallback. Reject devices, directories and
oversized inputs before reading. Reserve
the viewer and renderer ports while the dialog is open, then release them for
the CLI. Recheck the snapshot immediately before starting; changed contracts,
symlink destinations or cwd/executable resolution cannot inherit approval.
Changed renderer data falls back to static viewing with an explanatory error;
an invalid or moved bundle path is rejected. There is no silent second attempt
with renderer opt-in.

## Consequences and limits

Each stopped/restarted renderer needs another user decision. Static viewing may
not reproduce screens that require a running framework; it remains available
without executing project commands. Selected-path validation is independent of
origin filtering and does not make the renderer a sandbox.

The CLI still reads project files after process creation. This check does not
freeze executable/script/dependency contents or defend against arbitrary changes
by another process with the same OS user's authority after the final check. The
permission authorizes a project command, not a cryptographic snapshot of every
file it might execute. Executable provenance and OS process containment remain
separate concerns. Never describe these checks as full local-process isolation.

## Verification

`ketplus_managed_atlas_viewers_tests` uses a harmless CLI fixture and an intercepted
URL handler; it checks renderer opt-in, static fallback, changed contracts/paths,
pending Stop, and fresh consent on reopen/recreated registry. Watchdog-isolated
FIFO/device/directory regressions include replacement while consent is pending
and verify static fallback without renderer execution. UI checks exercise live
light/dark/system theme and 90/100/110/125 percent zoom at narrow/wide native
widget sizes. `ketplus_atlas_workspace_tests` checks that both desktop entry
points reach the permission dialog before launching the CLI.

These fixtures do not run an actual project renderer or browser and do not prove
packaging, installed-bundle behavior, or OS containment. Native capture evidence
and the applicable repository test matrix must be recorded by the implementation
PR; this ADR does not certify those results.
