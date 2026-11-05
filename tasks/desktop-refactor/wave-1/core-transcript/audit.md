# Core/Transcript Audit

## Trace

1. Provider notifications enter daemon runtime in
   `apps/daemon/src/runtime/SessionHost.cpp:460`. Agent deltas become
   `assistant.delta`, command output becomes `command.output_delta`, item
   starts/completions carry provider-shaped `item` payloads, and turn lifecycle
   events are appended to the journal at `SessionHost.cpp:474`,
   `SessionHost.cpp:480`, `SessionHost.cpp:486`, and `SessionHost.cpp:504`.
2. `LocalJournal::appendEvent` stores the raw event and updates the compact
   projection in the same SQLite transaction
   (`apps/daemon/src/storage/LocalJournal.cpp:432`,
   `LocalJournal.cpp:483`). `updateReplayProjection` removes completed
   assistant deltas for the same item and omits command-output deltas from
   replay projection (`LocalJournal.cpp:332`, `LocalJournal.cpp:347`).
3. Replay IPC is exposed through `session.attach`/`session.events_after` and
   `session.transcript_page` in `SessionHost::handleCommand`
   (`apps/daemon/src/runtime/SessionHost.cpp:97`). `session.list` advertises
   replay event/byte bounds (`SessionHost.cpp:186`).
4. Desktop daemon facade restores sessions by listing daemon sessions, then
   serially calls `session.attach` with `compact_completed: true` until
   `last_event_cursor` (`modules/ai/src/agent/CodexAppServer.cpp:529`,
   `CodexAppServer.cpp:611`). Live events received during replay are deferred
   while `found->replaying` is true (`CodexAppServer.cpp:680`).
5. The legacy/presenter history path uses `thread/resume` with
   `excludeTurns: true`, then `thread/turns/list` with `limit: 20` and
   `itemsView: full` (`modules/ai/src/agent/AgentSessionHistory.cpp:40`,
   `AgentSessionHistory.cpp:99`). The daemon facade currently returns empty
   data for `thread/turns/list` (`modules/ai/src/agent/CodexAppServer.cpp:422`).
6. Provider-shaped turns are converted to display rows by
   `historyMessages` (`modules/ai/src/agent/AgentMessageFormatter.cpp:165`).
   Live items are keyed through `messageIndexByItem` and updated by item ID
   (`modules/ai/src/agent/AgentSessionNotifications.cpp:217`).
7. `AgentMessageModel` is incremental for append/prepend/set, but stores the
   full in-memory `sourceItems_` and normalized `items_`; it has no byte or
   item-window eviction roles (`modules/ai/src/agent/AgentMessageModel.cpp:74`,
   `AgentMessageModel.cpp:96`, `AgentMessageModel.cpp:109`).
8. Rendering uses one QML `TextEdit` per assistant message with
   `TextEdit.MarkdownText`; every text/width/theme change calls
   `agentHost.formatMessageDocument` on the whole document
   (`modules/ai/src/agent/qml/AgentWorkspace.qml:1216`,
   `AgentWorkspace.qml:1256`).
9. Scroll anchors use message key plus optional text position/sample and offset
   (`modules/ai/src/agent/qml/TranscriptPosition.js:5`). This is stable for
   prepends/reflow in the current retained model, but there is no eviction/newest
   continuation model yet.

## Observed Failures / Gaps

- `session.transcript_page` is bounded but not lossless for a large turn. It
  selects whole turn sequences, then stops when `maximumReplayEvents` or
  `maximumReplayPayloadBytes` is reached and only returns `truncated: true`
  (`apps/daemon/src/storage/LocalJournal.cpp:586`,
  `LocalJournal.cpp:663`). `TranscriptPage` has no continuation token,
  covered range, item fragment marker, or byte accounting
  (`apps/daemon/src/storage/LocalJournal.h:29`).
- Desktop restore from daemon is still full compact-projection replay, not
  initial newest-3-turn attach. `CodexAppServer::attachDaemonSession` loops
  until replay cursor reaches daemon `last_event_cursor`
  (`modules/ai/src/agent/CodexAppServer.cpp:611`,
  `CodexAppServer.cpp:671`). This avoids raw full-history replay, but it is not
  the R05 windowed model.
- Client history path still requests 20 turns, not 3, for `thread/turns/list`
  (`modules/ai/src/agent/AgentSessionHistory.cpp:101`). The daemon facade
  returns an empty page for that RPC (`modules/ai/src/agent/CodexAppServer.cpp:422`),
  so a daemon-backed selected conversation relies on compact attach replay.
- Snapshot cursor is exposed as page metadata (`SessionHost.cpp:250`) and
  backend returns `snapshotCursor: session.acceptedEventCursor`
  (`origin/integration:apps/ketsuite/modules/ketplus_control/src/relay-functions.ts:1639`).
  Neither schema has coverage semantics that prove the visible history window is
  complete when `truncated` is true.
