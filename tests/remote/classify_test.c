/* How a failed romdev wasm({op:'command'}) is read (src/remote/romdev.c cw__remote_romdev_classify): a cart trap is
 * a crash in romdev's old wording (the trap's text alone) and in its new one (after "wasm({op:'command'}): "); the
 * tool's own failures, under the same prefix, are request errors and the game lives on. tests/remote.mjs runs it, and
 * again with CW_TEST_REMOTE_OLD_CLASSIFY=1 (the old rule: anything under the prefix a request error), which must fail. */
#include <stdio.h>

#include "remote/internal.h"

int main(void) {
  static const struct { const char *err; int want; } cases[] = {
    { "unreachable", 1 },
    { "memory access out of bounds", 1 },
    { "wasm({op:'command'}): unreachable", 1 },
    { "wasm({op:'command'}): memory access out of bounds", 1 },
    { "wasm({op:'command'}): RuntimeError: divide by zero", 1 },
    { "wasm({op:'command'}): Maximum call stack size exceeded", 1 },
    { "wasm({op:'command'}): no reply within 600 frames", 0 },
    { "wasm({op:'command'}): text is required", 0 },
    { "romdev is not reachable at 127.0.0.1:7331", 0 },
    { "No ROM loaded: loadMedia({platform:'wasmcart', ...})", 2 },
    { "wasm({op:'command'}): this cart has no debug channel", 3 },
  };
  int bad = 0, n = (int)(sizeof cases / sizeof cases[0]);
  for (int i = 0; i < n; i++) {
    int got = cw__remote_romdev_classify(cases[i].err);
    if (got != cases[i].want) { bad++; printf("FAIL classify \"%s\": %d, want %d\n", cases[i].err, got, cases[i].want); }
  }
  printf("%s classify: %d of %d romdev errors read as they mean\n", bad ? "FAIL" : "ok  ", n - bad, n);
  return bad != 0;
}
