#!/usr/bin/env bash
# Fetch every pinned dependency into deps/ (gitignored), from scripts/deps.lock.
#
#   scripts/fetch-deps.sh          fetch what is missing
#   FORCE=1 scripts/fetch-deps.sh  refetch everything
#
# Git deps are fetched from the source in the lock (a git URL, or a ../NAME
# sibling checkout when the lock names one) and checked out at the pinned
# commit, so the source can move on without changing cartwheel's build.
# Archives are downloaded and, where the lock gives one, checked by sha256.
# WASI_SDK (env) points at an existing wasi-sdk instead of downloading one.
#
# export/* entries are the exporters' toolchains and runtimes (tools/cwexport):
# cross compilers, sysroots, ANGLE builds. They are large and only exporting
# needs them, so they are fetched only on request, into deps/export/<name>:
#   scripts/fetch-deps.sh --export            everything, export deps included
#   scripts/fetch-deps.sh --only export/NAME  one entry (any name in the lock)
#   scripts/fetch-deps.sh --only 'a,b,font:*' several (comma separated, shell globs)
# Every export entry must carry a sha256. NAME@<os>-<arch> lines (e.g.
# export/llvm-mingw@linux-aarch64) replace NAME on that kind of host: the
# toolchain built to run there.
set -euo pipefail
WANT_EXPORT=0 ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --export) WANT_EXPORT=1 ;;
    --only) ONLY="$2"; shift ;;
    *) echo "usage: fetch-deps.sh [--export] [--only NAME]" >&2; exit 2 ;;
  esac
  shift
done
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/deps"
mkdir -p "$DEPS"
HOST="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
# sha256sum where there is one (Linux), shasum on macOS
sha_check() { if command -v sha256sum >/dev/null; then sha256sum -c --quiet -; else shasum -a 256 -c --quiet -; fi; }
# --only: is this lock name one of the requested (comma separated globs)?
wanted() {
  local pat IFS=,
  for pat in $ONLY; do
    # shellcheck disable=SC2053
    [[ "$1" == $pat ]] && return 0
  done
  return 1
}
# names a NAME@<this host> line replaces
OVERRIDDEN=" $(awk -v h="@$HOST" '$1 ~ h"$" { sub(h"$", "", $1); print $1 }' "$ROOT/scripts/deps.lock" | tr '\n' ' ')"

