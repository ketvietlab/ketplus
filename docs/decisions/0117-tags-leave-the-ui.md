# ADR 0117: Conversation tags leave the UI; the daemon keeps them

Status: accepted (Jay, 2026-11-11). Amends ADR 0107 and ADR 0113.

## Context

ADR 0107 stored a conversation's tags in the daemon, and ADR 0113 added the tag
catalog, **Manage tags…** and the `tags.changed` push. KétPlus Pro drew them in the
conversation header (tag chips, **Add tag**, the tag menu, click to filter), in the
sidebar rows and in the "Tagged <label>" filter above the repositories. The Qt mock
(`mockups/qt`, ADR 0033) drew the same.

The tags did not earn their place: they took header room from the title and the
Reviewer choice, and the conversation number, the repository and teams already
group the work. Jay decided that tags leave the UI and that the daemon stays as it is.

## Decision

- KétPlus Pro and the Qt mock draw no user tag: no chips in the conversation header,
  no **Add tag** or tag menu, no **Manage tags…**, no tag filter above the
  repositories, no tags in sidebar rows, hints, accessible descriptions, the compact
  window, search or menus.
- The code that only served the tag UI is deleted, not hidden behind a flag: the
  Desktop catalog defaults (`ConversationTags.h`), `AgentSessionTags.cpp`, both
  `ManageTagsDialog.qml` files, `SidebarTagFilterChip`, the tag filter of the
  sidebar, the header's tag signals and their wiring, and the client calls
  `session.tags.set`, `tags.catalog` and `tags.catalog.set`. The session manager
  no longer lists `tags` or `tagsEditable` with a conversation.
- A team member's role is not a user tag. It stays the locked role label in the
  conversation header (with its accessible description, opening the team, never
  editable) and the locked role tag of its row in the team's sidebar folder
  (ADR 0114). The sidebar draws only locked tags.
- The daemon is unchanged. `session.tags/1`, `session.tags.set`, `tags.catalog/1`,
  `tags.catalog`, `tags.catalog.set`, the `tags.changed` push and the sealed `tags`
  in the phone metadata keep working and keep their tests. `DaemonClient` still
  recognises `tags.changed` and drops it quietly: nothing in Desktop listens, and
  no error or warning is logged.

## Consequences

- Tags already stored stay in the journal and come back if a later client draws them.
  Another client can still change them; Desktop neither shows nor overwrites them.
- The phone still receives sealed `tags`. Whether the iOS app draws them is that
  app's decision.
- Bringing tags back is a UI change only: the daemon contract of ADR 0107 and 0113
  still holds.
