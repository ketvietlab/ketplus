# ADR 0080: Pin workflow actions and limit checkout credentials

Status: accepted for direct workflow action references and GITHUB_TOKEN defaults;
hosted acceptance is partial; transitive dependency and enforcement audits remain open.

The build, promotion and WebRTC workflows used movable action references. Build
and WebRTC also inherited repository token permissions, while checkout left its
authentication material available to later build/test steps by default.

Resolve each existing reference through the upstream repository's GitHub API and
pin its full commit SHA. Preserve the old reference in a comment for maintenance.
The Qt action's `v4` is an upstream branch, not a tag; it is resolved and pinned
the same way. This freezes the existing reference at the verified commit without
selecting a different action major version. Record the eight mappings in
`tasks/backend-hardening/evidence/h6-workflow-action-pins.json`.

Give all three workflows explicit `contents: read` permissions and set
`persist-credentials: false` on all six checkout steps. These public-source jobs
do not push commits or publish releases. Promotion's later integration fetch can
read the public repository without retaining the checkout token. Keep workflow
events, jobs, matrices, commands, timeouts, checkout revisions and branch
provenance checks unchanged.

Validation parses all three YAML documents and compares their complete structure
against the previous versions with only the intended pin/permission/checkout
changes. All twenty action occurrences are full SHAs verified in their upstream
repository; all six checkouts disable persistence. The existing promotion-policy
regression suite passes. No application implementation, dependency versions or UI behavior changes.

A hosted browser run exposed an existing assertion that assumed CMake diagnostics
never wrap. The stale-runtime guard correctly failed, but the diagnostic split
`from` and `sources` across lines. Match whitespace between the same required
words; preserve the failure-status, allow-stale and restaging checks. The original
probe fails with a short temporary path; the complete source-digest suite now
passes 3/3 with both short and platform-native temporary paths. Hosted acceptance
requires a fresh run of the corrected head.

This does not pin runner images, OS packages, action-internal downloads or every
transitive build dependency, nor prove hosted runner compatibility. Cache trust,
fork isolation and full release provenance remain H6 work. Actual GitHub jobs
are verified on the existing promotion/release lanes; do not enable CI on
integration or claim local checks exercised hosted actions.

Hosted execution at `5fa91c0d` passed browser, Lite macOS/Ubuntu, Pro macOS
(96/96), and Linux daemon (48/48 plus install staging, TGZ and clean-container
runtime). Windows failed Git/navigation fixture targets and the tooltip/theme
target. PR #268 fixes the filesystem fixtures independently; UI acceptance
remains outside this backend-only workstream. These jobs cover the integration
base plus this workflow change, not the other pending hardening branches.

The read-only GitHub audit on 2026-11-07 returned no repository rulesets and
`develop.protected=false`; classic protection returned `Branch not protected`.
Promotion currently executes its policy script and tests from the PR head. A
passing local script does not establish an immutable mandatory promotion gate.
Repository administrators must decide and verify enforcement; this change does
not silently change repository settings or adopt privileged workflow triggers.
