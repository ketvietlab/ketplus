# Stable sidebar placement

Status: accepted

## Context

Execution repositories and worktrees change during a conversation or a team's tasks
(ADR 0123). Using that mutable context as navigation ownership moves sidebar rows,
including team members, while the user is reading them. A directory called
`repository` is not evidence of either a real repository or a placeholder.

## Decision

- A new daemon conversation captures its repository identity at creation. Repository
  discovery reads Git directory metadata, including a linked checkout's `commondir`,
  without launching Git on the request path. Confirmed absence of Git metadata is
  known no-repository; unreadable or malformed metadata remains unknown.
- `conversation_navigation.repository_path` is nullable and written with the first
  navigation row for a session generation. SQL NULL means legacy/unknown. A non-NULL
  empty string means known no-repository. Execution context updates cannot replace
  this row. A new generation has its own navigation identity.
- A team stores optional `sidebar_repository`, independent of its mutable
  `repository`. Creation stamps it on the host. Existing teams use their persisted
  repository as a fallback, frozen before their next settings/task mutation. This
  does not claim to reconstruct their original creation repository. Member rows
  remain under the team's placement regardless of task execution scope.
- Legacy conversations keep their persisted navigation path and any recorded
  repository mapping for it as a fallback. They do not acquire an invented origin
  from their current execution directory. There is no bulk history relocation or
  user-preference migration. Older hosts without the additive fields retain their
  previous presentation until upgraded.
- Local session listings expose `sidebar_repository_path` only when known, otherwise
  `sidebar_repository_fallback_path`. Team members additionally expose their team's
  display placement. These fields never replace `worktree_path`, context revision,
  task scope, membership, or execution authorization.
- Paired listings receive only host-sealed additive display metadata:
  `sidebarPlacementKnown`, opaque `sidebarRepositoryId`, and `sidebarRepositoryLabel`.
  Unknown legacy placement carries `false` plus the stable navigation fallback.
  Optional team placement is sealed with the existing membership descriptor.
  Plaintext service routing continues to use the current execution repository/worktree;
  origin paths are not sent there. Missing fields on older hosts remain compatible.
- Workspace catalogs retain every execution worktree. A separate display-only
  `sidebarVisible` marker omits confirmed non-Git roots from repository groups, while
  their conversations use Other conversations. Local IPC derives it on the host.
  Paired catalogs use sealed session metadata or the existing signed configuration
  response, after scope verification; the verified cache stores an optional boolean.
  An unsigned index flag cannot hide or create a repository. Older hosts and unknown
  metadata retain their previous presentation. This is not an execution permission.
- A known empty placement uses Other conversations. A real Git repository whose
  basename is `repository` keeps its identity and name. Selection, pinning, order,
  and the temporary sidebar filter remain independent of execution updates.

## Verification

Synthetic journal, host, team and machine fixtures cover nullable-schema compatibility,
no-repository versus unknown, write-once placement, linked worktrees, repository names,
execution updates, membership and restart/reconnect. Native sidebar fixtures exercise
selection and filtering. Desktop/shared-header changes require the full Pro build and
suite in addition to the focused tests; no installed application or live data is used.
