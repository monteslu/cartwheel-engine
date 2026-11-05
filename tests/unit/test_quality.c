/* unit_quality: the graphics settings' choices and save words (cartwheel/quality.h), outside a deterministic run
 * (CW_PLAYER_TIMING: a deterministic run locks Auto and a saved choice out). tests/quality-unit.mjs runs it with each
 * case's control and the restart below.
 *   resolution   the player's 75% stays 75% on every preset and on Auto, the preset's name shown (not Custom), and a
 *                save round trip keeps both (control CW_TEST_QUALITY_OLD_RES: presets set 100% again, as before)
 *   save_compat  every word an older engine wrote (none, a preset byte, Auto rungs, Custom, with levers) restores the
 *                choice and settings it held (control CW_TEST_QUALITY_V2_ONLY: old words read with the new layout)
 *   levers       a lever's five preset values apply on each preset
 *   auto_jump    a slow host (CW_TEST_FRAME_MS=60, a 19 ms budget) starting at High is on the rung that fits within
 *                3 seconds of play (control CW_TEST_QUALITY_NO_JUMP: one rung every 150 frames)
 *   cycle        the graphics row steps through Very Low, Low, Medium, High, Ultra, Auto and back again
 *   restart      QUALITY_PHASE=1 saves 75% and Medium through the shell; QUALITY_PHASE=2 (a new process on the same
 *                CW_SAVE_FILE) must load both */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/profile.h"
#include "cartwheel/quality.h"
#include "cartwheel/shell.h"
#define CW_TEST_SAVE cw_shell_save_region()
#define CW_TEST_SAVE_SIZE cw_shell_save_region_size()
#include "test.h"

static bool timed(void) { return getenv("CW_PLAYER_TIMING") != NULL; }
static int phase(void) { const char *p = getenv("QUALITY_PHASE"); return p ? atoi(p) : 0; }

static bool label_is(const char *want) {
  bool ok = !strcmp(cw_quality_label(), want);
  if (!ok) cw_error("label %s, expected %s", cw_quality_label(), want);
  return ok;
}

static void test_resolution(void) {
  if (!timed() || phase()) return;
  cw_quality_set(CW_QUALITY_HIGH, true);
  cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_75);
  CHECK(label_is("High")); /* Resolution alone is no Custom */
  for (int q = 0; q < CW_QUALITY_COUNT; q++) {
    cw_quality_set((cw_quality)q, true);
    int r = cw_gfx_get(CW_GFX_RESOLUTION);
    CHECK_EQ_INT(r, CW_GFX_RES_75);
    CHECK(label_is(cw_quality_name((cw_quality)q)));
    /* a round trip through the save word, from a different choice */
    uint32_t w = cw_quality_saved();
    cw_quality_set(q == CW_QUALITY_HIGH ? CW_QUALITY_LOW : CW_QUALITY_HIGH, true);
    cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_100);
    cw_quality_restore(w);
    int r2 = cw_gfx_get(CW_GFX_RESOLUTION);
    CHECK_EQ_INT(r2, CW_GFX_RES_75);
    CHECK(label_is(cw_quality_name((cw_quality)q)));
    cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_75);
    FOLD(w);
  }
  /* Auto: the cycle after Ultra */
  cw_quality_set(CW_QUALITY_ULTRA, true);
  cw_quality_cycle();
  CHECK(label_is("Auto"));
  int ra = cw_gfx_get(CW_GFX_RESOLUTION);
  CHECK_EQ_INT(ra, CW_GFX_RES_75);
  uint32_t w = cw_quality_saved();
  cw_quality_set(CW_QUALITY_LOW, true);
  cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_100);
  cw_quality_restore(w);
  CHECK(label_is("Auto"));
  int ra2 = cw_gfx_get(CW_GFX_RESOLUTION);
  CHECK_EQ_INT(ra2, CW_GFX_RES_75);
  cw_quality_set(CW_QUALITY_HIGH, true);
  cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_100);
}

/* the engine settings now as one comparable number */
static int settings(void) {
  int v = 0;
  for (int s = 0; s < CW_GFX_ENGINE_COUNT; s++) v = v * 8 + cw_gfx_get(s);
  return v;
}
static int vec(int res, int ssao, int sh, int aa, int fx) { return (((res * 8 + ssao) * 8 + sh) * 8 + aa) * 8 + fx; }

