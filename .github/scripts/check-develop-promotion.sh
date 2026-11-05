#!/usr/bin/env bash
# Accept a pull request into develop only when it promotes the current
# integration tip from this repository. Develop must be an ancestor of that tip,
# or the merge commit of the previous promotion with the same tree as the
# integration commit it promoted. A change that exists only on develop fails.
set -euo pipefail

for name in BASE_SHA HEAD_BRANCH HEAD_REPOSITORY HEAD_SHA REPOSITORY; do
  [[ -n "${!name:-}" ]] || {
    printf 'Missing promotion input: %s\n' "$name" >&2
    exit 2
  }
done

fail() {
  printf '%s\n' "$1" >&2
  exit 1
}

[[ "$HEAD_BRANCH" == integration ]] ||
  fail "Pull requests into develop must come from integration, not $HEAD_BRANCH"
[[ "$HEAD_REPOSITORY" == "$REPOSITORY" ]] ||
  fail "Promotion must come from $REPOSITORY, not $HEAD_REPOSITORY"
[[ "$BASE_SHA" =~ ^[0-9a-f]{40}$ && "$HEAD_SHA" =~ ^[0-9a-f]{40}$ ]] ||
  fail 'Promotion commits must be full SHAs'
[[ "$(git rev-parse HEAD)" == "$HEAD_SHA" ]] ||
  fail 'The checkout is not the promotion head'

if ! git merge-base --is-ancestor "$BASE_SHA" "$HEAD_SHA"; then
  previous="$(git rev-parse --verify --quiet "$BASE_SHA^2")" ||
    fail 'Develop is neither an integration ancestor nor a promotion merge'
  git merge-base --is-ancestor "$previous" "$HEAD_SHA" ||
    fail 'The previous promotion is not in integration'
  [[ "$(git rev-parse "$BASE_SHA^{tree}")" == "$(git rev-parse "$previous^{tree}")" ]] ||
    fail 'Develop has changes that are not in integration'
fi

if [[ "${PROMOTION_SKIP_TIP_CHECK:-}" != 1 ]]; then
  git fetch --no-tags --quiet origin \
    '+refs/heads/integration:refs/remotes/origin/integration'
  [[ "$(git rev-parse refs/remotes/origin/integration)" == "$HEAD_SHA" ]] ||
    fail 'The promotion head is not the integration tip'
fi
