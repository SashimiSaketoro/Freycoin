#!/usr/bin/env bash
#
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Copyright (c) 2013-present The Riecoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -ex

if [ -n "$CIRRUS_PR" ]; then
  COMMIT_RANGE="HEAD~..HEAD"
  if [ "$(git rev-list -1 HEAD)" != "$(git rev-list -1 --merges HEAD)" ]; then
    echo "Error: The top commit must be a merge commit, usually the remote 'pull/${PR_NUMBER}/merge' branch."
    false
  fi
else
  # Otherwise, assume that a merge commit exists. This merge commit is assumed
  # to be the base, after which linting will be done. If the merge commit is
  # HEAD, the range will be empty.
  COMMIT_RANGE="$( git rev-list --max-count=1 --merges HEAD )..HEAD"
fi
export COMMIT_RANGE

echo
git log --no-merges --oneline "$COMMIT_RANGE"
echo
test/lint/commit-script-check.sh "$COMMIT_RANGE"
RUST_BACKTRACE=1 "${LINT_RUNNER_PATH}/test_runner"