typedef struct {
  uint32_t word;
  const char *label;
  int level, settings;
} old_case;

static void check_old(const old_case *c) {
  cw_quality_set(CW_QUALITY_HIGH, true);
  cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_100);
  cw_quality_restore(c->word);
  bool ok = label_is(c->label);
  int lvl = cw_quality_get(), st = settings();
  CHECK(ok);
  CHECK_EQ_INT(lvl, c->level);
  CHECK_EQ_INT(st, c->settings);
  if (!ok || lvl != c->level || st != c->settings) cw_error("old word 0x%x", c->word);
  FOLD(st);
}

static void test_save_compat(void) {
  if (!timed() || phase()) return;
  enum { R100 = CW_GFX_RES_100, R75 = CW_GFX_RES_75 };
  const int HIGH = vec(R100, 1, CW_GFX_SHADOWS_HIGH, CW_GFX_AA_FULL, CW_QUALITY_HIGH);
  const old_case cases[] = {
    { 0, "High", CW_QUALITY_HIGH, HIGH }, /* nothing chosen: the choice before stands */
    { 1, "Low", CW_QUALITY_LOW, vec(R100, 0, CW_GFX_SHADOWS_LOW, CW_GFX_AA_FXAA, CW_QUALITY_LOW) },
    { 2, "Medium", CW_QUALITY_MEDIUM, vec(R100, 1, CW_GFX_SHADOWS_MEDIUM, CW_GFX_AA_SMAA, CW_QUALITY_MEDIUM) },
    { 3, "High", CW_QUALITY_HIGH, HIGH },
    { 4, "Ultra", CW_QUALITY_ULTRA, vec(R100, 1, CW_GFX_SHADOWS_ULTRA, CW_GFX_AA_FULL, CW_QUALITY_ULTRA) },
    /* Auto at its old rungs (High, Medium, Low), retried one rung up as before */
    { 5 | 0 << 3, "Auto", CW_QUALITY_HIGH, HIGH },
    { 5 | 1 << 3, "Auto", CW_QUALITY_HIGH, vec(R100, 1, CW_GFX_SHADOWS_MEDIUM, CW_GFX_AA_FULL, CW_QUALITY_HIGH) },
    { 5 | 2 << 3, "Auto", CW_QUALITY_MEDIUM, vec(R100, 0, CW_GFX_SHADOWS_MEDIUM, CW_GFX_AA_SMAA, CW_QUALITY_MEDIUM) },
    /* Custom: 75%, ambient occlusion off, shadows High, FXAA, effects Ultra */
    { 6 | 3 << 3 | 0 << 6 | 2 << 7 | 1 << 9 | 3 << 11, "Custom", CW_QUALITY_ULTRA,
      vec(R75, 0, CW_GFX_SHADOWS_HIGH, CW_GFX_AA_FXAA, CW_QUALITY_ULTRA) },
    /* Custom that is the old Low with every setting written out: Low */
    { 6 | 5 << 3 | 0 << 6 | 0 << 7 | 1 << 9 | 0 << 11, "Low", CW_QUALITY_LOW, vec(R100, 0, CW_GFX_SHADOWS_LOW, CW_GFX_AA_FXAA, CW_QUALITY_LOW) },
    /* Custom from High with SMAA (the old SMAA kept the scene's MSAA: Full) and shadows Low */
    { 6 | 5 << 3 | 1 << 6 | 0 << 7 | 2 << 9 | 2 << 11, "Custom", CW_QUALITY_HIGH,
      vec(R100, 1, CW_GFX_SHADOWS_LOW, CW_GFX_AA_FULL, CW_QUALITY_HIGH) },
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(cases); i++) check_old(&cases[i]);
  cw_quality_set(CW_QUALITY_HIGH, true);
}