- Backend `readTranscriptPage` on `origin/integration` selects all retained
  `SessionEvent` rows for the session/generation, filters/sorts in memory, then
  slices to the newest turns (`relay-functions.ts:1592`). It limits event count
  but not encoded page bytes and has no in-turn continuation
  (`relay-functions.ts:1621`).
- `messageIndexByItem` is an in-memory row index map, cleared on new turn
  (`modules/ai/src/agent/AgentSessionTurns.cpp:131`). It handles live item
  updates but does not encode canonical order/revision or protect page/live
  conflict resolution across a bounded window.
- Streaming assistant deltas append to the complete body string on every delta
  (`modules/ai/src/agent/AgentSessionState.cpp:54`). QML reparses/layouts the
  full Markdown document on `onTextChanged` (`AgentWorkspace.qml:1273`).
  This is the documented giant-message cost path, not a virtualization problem.
- The active/inactive Desktop transcript model has no source-byte budget or
  eviction. Warm QML views are cached, but `AgentMessageModel` retains all
  loaded items in two lists (`AgentMessageModel.cpp:74`).
- Command output stripping is implemented in daemon projection
  (`LocalJournal.cpp:347`) and backend mobile projection
  (`origin/integration:apps/ketsuite/modules/ketplus_control/src/protocol.ts:225`),
  but there was no shared synthetic marker fixture before this slice.

## Suspected Risks

- `compactedEventsAfter` advances `nextCursor` to session tail when a compact
  page has fewer than the requested rows (`LocalJournal.cpp:571`). This is
  correct for compact replay cursors, but downstream consumers must not confuse
  raw cursor contiguity with rendered-history coverage.
- Live deferred events are replayed from a `QMap` keyed by cursor after compact
  replay (`CodexAppServer.cpp:710`). This orders live events, but a future
  transcript-page path will need explicit page/live coverage merge rules so old
  page snapshots cannot overwrite live item bodies.
- `historyMessages` de-duplicates by turn ID only
  (`AgentMessageFormatter.cpp:170`). That is adequate for current Codex pages,
  but multi-assistant/tool item revisions in a canonical transcript need item
  IDs plus revision/order keys.
- Existing anchor capture is key-based and text-sample aware, but eviction,
  newest fetch after tail eviction, and giant pinned selection are not modeled
  (`TranscriptPosition.js:12`).

## Mobile Production Trace

Evidence revision: `ketplus-app origin/develop`
`fdd84d4fcdf94fd5e5427ca2ee8a99530a25f70f`. Mobile source was read-only.

Production path:

1. `RootView` owns the production `RemoteAPIClient`, restores credentials in
   `.task { await restoreOnce() }`, and on signed-in state constructs
   `LiveRemoteView(client:authenticatedSubject:displayName:)`
   (`apps/ios/Sources/KetPlusRemoteFeature/RootView.swift:17`,
   `RootView.swift:33`, `RootView.swift:44`, `RootView.swift:69`).
2. `LiveRemoteView` uses `LiveControlStore`, an `@MainActor` observable store
   with published session, event, transcript, cursor, generation, and
   selection state (`LiveRemoteView.swift:10`, `LiveRemoteView.swift:31`,
   `LiveRemoteView.swift:34`, `LiveRemoteView.swift:36`,
   `LiveRemoteView.swift:42`).
3. Network calls go through `HTTPControlPlaneClient`, a Swift actor conforming
   to `RemoteAPIClient`; encrypted transcript page and live event DTOs include
   session ID, generation, cursor, snapshot cursor, and events
   (`apps/ios/Sources/KetPlusRemoteCore/HTTPControlPlaneClient.swift:97`,
   `HTTPControlPlaneClient.swift:125`, `HTTPControlPlaneClient.swift:144`,
   `HTTPControlPlaneClient.swift:191`).
4. The live transcript projection is built by `LiveTranscriptProjection` in
   `LiveTranscriptView.swift`; `LiveControlStore.merge` feeds retained events
   into that projection (`LiveTranscriptView.swift:96`,
   `LiveRemoteView.swift:287`).
5. Mobile cache uses `EncryptedSQLiteTranscriptStore`, also `@MainActor`, and
   performs JSON encode/decode, AES-GCM seal/open, SQLite prepare/step, and
   transaction work on the main actor (`SQLiteTranscriptStore.swift:7`,
   `SQLiteTranscriptStore.swift:144`, `SQLiteTranscriptStore.swift:164`,
   `SQLiteTranscriptStore.swift:170`, `SQLiteTranscriptStore.swift:204`,
   `SQLiteTranscriptStore.swift:221`).

Preview path is different: `ReconnectPreviewView` is `#if DEBUG`, uses
`ReconnectPreviewModel`, `FakeRemoteSessionTransport`, synthetic sessions, and
`RemoteSessionConnection` (`ReconnectPreviewView.swift:1`,
`ReconnectPreviewView.swift:145`, `ReconnectPreviewView.swift:149`,
`ReconnectPreviewView.swift:176`). It is useful regression context but is not
production evidence for `RootView -> LiveRemoteView -> HTTPControlPlaneClient`.

