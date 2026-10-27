#!/usr/bin/env bash
# Stop a build whose deps/ is older (or newer) than scripts/deps.lock, naming the fix, before the compiler
# fails on a missing symbol. A dependency that is a symlink to a shared checkout but is at its pin does not
# stop the build (scripts/fetch-deps.sh --verify still lists it). CW_SKIP_DEPS_CHECK=1 skips the check.
#   scripts/lib/check-deps.sh [root]
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
[ "${CW_SKIP_DEPS_CHECK:-0}" = 1 ] && exit 0
drift="$(bash "$ROOT/scripts/fetch-deps.sh" --verify 2>/dev/null | grep '^DRIFT ' || true)"
if [ -n "$drift" ]; then
  echo "build stopped: dependencies are not at the commits pinned in scripts/deps.lock:" >&2
  echo "$drift" | sed 's/^/  /' >&2
  echo "fix: run scripts/fetch-deps.sh (a symlinked shared checkout is replaced by a private copy at the pin; the shared one is not moved)" >&2
  exit 1
fi
