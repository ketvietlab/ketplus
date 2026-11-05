# Transcript shell / renderer implementation — updated 2026-09-14

Scope: private transcript only. No surrounding UI redesign, production cutover,
release packaging, or authenticated provider calls.

Implemented:

- Independent two-state scroll controller; content growth does not change state.
- Renderer family registry with generic fallback, exact command copy and bounded-detail notice.
- Keyboard activity expand/collapse; accessibility names/actions and token focus rings.
- File operation labels including create/delete/move destination.
- Browser operation labels for explicit browser tool identities; unrelated screenshot tools remain integrations.
- Activity images decode asynchronously at bounded thumbnail dimensions, initially 12;
  Show more adds 12; collapsed galleries instantiate no image delegates.
- Remote resolution/expiry closes pending questions and rejects later local submission.
  No answer is inferred when the daemon event contains only an approval ID.

Regression coverage:

Prior checkpoint: full dev build succeeded; private CTest **15/15 suites passed** (43.12 s).
Live provider tests stayed opt-in/skipped. Changed C++ files were clang-formatted;
`git diff --check` passed. Existing working branch and unrelated edits were preserved.

- `webSearchLabelsAndAnimation`: keyboard Left/Right, copy query, no raw JSON noise.
- `activityGalleryLoadsInBatches`: 50 images, 12/24 delegates, collapse to zero,
  decoded thumbnail width at most 224 pixels.
- `activityOperationSemantics`: browser start/complete operations and file create/delete/move.
- `answersAgentQuestionsSequentially`: immutable answer receipt and remote resolution/expiry rejection.
- Existing compact file/code, image preview, history, question and link tests retained.

Native check: `streamingPreservesReadingPosition`, Cocoa/Metal, four scenarios,
36 sampled frames each, maximum channel difference 1/1/2/2 (allowed 2/255).
Activity screenshot inspected at the initial checkpoint.
These synthetic checks do not establish live-session performance or full visual parity.

## Completed continuation

Final verification: full dev build succeeded; **15/15 private CTest suites passed**
in **67.41 s**. Agent suite: **67 passed, 0 failed, 1 opt-in live test skipped**.
Native gallery/resource tests additionally passed **4/4**. No authenticated calls.

- Kept one virtualized transcript ListView in `AgentWorkspace`, and extracted
  `TranscriptMessage` (message layout) plus `TranscriptActivityRow` (activities). Removed duplicate
  legacy command/file delegate branches. Scope/key viewport anchoring remains
  until the separate legacy-cleanup gate.
- Expansion/gallery/output-page state survives delegate recreation and scope
  switching, bounded to 256 entries. A user-collapsed failure stays collapsed on
  return; a newly failed activity still auto-expands.
- Output renders in 16 KiB increments. Evicted and oversized payloads rehydrate
  from the session only for matching scope/key/content hash. Cache stays within
  64 entries/4 MiB. Tests resolve 5 MiB without retaining it in the view cache,
  reject wrong scope/worktree/field/content and invalidate old references.
  No disk spool or second full-payload store was introduced. Transient parsing
  and total canonical history still require the load benchmark.
- Typed file read/code search/file search and browser operations retain semantic
  identity. Browser row uses safe title/domain; sanitized full URL stays in detail.
  Local browser screenshots remain attached to their activity. Failed MCP results
  get a failed status rather than a success label.
- Native gallery tests cover 50 images, 12→24→0→24 delegates, decode width ≤224,
  keyboard original-image preview and Escape dismissal. Broken images retain a
  placeholder. Fixed the image SVG source color for dark-mode tint.
- A second session manager replays the daemon journal's closed question and rejects
  resubmission. ID-only resolution says closed rather than inventing an answer.
- Artifact links open only on explicit click; executable schemes are rejected.
  Shared copy/export includes activity targets/questions and masks secret answers.
  Sanitizer preserves prose containing a URL instead of encoding it as a URI.

New regression coverage: `transcriptRendererInteractionMatrix` (light/dark,
failure/output paging/exact copy/state restore/QAccessible/fallback/broken image),
`transcriptArtifactSafetyAndExport`, `activityOperationSemantics`, extended
`transcriptModelDeduplicatesPagesAndBoundsPayloads`, `streamsAndKeepsSessionsByWorktree`
and daemon `answersAgentPromptThroughDaemon`. Existing lifecycle/retry/corpus,
question expiry, Markdown/link/code/image/Mermaid and copy tests remain in the suite.

Native Cocoa/Metal, macOS 26.2, Qt 6.11.1 arm64:

- Renderer matrix passed light/dark at scales 1, 1.25 and 2.
- Inspected collapsed command (dark/100%), integration (light/100%), browser detail
  (light/125%) and broken image (dark/200%); image icon retested after tint correction.
- Repeated pixel test **after shell extraction**: four scenarios ×36 frames,
  maximum channel difference **1/1/2/2**, within 2/255.
- Initial captures before settling, and one run without an exposed native window,
  were rejected. Harness now waits for exposure and transcript opacity. Successful
  reruns supersede those images; blank captures are not visual evidence.

These are standalone synthetic Agent Workspace test windows, **not screenshots
of the full Desktop shell**. They do not establish sidebar/header parity.

Wave 4 functional acceptance is complete. Wave 3 retains manual VoiceOver QA:
QAccessible role/name/action and keyboard tests are not a VoiceOver session.
Full-session restart, remote privacy, load/memory budgets, full visual corpus,
production seam/legacy removal and DMG smoke remain separate cutover gates.
No production readiness or new release is claimed.
