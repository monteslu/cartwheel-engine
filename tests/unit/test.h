/* Unit tests that run natively AND as carts: each suite is an app whose
 * init() runs every test and publishes the counts as debug fields
 * (unit.passed, unit.failed, unit.checksum), so tests/gate.mjs compares them
 * across hosts like any other state. A failing check logs file:line. */
#ifndef CW_TEST_H
#define CW_TEST_H

#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/core.h"
#include "cartwheel/gpu.h"
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"

extern int32_t cw_test_passed, cw_test_failed;
extern uint32_t cw_test_checksum; /* results that must agree across hosts fold in here */

#define CHECK(cond) \
  do { \
    if (cond) cw_test_passed++; \
    else { cw_test_failed++; cw_error("CHECK failed %s:%d: %s", __FILE__, __LINE__, #cond); } \
  } while (0)
#define CHECK_EQ_INT(a, b) \
  do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) cw_test_passed++; \
    else { cw_test_failed++; cw_error("CHECK_EQ failed %s:%d: %s = %lld, expected %lld", __FILE__, __LINE__, #a, _a, _b); } \
  } while (0)
#define FOLD(v) (cw_test_checksum = cw_hash32(&(v), sizeof(v), cw_test_checksum))

typedef void (*cw_test_fn)(void);
typedef struct { const char *name; cw_test_fn fn; } cw_test_case;

/* Define the suite: CW_TEST_SUITE("core", {"pool", test_pool}, ...) */
#define CW_TEST_SUITE(suite_name, ...) \
  int32_t cw_test_passed, cw_test_failed; \
  uint32_t cw_test_checksum = CW_HASH32_INIT; \
  static const cw_test_case cw__cases[] = { __VA_ARGS__ }; \
  static int32_t cw__done; \
  static void cw__test_render(void *u, double a) { /* green = passed, red = failed (GL or WebGPU) */ \
    (void)u; (void)a; \
    cw_gpu_clear_window(cw_test_failed ? 0.8f : 0.1f, cw_test_failed ? 0.1f : 0.7f, 0.2f, 1); \
  } \
  static void cw__test_init(void *u) { \
    (void)u; \
    cw_debug_i32("unit.passed", &cw_test_passed); \
    cw_debug_i32("unit.failed", &cw_test_failed); \
    cw_debug_u32("unit.checksum", &cw_test_checksum); \
    cw_debug_i32("unit.done", &cw__done); \
    for (size_t i = 0; i < CW_ARRAY_LEN(cw__cases); i++) { \
      int before = cw_test_failed; \
      cw__cases[i].fn(); \
      cw_info("%s.%s: %s", suite_name, cw__cases[i].name, cw_test_failed == before ? "ok" : "FAILED"); \
    } \
    if (getenv("CW_TEST_INJECT_FAIL")) CHECK(!"injected failure (the gate control)"); \
    cw_info("%s: %d passed, %d failed", suite_name, cw_test_passed, cw_test_failed); \
    cw__done = 1; \
  } \
  cw_app_desc cw_main(int argc, char **argv) { \
    (void)argc; (void)argv; \
    return (cw_app_desc){ .title = suite_name " tests", .width = 64, .height = 64, .debug = true, \
                          .init = cw__test_init, .render = cw__test_render }; \
  }

#endif