# patches/<name>/*.patch are applied on top of the pinned commit (git deps);
# the tree is reset first, so re-running is idempotent.
apply_patches() { # name dst
  local p
  git -C "$2" checkout -q -- .
  for p in "$ROOT"/patches/"$1"/*.patch; do
    [ -f "$p" ] || continue
    git -C "$2" apply "$p" || { echo "$1: patch failed: $p" >&2; exit 1; }
  done
}

# ../NAME sources are siblings of the checkout; a git worktree (worktrees/cartwheel-<topic>) has
# none beside it, so they resolve against the main checkout there
sibling() { # ../name
  local p="$ROOT/$1" main
  if [ ! -e "$p" ]; then
    main="$(cd "$(git -C "$ROOT" rev-parse --git-common-dir)/.." && pwd)"
    [ -e "$main/$1" ] && p="$main/$1"
  fi
  echo "$p"
}

fetch_git() { # name src rev
  local name=$1 src=$2 rev=$3 dst="$DEPS/$1"
  case "$src" in ../*) src="$(sibling "$src")" ;; esac
  if [ -d "$dst/.git" ] && [ "${FORCE:-0}" != 1 ]; then
    if [ "$(git -C "$dst" rev-parse HEAD)" = "$rev" ]; then apply_patches "$name" "$dst"; echo "$name: $rev"; return; fi
    git -C "$dst" fetch -q "$src" "$rev" 2>/dev/null || git -C "$dst" fetch -q "$src" "+refs/heads/*:refs/remotes/src/*" "+refs/tags/*:refs/tags/*"
  else
    # only the pinned commit when the server allows it (GitHub does), else the whole history
    rm -rf "$dst" && git init -q "$dst"
    git -C "$dst" fetch -q --depth 1 "$src" "$rev" 2>/dev/null || git -C "$dst" fetch -q "$src" "+refs/heads/*:refs/remotes/src/*" "+refs/tags/*:refs/tags/*"
  fi
  git -C "$dst" -c advice.detachedHead=false checkout -q "$rev"
  apply_patches "$name" "$dst"
  echo "$name: $rev"
}

fetch_archive() { # name url version sha
  local name=$1 url=$2 ver=$3 sha=$4 dst="$DEPS/$1"
  if [ -f "$dst/.version" ] && [ "$(cat "$dst/.version")" = "$ver" ] && [ "${FORCE:-0}" != 1 ]; then
    echo "$name: $ver"; return
  fi
  local tgz="$DEPS/.dl/$(basename "$url")"
  mkdir -p "$DEPS/.dl"
  [ -f "$tgz" ] || curl -fsSL "$url" -o "$tgz"
  if [ "$sha" != - ]; then
    echo "$sha  $tgz" | sha_check || { echo "$name: sha256 mismatch" >&2; exit 1; }
  fi
  rm -rf "$dst" && mkdir -p "$dst"
  case "$url" in
    *.zip) # one top directory (emdawnwebgpu_pkg/): its contents land in deps/<name>
      local tmp="$DEPS/.dl/unzip-$name"
      rm -rf "$tmp" && mkdir -p "$tmp" && unzip -q "$tgz" -d "$tmp"
      if [ "$(ls -A "$tmp" | wc -l)" = 1 ] && [ -d "$tmp/$(ls -A "$tmp")" ]; then cp -a "$tmp/$(ls -A "$tmp")/." "$dst/"; else cp -a "$tmp/." "$dst/"; fi
      rm -rf "$tmp" ;;
    *) # strip the top directory when the archive has one (native-dawn's tarball is a bare prefix)
      local tops; tops="$(tar -tzf "$tgz" | sed 's|^\./||' | cut -d/ -f1 | sort -u | grep -c .)"
      if [ "$tops" = 1 ]; then tar -xzf "$tgz" -C "$dst" --strip-components=1; else tar -xzf "$tgz" -C "$dst"; fi ;;
  esac
  echo "$ver" > "$dst/.version"
  echo "$name: $ver"
}

# export/NAME: archive (tar.*, zip) or single file into deps/export/NAME,
# sha256 required. Per-entry extraction rules below.
fetch_export() { # name url version sha
  local name=$1 url=$2 ver=$3 sha=$4 dst="$DEPS/$1"
  if [ -f "$dst/.version" ] && [ "$(cat "$dst/.version")" = "$ver" ] && [ "${FORCE:-0}" != 1 ]; then
    echo "$name: $ver"; return
  fi
  [ "$sha" != - ] || { echo "$name: export deps must pin a sha256" >&2; exit 1; }
  local file="$DEPS/.dl/${name//\//_}-$ver-$(basename "$url")"
  mkdir -p "$DEPS/.dl"
  if [ ! -f "$file" ]; then
    curl -fsSL "$url" -o "$file.part" || { echo "$name: download failed: $url" >&2; exit 1; }
    mv -f "$file.part" "$file"
  fi
  echo "$sha  $file" | sha_check || { echo "$name: sha256 mismatch ($file)" >&2; exit 1; }
  rm -rf "$dst" && mkdir -p "$dst"
  case "$name" in
    export/sysroot-*) tar -xJf "$file" -C "$dst" ;;                      # Chromium sysroots: no top directory
    export/native-dawn-*|export/angle-gles-*) tar -xzf "$file" -C "$dst" ;; # native-dawn / native-gles archives: a bare prefix
    *) case "$url" in
         *.tar.xz|*.tar.gz|*.tgz) tar -xf "$file" -C "$dst" --strip-components=1 ;;
         *.pkg.tar.zst) tar -xf "$file" -C "$dst" --strip-components=1 --exclude=".[A-Z]*" ;; # MSYS2 package: <prefix>/{bin,lib,include,share}
         *.zip) unzip -q -o "$file" -d "$dst" ;;
         *) cp "$file" "$dst/$(basename "$url")"; chmod +x "$dst/$(basename "$url")" ;;
       esac ;;
  esac
  echo "$ver" > "$dst/.version"
  echo "$name: $ver"
}

# font:<file> lines: one file into deps/fonts/<file>, sha256-checked (fonts and their licenses)
fetch_file() { # file url version sha
  local dst="$DEPS/fonts/$1"
  mkdir -p "$DEPS/fonts"
  if [ -f "$dst" ] && [ -f "$dst.version" ] && [ "$(cat "$dst.version")" = "$3" ] && [ "${FORCE:-0}" != 1 ]; then
    echo "$1: $3"; return
  fi
  curl -fsSL "$2" -o "$dst.part"
  echo "$4  $dst.part" | sha_check || { echo "$1: sha256 mismatch" >&2; rm -f "$dst.part"; exit 1; }
  mv "$dst.part" "$dst"
  echo "$3" > "$dst.version"
  echo "$1: $3"
}

while read -r name src rev sha; do
  [ -z "$name" ] || [ "${name#\#}" != "$name" ] && continue
  case "$name" in
    *@"$HOST") name="${name%@*}" ;;          # this host's build of NAME
    *@*) continue ;;                          # another host's
    *) [[ "$OVERRIDDEN" == *" $name "* ]] && continue ;;
  esac
  if [ -n "$ONLY" ] && ! wanted "$name"; then continue; fi
  case "$name" in
    export/*)
      [ "$WANT_EXPORT" = 1 ] || [ -n "$ONLY" ] || continue
      mkdir -p "$DEPS/export"
      fetch_export "$name" "$src" "$rev" "$sha" ;;
    font:*) fetch_file "${name#font:}" "$src" "$rev" "$sha" ;;
    native-dawn)
      if [ -n "${NATIVE_DAWN_PREFIX:-}" ]; then
        rm -rf "$DEPS/native-dawn"; ln -sfn "$NATIVE_DAWN_PREFIX" "$DEPS/native-dawn"; echo "native-dawn: $NATIVE_DAWN_PREFIX (NATIVE_DAWN_PREFIX)"; continue
      fi
      fetch_archive "$name" "$src" "$rev" "$sha" ;;
    wasi-sdk)
      if [ -n "${WASI_SDK:-}" ]; then
        rm -rf "$DEPS/wasi-sdk"; ln -sfn "$WASI_SDK" "$DEPS/wasi-sdk"; echo "wasi-sdk: $WASI_SDK (WASI_SDK)"; continue
      fi
      fetch_archive "$name" "$src" "$rev" "$sha" ;;
    *)
      # a git source pins a commit: "<rev>", or "<version tag> <full hash>" (the hash is what is checked out)
      grev="$rev"; [[ "$sha" =~ ^[0-9a-f]{40}$ ]] && grev="$sha"
      case "$src" in
        http*://*.git) fetch_git "$name" "$src" "$grev" ;;
        http*://*) fetch_archive "$name" "$src" "$rev" "$sha" ;;
        *) fetch_git "$name" "$src" "$grev" ;;
      esac ;;
  esac
done < "$ROOT/scripts/deps.lock"

# wasmcart's Node tooling (pack, the reference hosts) needs its packages (a
# checkout: an SDK carries the few it uses, and no lockfile)
if [ -f "$DEPS/wasmcart/package-lock.json" ] && [ ! -d "$DEPS/wasmcart/node_modules" ]; then
  (cd "$DEPS/wasmcart" && npm ci --no-audit --no-fund >/dev/null)
fi
# the WebGPU tier's hosts (webgpu-node, playwright) for the dual-cart runs
if [ -f "$DEPS/wasmcart-wgpu/package-lock.json" ] && [ ! -d "$DEPS/wasmcart-wgpu/node_modules" ]; then
  (cd "$DEPS/wasmcart-wgpu" && npm ci --no-audit --no-fund >/dev/null)
fi
