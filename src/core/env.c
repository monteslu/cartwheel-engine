/* A test's environment on every host: the debug channel's `env` request.
 *
 * Games and the engine pick test modes from environment variables (a game's
 * PP_ROUNDS, the engine's CW_TEST_*). A native run gets them from its process
 * environment; a cart has none. `env NAME=VALUE ...` sets them in the running
 * game on any host: natively with setenv, in a cart in this table, which
 * getenv reads first (every cart link wraps getenv, cmake/app.cmake, so the
 * game's own getenv calls see it with no change to the game).
 *
 * A cart host delivers the request at a frame (romdev answers a debug request
 * at the start of frame 2 at the earliest; Chromium and wasmcart-native post
 * it before frame 1). A name the game read before the request arrived (in its
 * init, or a value it cached in frame 1) would run with the value missing and
 * agree with nothing: the cart remembers the names read before the first
 * `env` request, and setting one of them fails the request, naming it. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"

#define ENV_MAX 64
#define ENV_NAME 64
#define ENV_VALUE 256

#if CW_CART
static struct {
  char name[ENV_NAME];
  char value[ENV_VALUE];
} vars[ENV_MAX];
static int nvars;
/* names read before the first `env` request (the late check) */
#define EARLY_MAX 512
static char early[EARLY_MAX][ENV_NAME];
static int nearly;
static bool early_full;
static bool delivered;

char *__real_getenv(const char *name);

char *__wrap_getenv(const char *name) {
  if (!name) return NULL;
  for (int i = 0; i < nvars; i++)
    if (!strcmp(vars[i].name, name)) return vars[i].value;
  if (!delivered) {
    bool seen = false;
    for (int i = 0; i < nearly && !seen; i++) seen = !strcmp(early[i], name);
    if (!seen) {
      if (nearly < EARLY_MAX && strlen(name) < ENV_NAME) snprintf(early[nearly++], ENV_NAME, "%s", name);
      else early_full = true;
    }
  }
  return __real_getenv(name);
}
#endif

/* Sets name to value (check_only: only says whether it could). Returns NULL, or
 * why it cannot be set (the message lives until the next call). */
const char *cw__env_set(const char *name, const char *value, bool check_only);
const char *cw__env_set(const char *name, const char *value, bool check_only) {
  static char why[320];
  size_t n = strlen(name);
  if (!n || n >= ENV_NAME || strchr(name, '=')) {
    snprintf(why, sizeof why, "'%s' is not a variable name (at most %d characters, no '=')", name, ENV_NAME - 1);
    return why;
  }
  if (strlen(value) >= ENV_VALUE) {
    snprintf(why, sizeof why, "%s: the value is over %d characters", name, ENV_VALUE - 1);
    return why;
  }
#if CW_CART
  if (!delivered) {
    for (int i = 0; i < nearly; i++)
      if (!strcmp(early[i], name)) {
        snprintf(why, sizeof why,
                 "%s was read before this request reached the game (in its init or its first frame), so the run "
                 "so far had it unset: read it later (when the mode is first needed), give the game a debug command "
                 "for it, or deliver the request before frame 1 (Chromium, wasmcart-native)", name);
        return why;
      }
    if (early_full) {
      snprintf(why, sizeof why, "%s: the game read more than %d variables before this request, so a late one cannot be "
               "told apart; send the request earlier", name, EARLY_MAX);
      return why;
    }
  }
  int at = nvars;
  for (int i = 0; i < nvars; i++)
    if (!strcmp(vars[i].name, name)) at = i;
  if (at == ENV_MAX) {
    snprintf(why, sizeof why, "%s: more than %d variables set", name, ENV_MAX);
    return why;
  }
  if (check_only) return NULL;
  snprintf(vars[at].name, ENV_NAME, "%s", name);
  snprintf(vars[at].value, ENV_VALUE, "%s", value);
  if (at == nvars) nvars++;
#else
  if (check_only) return NULL;
#if defined(_WIN32)
  if (_putenv_s(name, value)) {
#else
  if (setenv(name, value, 1)) {
#endif
    snprintf(why, sizeof why, "%s: setenv failed", name);
    return why;
  }
#endif
  return NULL;
}

const char *cw_test_env(const char *name) {
  if (cw_frame_index() == 0) return NULL; /* the first frame: no cart host has delivered the env request yet */
  return getenv(name);
}

/* After the first `env` request getenv stops remembering names. */
void cw__env_delivered(void);
void cw__env_delivered(void) {
#if CW_CART
  delivered = true;
#endif
}
