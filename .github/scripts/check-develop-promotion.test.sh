#!/usr/bin/env bash
# Exercise check-develop-promotion.sh against a throwaway repository.
set -euo pipefail

script="$(cd "$(dirname "$0")" && pwd)/check-develop-promotion.sh"
repository="$(mktemp -d)"
trap 'rm -rf "$repository"' EXIT
cd "$repository"

git init --quiet --initial-branch=develop
git config user.email ci@example.invalid
git config user.name CI
commit() {
  printf '%s\n' "$1" >"$1"
  git add "$1"
  git commit --quiet -m "$1"
}

commit base
git switch --quiet -c integration
commit feature

check() {
  local expected="$1" base="$2" head="$3" branch="${4:-integration}" from="${5:-ketvietlab/ketplus}"
  git checkout --quiet --detach "$head"
  local status=0
  BASE_SHA="$(git rev-parse "$base")" HEAD_SHA="$(git rev-parse "$head")" \
    HEAD_BRANCH="$branch" HEAD_REPOSITORY="$from" REPOSITORY=ketvietlab/ketplus \
    PROMOTION_SKIP_TIP_CHECK=1 bash "$script" 2>/dev/null || status=$?
  if [[ "$expected" == pass && "$status" -ne 0 || "$expected" == fail && "$status" -eq 0 ]]; then
    printf 'expected %s: base=%s head=%s branch=%s from=%s\n' "$expected" "$base" "$head" "$branch" "$from" >&2
    exit 1
  fi
}

check pass develop integration
check fail develop integration feature/x
check fail develop integration integration someone/ketplus

# The previous promotion merged integration into develop with a merge commit.
git switch --quiet develop
git merge --quiet --no-ff --no-edit integration
git switch --quiet integration
commit next
check pass develop integration

# A commit made only on develop is refused.
git switch --quiet develop
commit hotfix
check fail develop integration

# A merge whose tree differs from the integration commit it merged is refused.
git reset --quiet --hard HEAD~1
git commit --quiet --amend -m 'promotion with an extra change' --allow-empty
printf 'drift\n' >drift && git add drift && git commit --quiet --amend --no-edit
check fail develop integration

printf 'check-develop-promotion: all cases passed\n'
