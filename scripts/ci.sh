#!/usr/bin/env bash
# The whole check, in order: build both targets, regenerate the API data,
# run every gate on every host plus the channel / project / hot-reload tests,
# then the sanitizer pass. Exit status is nonzero on any failure.
#   scripts/ci.sh               QUICK=1 skips the sanitizer pass
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail=0
step() { echo "== $*"; "$@" || { echo "FAILED: $*"; fail=1; }; }
step scripts/build.sh
step node tools/gen-api.mjs
step node tools/gen-api.mjs --check
step node tools/gen-noise-shaders.mjs --check
# the suites are GPU suites: lock, kernel-log and memory guards, no desktop (exit 3 = a guard fired)
step node tests/all.mjs --no-chromium
step node tests/all.mjs --chromium-only   # last and alone: the only thing that has ever faulted the GPU
[ "${QUICK:-0}" = 1 ] || step node tools/with-gpu-suite.mjs sanitize scripts/sanitize.sh
[ "${QUICK:-0}" = 1 ] || step env SAN=thread node tools/with-gpu-suite.mjs sanitize-thread scripts/sanitize.sh
exit $fail
