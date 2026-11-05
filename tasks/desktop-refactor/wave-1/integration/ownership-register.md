# Ownership Register

This register freezes allowed lanes for R00-B and separates what is allowed now from future implementation ownership. It is a coordination artifact, not permission to implement product UI in this branch.

## Backend

Interface owner: Backend owner for `ketviet` Control v2 and relay jobs.

Allowed now -- wave 1 audit/contract validation only:

- `ketviet/contracts/ketplus-control-v2.openapi.json` or a successor contract proposal file.
- `ketviet/contracts/fixtures/ketplus-control-v2/**` for sanitized contract fixtures.
- `ketviet/apps/ketsuite/modules/ketplus_control/**/__tests__/**` for contract tests only.
- `ketviet/docs/**` contract notes needed to explain schema and compatibility.

Future implementation ownership, not authorized by PR #18:

- Control auth/session/relay handlers.
- Durable jobs and maintenance workers.
- Database migrations, query implementations, retention cleanup, command lifecycle, tombstone/catalog metadata, and production rollout.

Dependencies:

- Desktop daemon IPC proposal from R02/R03 before Desktop consumers call new fields.
- Mobile parity fixtures before iOS DTO/cache changes.
- Existing code/spec baseline at backend integration `65f1e35fffffd299cfb4f93f4b24982e2b1bf807`.

Artifact handoff:

- OpenAPI/schema diff and normalized payload examples.
- Sanitized fixture bundle with hashes and positive/negative cases.
- Query plans for transcript paging and retention cleanup.
- Compatibility statement for old Desktop daemon, old mobile, and old cloud sessions.

Not allowed:

- PR #18 and R00-B do not authorize changing backend handlers, jobs, migrations, production DB queries, secrets, deployments, or live services.
- No UI assumptions in backend routes.
- No raw command output or plaintext transcript parsing in cloud.
- No endpoints invented by UI workers outside the reviewed contract.

## Public CM

Interface owner: Public KetPlus CM maintainer. Changes land in public `ketplus` first, then a tested production tag is pinned in Desktop.

Allowed now -- wave 1 audit/brief only:

- `tasks/desktop-refactor/wave-1/integration/public-safe-cm-brief.md`.
- Public-safe measurement/token tables prepared by Integration.
- Review comments on proposed public CM API shape.

Future implementation ownership, not authorized by PR #18:

- Public native primitives for semantic tokens, metrics, typography, theme propagation, settings host/deep-link APIs, search/workspace primitives, and generic safe Git/worktree operations.
- Public tests and visual primitive fixtures.

Dependencies:

- Design reference freeze from R00.
- Lifecycle/safe-remove contract proposal from R02/R08.
- Current Desktop pin: `cm-v0.1.5` at `9fb86c8ccf64bb75e1e5cdf31314cedf46d24439`.
- Public develop observed at `7ac9d758f1b095b4f85362d40fe4b6e55c261680`; do not pin this moving ref directly.

Artifact handoff:

- Public-safe token and measurement table.
- CM test results and visual primitive evidence.
- Release tag and changelog entry.
- Private Desktop gitlink update PR only after public release and private full-suite gate.

Not allowed:

- PR #18 and R00-B do not authorize editing public CM source or moving the Desktop gitlink.
- Do not edit `vendor/ketplus-cm` from Desktop.
- Do not publish private AI, Atlas, account, Router, daemon, transcript, or remote-control implementation.
- Do not add proprietary compile flags or private service assumptions to CM.

## Core / Transcript

Interface owner: Desktop daemon/Core transcript owner for canonical projection and Desktop transcript model owner for UI windowing/render roles.

Allowed now -- wave 1 audit/harness/baseline only:

- `tasks/desktop-refactor/wave-1/integration/**`.
- `tests/design/integration-evidence.test.cjs`.
- Future synthetic metadata/workload generators under `tests/design/**` when they do not touch runtime modules, real daemon state, provider quota, or user data.

Future implementation ownership, not authorized by PR #18:

- Desktop daemon journal/projection.
- IPC transcript page API, migration/outbox, bounded replay, and local journal changes.
- Desktop transcript model/window/anchor roles and renderer contracts after owner freeze.
- Focused runtime tests under the owning private modules.

Dependencies:

- R02 canonical item/page/lifecycle fixtures.
- R03 daemon projection and continuation implementation before R05 model behavior.
- R05 model roles before R06 renderer.
- R07 workspace composition after model/render contracts.

Artifact handoff:

- IPC schema or typed protocol examples.
- Synthetic giant-turn, 1k/10k item, page/live overlap, stale generation, and command-output-stripping fixtures.
- Cache budget counters and benchmark raw output.
- Explicit old-daemon fallback behavior.

Not allowed:

- PR #18 and R00-B do not authorize changing daemon journal/render/runtime code, QML behavior, handlers, or production test registration.
- Do not make QML the authority for transcript history.
- Do not parse full journal/history on first paint to pass a demo.
- Do not silently truncate a turn or mark a missing range as loaded.
- Do not call provider/live quota tests for baseline or benchmarks.

## Integration-Owned Common Files

Integration owns tracking, coverage, harness coordination, and final evidence ledgers. Common CMake/test registration, gitlink movement, `atlas.progress.json`, and cross-task coverage mapping must be coordinated through Integration. Workers may propose patches, but only one writer should touch those common files at a time.
