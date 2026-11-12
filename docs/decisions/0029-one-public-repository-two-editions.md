# 0029: One public repository, two editions

Status: accepted
Date: 2026-11-05
Supersedes: [0001](0001-public-private-source-boundary.md)

## Context

KetPlus shipped as two products from two repositories: the public KetPlus CM editor
(`ketplus`, MPL 2.0) and the private full desktop (`ketplus-desktop`, proprietary), which
consumed CM through the pinned `vendor/ketplus-cm` submodule. Every editor change was
released in the public repository first and then pulled in by a gitlink update.

The desktop is now open source. Revenue comes from the mobile apps and the hosted relay,
gateway and account services, which stay private. The editor alone is preinstalled on
Lampion.

## Decision

- The product keeps the name KétPlus and the repository keeps the name `ketplus`. One
  source tree builds two editions, selected by `KETPLUS_EDITION`:

  | Edition | Product | Contents | Default on |
  | --- | --- | --- | --- |
  | `lite` | KétPlus | `core/`: editor, workspace, Git, terminal, preview | Linux, Windows |
  | `pro` | KétPlus Pro | `core/` plus `apps/` and `modules/`: Agent Workspace, Atlas, daemon, remote sessions | macOS |

  Lite is the former KetPlus CM and the default editor on Lampion. Pro is the former
  KetPlus Desktop.
- The CM source moves to `core/` with its history. `core/` builds without any Pro
  directory and contains no edition checks; Pro links its exported `KetPlusCM::*` targets.
  The submodule and the public-first release step are removed. A change to both lands in
  one pull request.
- The private desktop source is imported as one commit (ketplus-desktop `ae3add63`,
  desktop 0.1.50) after a secret, path and license review. Its history is not published;
  `ketplus-desktop` is archived as a private repository. Open pull requests there are
  ported as new pull requests here.
- The whole repository is MPL 2.0. Third-party licenses are unchanged and are listed in
  `docs/licensing.md`. The bundled `ket-atlas`, `enterprise-grade` and
  `ket-design-system` agent skills are MIT. The KÉT Sans font keeps its own license from
  the public `ket-sans-font` repository.
- The relay client is open source with the rest of the desktop: `modules/remote`,
  `apps/desktop/src/remote` and the daemon's host side in `apps/daemon/src/cloud`. Users
  can audit the end-to-end encryption, and the daemon does not have to close again
  because it shares the protocol and keys with the client.
- the private mobile app (iOS and Android) and the relay, gateway, account and KétRouter services
  in the private relay service stay private and proprietary. Paid features are enforced by those services,
  never by a build flag: anyone may build KétPlus Pro from source.
- Each edition keeps its own version and tag (`lite-v*`, `pro-v*`;
  `docs/release-process.md`). Earlier `cm-v*` tags stay as they are.

## Consequences

- Editor changes land once instead of as a public release followed by a gitlink update.
- The daemon, the relay client and the end-to-end encryption between desktop and phone
  are public. Their security must not rely on the protocol staying private.
- Modified clients can reach the hosted relay. The relay authorizes every request by
  account, entitlement and rate limit, and trusts nothing a client asserts about its build.
- The wave-1 refactor evidence under `tasks/desktop-refactor` was pinned to private
  revisions. Its CI gates (`design-evidence.yml`) and `tests/design/integration-evidence`
  cannot run without that history and are retired; the evidence stays as a record.
- User-visible names change to KétPlus and KétPlus Pro. Bundle identifiers, settings and
  data directories are unchanged in this step, so existing installs, Sparkle updates and
  paired phones keep working; renaming them needs a migration of its own.
- Forks that use another relay must not use the KétPlus name or icon; a trademark policy,
  not the license, says so.

## Open questions

- A trademark policy for the KétPlus name and logo.

## Alternatives considered

- **Pro in a `pro/` subdirectory with the editor at the root:** every Pro path, include
  and document changes, and Pro code already builds the editor as a subdirectory.
- **Relay client as a private package or plugin:** the protocol is still readable from the
  binary and traffic, users cannot verify the encryption, the daemon's host side would
  have to close too, and Linux distributions cannot ship it.
- **New empty repository:** loses the public history, tags and redirects of `ketplus`.
- **Make `ketplus-desktop` public as it is:** publishes 700+ commits that were never
  reviewed for publication.
- **Keep two repositories:** keeps the two-step release for every editor change with no
  remaining licensing reason for the split.
