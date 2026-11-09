# 0030: Integration branch without CI, verified promotion into develop

Status: accepted
Date: 2026-11-05

## Context

Every pull request into `develop` ran the full build: the Linux daemon lane, the
browser checks, Lite on each platform and the macOS Pro build and tests. A small
change waited for the same matrix as a large one, and each push to its branch
started the matrix again. The CI cost was paid per pull request, not per change
that reached users.

`ketvietlab/ketviet` already solved this with an `integration` branch that has no
CI and a batch gate on promotion into `develop`.

## Decision

- Feature and fix pull requests target `integration`, which runs no CI. The
  author tests the part they changed and records the commands and results in
  the pull request.
- `develop` receives only promotion pull requests from `integration` in this
  repository, pointing at the integration tip. `promotion.yml` runs
  `.github/scripts/check-develop-promotion.sh`: `develop` must be an ancestor of
  the head, or the previous promotion's merge commit with the same tree as the
  integration commit it merged. A change made only on `develop` is refused.
  The job runs on `pull_request_target`, so the workflow and script come from
  `develop`, not from the pull request. The head is fetched as git data and never
  executed, the token is read-only and no secret is used; a pull request cannot
  rewrite the check that guards it.
- Promotions are merged with a merge commit, never squashed or rebased.
- `build.yml` and `webrtc-spike.yml` run on pull requests into `develop` and
  `main` and on pushes to `main`, and `build.yml` can also be started by hand.
  Pushes to `develop` no longer build, because the promotion pull request built
  the same tree.
- `main` and the release flow are unchanged.

## Consequences

- One full build covers a batch of changes. A failure found on promotion is
  fixed with a new pull request into `integration`, owned by the promoter or
  returned to the feature author.
- `integration` can be broken between promotions. Local testing of the changed
  area is the only check before merge, so pull requests must say what was run.
- Required checks on `develop` must name `promotion provenance` and the `build`
  jobs; `integration` needs no required checks but keeps force-push and deletion
  disabled.

## Alternatives considered

- Path-filtered CI per pull request: still one matrix per pull request, and the
  filters drift as `core/` and Pro share more code.
- Reusing a green ancestor gate for documentation-only promotions, as ketviet
  does: `paths-ignore` for Markdown already covers the common case here.
