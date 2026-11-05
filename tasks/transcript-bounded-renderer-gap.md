# Transcript render scheduling and remaining native virtualization gap

Status: giant-message block virtualization and bounded deferred materialization
are implemented. Native visual/performance evidence and cross-block selection
remain open. Turn Navigator is separately deferred in
[its debt entry](transcript-turn-navigator-debt.md).

## Implemented in the performance worktree

`AgentWorkspace.qml` retains an already measured assistant body while its cached
delegate is outside the active viewport plus a 128-pixel prewarm margin. Body
changes still reach the message model, but that offscreen `TextEdit` does not
reparse each intermediate source. Entering the viewport applies the newest source
and newest Mermaid revision, without replaying the intermediate prefixes.

- A newly created or reused row still receives its initial full measurement.
  Empty placeholders or arbitrary estimated heights would break the existing
  `ListView`, reading-anchor, and Latest behavior.
- Existing offscreen geometry is retained, not collapsed or truncated.
- Selected text is excluded from this scheduling deferral. This does not add a
  new selection model or change the existing whole-document selection semantics.
- Hidden warm conversations defer Markdown/media refresh and custom paragraph
  formatting until activation. Font property changes can still cause Qt layout;
  this is not a claim that every hidden theme-change cost has been eliminated.
- The implementation keeps the existing Qt Markdown document, typography, links,
  images, code frames, Mermaid overlays, and text-position scroll anchor APIs.
- Command/file line extraction and user-bubble `TextMetrics` do not process an
  assistant body; these previously did unnecessary whole-source work in every
  assistant delegate.
- A bounded history item with `partialBody=true` is shown as literal text, with
  no Markdown or Mermaid interpretation of a suffix lacking its prefix. Clearing
  the flag re-enables the normal rich path. The explicit incomplete-history
  indicator is owned by the history/window UI, not by this scheduling policy.

This earlier scheduling layer still bounds repeated work on already measured
offscreen documents. The later production layer additionally splits giant source
items into stable 12 KiB target / 16 KiB maximum render rows off the GUI thread,
publishes the tail first, and incrementally prepends older blocks. Visible rich
documents are therefore bounded by block size rather than the complete source.

## Reading position while streaming

A regression reproduced a logical text-anchor jump when a link-reference
definition arrived at completion while the user was reading the middle of that
same assistant message. Qt reinterpreted earlier Markdown; the old rendered text
position no longer matched, so the previous code fell back to a row offset.
Keeping that row offset did not keep the paragraph on screen.

The implemented policy latches the displayed source of the **anchored live
assistant message** when tail-following is off. Canonical/session/model data still
receives every update; the pinned `TextEdit` does not reparse new source or media
revisions. The pin stays through completion and is released by Latest or leaving
the message's reading anchor. This is an explicit reading-stability policy, not a
claim that the old Markdown renderer is incremental. Font/width changes still use
the existing same-source text-position anchor; unrelated visible rows are not
frozen.

An explicit older-prefix history load is exempt from the pin, so loading earlier
text is not silently ignored. A presentation-only cumulative
`bodyPrefixCharacters` counter identifies that operation independently of live
appends. Literal partial bodies adjust the text position by that prefix delta.
When the newly complete body becomes Markdown, a unique exact sample is searched
within at most 16 Ki UTF-16 code units around the estimated new position.

Ambiguous repeated samples, a changed literal sample, or a match outside that
bounded range deliberately fall back to the row anchor. This remaining semantic
anchor limitation requires the source-to-render position map of the full future
renderer; it must not be described as pixel-perfect history-prefix anchoring for
arbitrary Markdown.

## Why a split at paragraph boundaries is not a complete replacement

The current renderer's public Qt Markdown entry point builds a whole
`QTextDocument`. Existing production behavior also uses that document for:

1. Tables, nested lists, reference links, inline formatting, and text offsets.
2. Local image scaling, Mermaid image/placeholder placement, and code frames.
3. Selection/copy across paragraphs and source-validated reading anchors.

There is no exported incremental Markdown AST/parser contract in the current
private dependencies. Splitting on blank lines changes Markdown interpretation;
using a `Repeater` over every block or line does not virtualize it; one large
`TextEdit` per giant block retains the original problem. Parsing the entire source
on a worker for every delta can reduce GUI blocking but is not incremental work.

## Implemented renderer contract and remaining gaps

| Area | Required capability | Current gap |
| --- | --- | --- |
| Parsing | Markdown-aware safe boundaries; unsafe partial fragments render literally | Full incremental AST/dependency invalidation remains open |
| Render units | Stable block rows, 12 KiB target and 16 KiB maximum | Cross-block semantic layout is intentionally not fabricated |
| Selection | Exact canonical copy/export independent of rendered rows | Native cross-block selection and accessibility range remain open |
| Geometry | Estimated rows, tail-first publication, persistent semantic scroll anchor | Native Metal reflow evidence remains open |
| Media | Visible block resources keep image/Mermaid lifecycle and open actions | Cross-block references can require literal fallback |
| Memory | 300-message / 16 MiB active target plus delegate/document block bounds | Physical-device process+GPU evidence remains open |

The implemented rows are production wired and covered by deterministic tests.
The remaining entries above must not be claimed complete from offscreen evidence.

## Deferred-work bound

Direct giant-message streaming uses exactly one active worker plus one replaceable
pending batch. The pending batch stores the latest authoritative source per row,
including rows invalidated while the active batch is running. Synthetic 60-second
runs delivered 3,000 updates at 50 Hz and 6,000 at 100 Hz with a maximum
outstanding count of two, lossless terminal text, and zero model resets. These
are offscreen Release measurements, not native FPS or pixel approval.

## Verification

Focused deterministic tests added to `AgentTranscriptViewportTest.cpp`:

- `offscreenMarkdownKeepsGeometryAndFlushesNewestSource`: 100 authoritative body
  updates produce zero `QTextDocument::contentsChanged` signals on a retained
  offscreen assistant; geometry stays unchanged; reentry renders the latest full
  source; selection and Latest remain available.
- `hiddenWarmMarkdownDefersMediaAndFormatUntilActivation`: hidden warm source,
  Mermaid revision, and custom format work are deferred; activation catches up
  to the newest revision and correctly renders a Markdown link.
- `partialBodyIsLiteralUntilItsPrefixIsAvailable`: a suffix remains literal until
  the flag changes, then its link and fenced code use normal Markdown rendering.
- `assistantRowsDoNotComputeCommandFileOrUserMetrics`: a large assistant creates
  no unused command/file arrays or user-bubble width measurement.
- `anchorRelocationIsBoundedAndRejectsAmbiguousMatches`: exact large-prefix
  relocation, live append distinction, Markdown transition, duplicate rejection,
  and bounded search even when the match is outside the local search region.
- `sameItemHistoryPrefixKeepsVisibleTextAnchor`: a prefix load while streaming,
  additional live text, and complete-source Markdown promotion keep a unique
  visible paragraph anchored.
- `middleReaderRemainsStableThroughStreamAndCompletion`: reading inside the
  streaming body, reading an older message, and holding/releasing the scrollbar
  all keep the logical reading paragraph fixed; new source and completion remain
  in the model; Latest displays the newest complete source.

Run with a Release test build and `QT_QPA_PLATFORM=offscreen`. The measurements
above are deterministic document-update counts, **not native FPS, RSS, or a
pixel-perfect visual approval**. Native screenshot validation remains outstanding
and must respect the user's existing capture-permission boundary.
