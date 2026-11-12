# Agent instructions

These instructions apply to the entire public `ketplus` repository. It builds two
editions from one source tree (ADR 0029): KétPlus (`KETPLUS_EDITION=lite`, the
editor in `core/`) and KétPlus Pro (`pro`).

## Start here

Before changing code, read:

1. `docs/project-status.md` for implemented and planned behavior.
2. `docs/repository-boundaries.md` for Lite/Pro ownership.
3. `docs/architecture.md` and the feature-specific document for the area changed.
4. `docs/development.md` and `docs/testing.md` before building or testing.

Product, identity, gateway, Router, and monetization decisions live in
`docs/product-architecture.md`. Licensing constraints live in
`docs/licensing.md` and are release requirements, not optional guidance.

## Non-negotiable boundaries

- This repository is public under MPL 2.0. The mobile apps and hosted services
  are proprietary and live in private repositories. Do not publish their repository
  links, source revisions, file paths, service/schema names or infrastructure details.
  Do not copy their implementation, endpoints or internal hostnames here.
  Describe external ownership generically; preserve public library API names and licenses.
- Security must not depend on the source or protocol staying private. Paid
  features are enforced by the hosted services, never by a build flag.
- `core/` must build without any Pro directory. Do not add `KETPLUS_EDITION`
  checks inside it; Pro extends it through exported `KetPlusCM::*` targets and
  public include paths, never relative includes into `core/src`.
- Never commit credentials, access tokens, user transcripts, machine-specific
  paths, generated build trees, `.cache` contents, or signing material.
- Do not run tests that consume provider quota unless the user explicitly asks.

## Replacement bundles

Never copy raw build-tree executables into installed or previously deployed apps.
Use `cmake --install` into a staging directory and replace the entire deployed bundle.
Check the main executable's `otool -L` for Homebrew dependencies, strict deep
codesign verification, and the daemon `--build-info` URL against the user's intended
deployment before giving replacement commands. Increase the project version when
distributing daemon changes; never interrupt active turns to install.

## Browser tasks

When a task needs a real browser (opening a local page, checking rendered UI,
screenshots, visual comparison), read
`apps/browser/.claude/skills/ketplus-browser/SKILL.md` first and use the
`ketplus_browser` MCP tools when they are available instead of ad-hoc Playwright.

You MUST close every browser session you create with `browser_close_session` as
soon as browser work ends and before your final response, including failed or
blocked work. Preserve needed evidence first and verify closure; report unclosed
session IDs if cleanup fails. Only keep a session open at the user's explicit
request. Do not close user-owned or other agents' sessions or kill shared browsers.

## Change workflow

1. Inspect the affected target, nearby tests, and relevant design document.
2. Keep the change within its owning module. Add a new abstraction only when it
   removes real duplication or establishes a required boundary.
3. Add or update focused tests, then run the minimum matrix in
   `docs/testing.md`.
4. Format changed C++ files with the repository `.clang-format`.
5. Run `git diff --check` before committing. A `core/` change also runs the Lite
   suite (`ctest --preset lite-dev`).
6. Branch from `integration` and open the pull request into `integration`; it
   has no native build CI, so step 3 is the check; the lightweight public-tree
   boundary guard still runs on pull requests. Only `integration` is promoted into
   `develop`, and `main` is production, updated only by an explicit release
   (`docs/release-process.md`).

## Két UI migration (#402)

The compatibility stylesheet in `core/src/ui/Theme.cpp` (`styleSheetFor`) and
`ThemePalette` are being retired in favour of Két kit widgets, recipes, and
tokens. Do not grow either one. When a change touches a widget or screen that
still takes colours or metrics from that stylesheet or from `ThemePalette`:

1. Move that surface onto kit widgets, recipes, or `ket::Tokens` in the same
   change. A component the kit lacks goes to ket-qt first; until it lands,
   leave the surface as is and say so on #402.
2. Delete each selector and palette field that nothing reads anymore.
3. Keep light, dark, follow system, and every zoom step switching live, and run
   the tests listed in #402 that cover the surface.
4. Update #402 when the pull request merges: comment with the surface, the
   selectors and fields removed, and what still depends on them there. Tick a
   progress-list entry only when every consumer it names is migrated; until
   then the comment records the partial progress. Tick any acceptance item
   that now holds.

## Source structure

- `core/`: editor, documents, workspace, Git, preview, terminal, settings, and
  shared UI; the Lite application. Used by both editions.
- `apps/desktop`: Pro executable shell, windows, menus, tabs, and composition.
- `apps/daemon`: local and headless Linux daemon and `ketplusctl`.
- `apps/browser`: bundled browser runtime, a separate helper process.
- `apps/computer`: KetPlus Computer, the separately signed macOS app for agent
  computer use (Swift Package; ADR 0030-0032).
- `modules/ai`: provider transport and Agent Workspace.
- `modules/atlas`: Atlas process, WebView, and code index.
- `modules/remote`: daemon IPC client, relay (Control v2) client, and the
  `ketplus_control_crypto` keys/credentials/sign-in target the daemon also links.
- `modules/computer`: computer use settings and provider MCP launch.
- `tests`: Pro and cross-boundary integration tests.
- `modules/accounts` and `modules/router`: reserved; create each only with its
  first real implementation.

Avoid returning to flat or monolithic source layout. As a review trigger,
consider splitting a production file before it exceeds 500 lines. A production
file above 1,000 lines requires an explicit reason in the change description.
Split by ownership or behavior, not by arbitrary line ranges. Existing large
files are listed in `docs/project-status.md`; do not expand them casually.

## Documentation

Update documentation in the same change when modifying a protocol, ownership
boundary, security property, build command, dependency, or release process.
Record durable architectural decisions under `docs/decisions/`.
