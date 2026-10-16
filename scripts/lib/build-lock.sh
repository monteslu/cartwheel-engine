# shellcheck shell=bash
# An exclusive lock on a build tree, the same one tools/lib/build-lock.mjs
# takes: two builds of one tree at once (two agents running scripts/build.sh,
# or an export building build/cart while build.sh does) run two ninjas over
# the same outputs, and a link then finds an archive another ninja is
# rewriting ("cannot open libcw_basisu_transcoder.a") or a .wasm another is
# still writing. The lock is the directory <tree>.lock (mkdir is atomic)
# holding the owner's pid; a lock whose owner is gone is taken over.
#   source scripts/lib/build-lock.sh
#   build_lock "$ROOT/build/cart"   ...   build_unlock "$ROOT/build/cart"
# Locks still held when the script exits are released.
_cw_locks=()
build_lock() {
  local lock="$1.lock" said=0 pid
  mkdir -p "$(dirname "$lock")"
  until mkdir "$lock" 2>/dev/null; do
    pid="$(cat "$lock/pid" 2>/dev/null || true)"
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then rm -rf "$lock"; continue; fi
    if [ $said = 0 ]; then echo "== waiting for another build of $1 (pid ${pid:-?})" >&2; said=1; fi
    sleep 0.25
  done
  echo $$ > "$lock/pid"
  _cw_locks+=("$lock")
  trap _cw_unlock_all EXIT
}
build_unlock() {
  rm -rf "$1.lock"
  local keep=() l
  for l in "${_cw_locks[@]}"; do [ "$l" = "$1.lock" ] || keep+=("$l"); done
  _cw_locks=("${keep[@]+"${keep[@]}"}")
}
_cw_unlock_all() { local l; for l in "${_cw_locks[@]+"${_cw_locks[@]}"}"; do rm -rf "$l"; done; }