Observed mobile findings:

- `loadInitialTranscript(for:force:)` performs a post-await guard on
  `selectedSessionID` and `selectedGeneration` before replacing events
  (`LiveRemoteView.swift:257`, `LiveRemoteView.swift:268`).
- `loadOlder()` captures `selectedSession` before the await, but after
  `client.transcript(...)` it calls `merge(page.events)`, updates paging state,
  clears error, and persists without rechecking selected session, generation,
  or a selection epoch (`LiveRemoteView.swift:152`, `LiveRemoteView.swift:159`,
  `LiveRemoteView.swift:165`, `LiveRemoteView.swift:170`).
- `refresh()` lists sessions, may select/adopt a session, then when
  `initialSnapshotReady` awaits `client.events(...)`; after that await it
  merges events, advances cursor, and persists without a post-await
  session/generation/epoch guard (`LiveRemoteView.swift:204`,
  `LiveRemoteView.swift:225`, `LiveRemoteView.swift:231`,
  `LiveRemoteView.swift:234`).
- Error/defer paths can still update shared store state after async
  suspension. Examples: `loadOlder()` `defer { loadingOlder = false }` and
  catch state updates, `loadInitialTranscript` catch state updates, and
  submit/stop/approval catch updates (`LiveRemoteView.swift:156`,
  `LiveRemoteView.swift:171`, `LiveRemoteView.swift:280`,
  `LiveRemoteView.swift:124`, `LiveRemoteView.swift:147`,
  `LiveRemoteView.swift:199`). MainActor serialization does not by itself
  prove these updates still belong to the same selection after an `await`.
- `assistantIndexes` is keyed by `turnSequence` (`[Int64: Int]`), not item ID.
  `assistant.delta` and `agentMessage` both update the entry for that turn, so
  multiple assistant items in one turn can collapse into one assistant row
  (`LiveTranscriptView.swift:96`, `LiveTranscriptView.swift:99`,
  `LiveTranscriptView.swift:123`, `LiveTranscriptView.swift:139`).
- Command output deltas are removed from the mobile retained/projection path,
  while command item fields such as command/status/exitCode are retained
  (`LiveTranscriptView.swift:67`, `LiveTranscriptView.swift:74`,
  `LiveTranscriptView.swift:79`, `LiveTranscriptView.swift:155`,
  `LiveTranscriptView.swift:187`).
- SQLite cache validation checks session ID, generation, workspace, increasing
  cursor, and cursor <= accepted cursor; it does not validate canonical item
  identity/order/revision semantics (`SQLiteTranscriptStore.swift:287`).

Suspected mobile risks:

- A late `loadOlder()` or live events page can merge into the newly selected
  transcript if selection changed during the await. This is suspected from the
  absence of a post-await guard; it was not reproduced on device in this slice.
- `transcriptError` and `connection` can reflect an older request after the
  user moved to another selection. This is suspected from catch paths without
  operation scoping.
- MainActor reduces data-race hazards but does not remove async logical races:
  other main-actor tasks can run while a function is suspended at `await`.

Not verified for mobile:

- Physical-device frame/RSS/cache cost under 1k/10k/giant workloads.
- Visual anchor preservation after `loadOlder()` when selection changes or when
  large pages arrive late.
- Production C++/Swift cloud projection parity for command output stripping.

## Not Yet Verified

- Native viewport/frame evidence was not captured in this audit turn. No
  alternate browser, OS automation, or offscreen FPS bypass was used.
- Release performance metrics for the new 1k/10k/giant/page-overlap workloads
  were not run because this slice only adds workload generators and fail-closed
  baseline harness. Existing daemon restore benchmark remains available but is
  not tied to those fixtures.
- iOS physical-device performance, MainActor cache cost under large transcript
  payloads, and loadOlder visual anchoring were not measured.

## Backend / Mobile Compatibility Notes

- Backend `origin/integration` command kinds remain
  `session.start_turn`, `session.steer`, `session.interrupt`, and
  `approval.resolve`
  (`origin/integration:apps/ketsuite/modules/ketplus_control/src/types.ts:39`).
  There is no observe-only history request/response mailbox command kind yet.
- Backend appends relay events with a strict cursor gap check and updates
  `acceptedEventCursor` in transaction
  (`origin/integration:apps/ketsuite/modules/ketplus_control/src/relay-functions.ts:1438`).
  This is a transport cursor, not page completeness.
- Mobile production live path is `RootView -> LiveRemoteView ->
  HTTPControlPlaneClient/EncryptedSQLiteTranscriptStore ->
  LiveTranscriptProjection`, not `RemoteSessionConnection`.
- `RemoteSessionConnection` remains relevant to the debug reconnect preview
  path, where epoch checks exist, but those checks do not fence the production
  `LiveControlStore.loadOlder()` or `refresh()` page/live merge paths.