static int lever_a = -1, lever_b = -1;
static void test_levers(void) {
  static const char *const FOUR[] = { "None", "Few", "Some", "All" };
  static const char *const TWO[] = { "Off", "On" };
  lever_a = cw_quality_lever_add("Debris", FOUR, 4, (cw_quality_lever_presets){ .very_low = 0, .low = 1, .medium = 2, .high = 3, .ultra = 3 });
  lever_b = cw_quality_lever_add("Glints", TWO, 2, (cw_quality_lever_presets){ .very_low = 0, .low = 0, .medium = 1, .high = 1, .ultra = 1 });
  CHECK(lever_a == CW_GFX_ENGINE_COUNT && lever_b == CW_GFX_ENGINE_COUNT + 1);
  static const int WANT_A[CW_QUALITY_COUNT] = { 0, 1, 2, 3, 3 }, WANT_B[CW_QUALITY_COUNT] = { 0, 0, 1, 1, 1 };
  for (int q = 0; q < CW_QUALITY_COUNT; q++) {
    cw_quality_set((cw_quality)q, true);
    int a = cw_gfx_get(lever_a), b = cw_gfx_get(lever_b);
    CHECK_EQ_INT(a, WANT_A[q]);
    CHECK_EQ_INT(b, WANT_B[q]);
    FOLD(a), FOLD(b);
  }
  cw_quality_set(CW_QUALITY_HIGH, true);
  if (!timed() || phase()) return;
  /* an old Custom word with levers (bits 13+, two bits each): Debris Some, Glints Off */
  old_case c = { 6 | 5 << 3 | 1 << 6 | 2 << 7 | 2 << 9 | 2 << 11 | 2 << 13 | 0 << 15, "Custom", CW_QUALITY_HIGH, vec(5, 1, 3, 3, 3) };
  check_old(&c);
  int a = cw_gfx_get(lever_a), b = cw_gfx_get(lever_b);
  CHECK_EQ_INT(a, 2);
  CHECK_EQ_INT(b, 0);
  /* and the new word keeps them */
  uint32_t w = cw_quality_saved();
  cw_quality_set(CW_QUALITY_LOW, true);
  cw_quality_restore(w);
  int a2 = cw_gfx_get(lever_a), b2 = cw_gfx_get(lever_b);
  CHECK_EQ_INT(a2, 2);
  CHECK_EQ_INT(b2, 0);
  CHECK(label_is("Custom"));
  cw_quality_set(CW_QUALITY_HIGH, true);
}

static void test_auto_jump(void) {
  if (!timed() || phase() || !getenv("CW_TEST_FRAME_MS")) return;
  cw_quality_set(CW_QUALITY_HIGH, true);
  cw_quality_auto(false, 19);
  cw_quality_set(CW_QUALITY_ULTRA, true);
  cw_quality_cycle(); /* Auto, from its top rung (High) */
  CHECK(label_is("Auto"));
  int at = -1;
  for (int f = 0; f < 600 && at < 0; f++) {
    cw_quality_auto_measure(true);
    if (cw_quality_get() == CW_QUALITY_VERY_LOW) at = f;
    for (uint64_t t = cw_profile_now_ns(); cw_profile_now_ns() - t < 200000;) {}
  }
  cw_info("auto_jump: Very Low after %d frames", at);
  CHECK(at >= 0 && at <= 180); /* 3 seconds of play at 60 Hz */
  cw_quality_set(CW_QUALITY_HIGH, true);
}

/* the graphics row (the shell, the quality page) steps through every choice and back */
static void test_cycle(void) {
  if (phase()) return;
  static const char *const ORDER[] = { "Very Low", "Low", "Medium", "High", "Ultra", "Auto", "Very Low" };
  cw_quality_set(CW_QUALITY_VERY_LOW, true);
  for (int i = 1; i < (int)CW_ARRAY_LEN(ORDER); i++) {
    cw_quality_cycle();
    CHECK(label_is(ORDER[i]));
  }
  for (int i = (int)CW_ARRAY_LEN(ORDER) - 2; i >= 0; i--) {
    cw_quality_cycle_back();
    CHECK(label_is(ORDER[i]));
  }
  cw_quality_set(CW_QUALITY_HIGH, true);
}

static void test_restart(void) {
  if (!timed() || !phase()) return;
  cw_shell_init(&(cw_shell_desc){ .title = "t", .max_players = 1 });
  if (phase() == 1) {
    cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_75);
    cw_quality_set(CW_QUALITY_MEDIUM, true);
    cw_shell_save_commit();
    CHECK(label_is("Medium"));
  } else {
    int r = cw_gfx_get(CW_GFX_RESOLUTION);
    CHECK_EQ_INT(r, CW_GFX_RES_75);
    CHECK(label_is("Medium"));
  }
}

CW_TEST_SUITE("quality", { "resolution", test_resolution }, { "save_compat", test_save_compat }, { "levers", test_levers },
              { "auto_jump", test_auto_jump }, { "cycle", test_cycle }, { "restart", test_restart })
