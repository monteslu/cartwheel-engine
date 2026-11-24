/* look_env_end: the end of a style change into a world that lights the scene. The note highway of style_switch over
 * two Backdrop worlds: the fire (no environment) with half blocks on a palette (the SCENE stage), and the stage, whose
 * environment lights the highway (64 texels a face, a capture over 12 frames, over the scene's own sky environment),
 * with a one-bit Bayer dither with the tone options (the DISPLAY stage). A style change fades the stage in over the
 * fire and the looks across the stages, as a rhythm game does it: the stage's environment off while the fire still
 * shows (a capture and its filter wait for the fade's end), and on again the frame the fire's weight is under 0.0005
 * (the fire hidden, the stage at its weight, just under 1: the last frame of the fade); the frame after, the weight is
 * 1 and the dither is alone.
 *
 *   round 0, tick  40  the fire and the half blocks (a cut); tick 70 the fade (120 ticks, smoothstep), then held
 *   round 1, tick 240  the same again: the fire cut back in, the fade into the stage, held (its environment is the kept
 *                      filter of its last capture of round 0)
 *
 * Gates look-env-end, look-env-end-keep and look-env-end-tone (filters and builds, on every host) and
 * look-env-end-timing (every frame timed through romdev).
 *
 * Switch (cw_test_env): LE_RES=50 sets the Resolution setting to 50% at tick 2, after the warm-up (as a player who
 * lowers it, or an Auto preset that starts low and is raised): the dither is then drawn at another size than the
 * warm-up's prewarm saw it.
 *
 * Debug state: le.tick, le.round (the last round begun; -1 before the first), le.phase (0 the fire, 1 the fade, 2 the
 * stage held), le.k (the fade's weight), le.env (the stage's environment on), le.ret_frame0/1 (the tick of each round's
 * environment-return frame), le.ret_filters0/1 (the stage's environment filters on that frame and the next),
 * le.filters (all of them so far), le.kept_uses (its kept filter taken back), le.late_builds (pass programs and
 * pipelines made after the warm-up, cw_pipeline_pass_builds). */
#include <math.h>
#include <stdlib.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/look.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/scene_file.h"

enum { FIRE, STAGE, NBD };
enum { ROUND = 200, START = 40, FADE_AT = 30, FADE = 120, BASE_TICK = 10 };
static int32_t tick, round_i = -1, phase, env_on = 1, ret_frame[2], ret_filters[2] = { -1, -1 }, filters, kept_uses, late_builds;
static float k_dbg;
static int32_t filters_at_ret, builds_base = -1;
static cw_node bd[NBD], bd_root[NBD];
static cw_look half, onebit;
static float op[NBD];

static void init(void *u) {
  (void)u;
  cw_node game = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Game");
  bd[FIRE] = cw_node_get(game, "Fire");
  bd[STAGE] = cw_node_get(game, "Stage");
  for (int i = 0; i < NBD; i++) {
    bd_root[i] = cw_backdrop_root(bd[i]);
    cw_node_set(bd_root[i], "visible", cw_val_bool(false)); /* hidden while out of sight, as a game does */
  }
  static const float PAL[] = { 0, 0, 0, 0.003f, 0.005f, 0.04f, 0.02f, 0.05f, 0.9f, 0.25f, 0.4f, 1, 1, 0.6f, 0.42f, 1, 1, 1,
                               1, 0.06f, 0.08f, 1, 0.65f, 0.05f, 0.05f, 1, 0.2f, 0.2f, 0.8f, 1, 1, 0.3f, 0.6f, 0.4f, 0.12f, 0.15f };
  cw_look_desc h = cw_look_default(CW_LOOK_HALFBLOCK);
  h.halfblock.rows = 90, h.halfblock.palette = PAL, h.halfblock.palette_count = (int)(sizeof PAL / sizeof PAL[0] / 3);
  half = cw_look_new(&h);
  cw_look_desc b = cw_look_default(CW_LOOK_DITHER_ORDERED);
  b.stage = CW_LOOK_STAGE_DISPLAY;
  b.dither.pattern = CW_DITHER_BAYER;
  b.dither.pixel = 2;
  b.dither.light[0] = 1.0f, b.dither.light[1] = 0.93f, b.dither.light[2] = 0.8f;
  b.dither.dark[0] = 0.012f, b.dither.dark[1] = 0.01f, b.dither.dark[2] = 0.016f;
  b.dither.hue_value = 1.15f, b.dither.grey_gain = 0.8f, b.dither.sharpen = 0.9f, b.dither.outline = 1;
  b.dither.gamma = 1.25f, b.dither.contrast = 1.25f;
  onebit = cw_look_new(&b);
  cw_debug_i32("le.tick", &tick);
  cw_debug_i32("le.round", &round_i);
  cw_debug_i32("le.phase", &phase);
  cw_debug_f32("le.k", &k_dbg);
  cw_debug_i32("le.env", &env_on);
  cw_debug_i32("le.ret_frame0", &ret_frame[0]);
  cw_debug_i32("le.ret_frame1", &ret_frame[1]);
  cw_debug_i32("le.ret_filters0", &ret_filters[0]);
  cw_debug_i32("le.ret_filters1", &ret_filters[1]);
  cw_debug_i32("le.filters", &filters);
  cw_debug_i32("le.kept_uses", &kept_uses);
  cw_debug_i32("le.late_builds", &late_builds);
}

/* a backdrop's opacity; its world shown while above 0 */
static void set_op(int i, float o) {
  if (o > 0 && !(op[i] > 0)) cw_node_set(bd_root[i], "visible", cw_val_bool(true));
  if (!(o > 0) && op[i] > 0) cw_node_set(bd_root[i], "visible", cw_val_bool(false));
  op[i] = o;
  cw_backdrop_set_opacity(bd[i], o);
}
static void set_env(bool on) {
  if (on == (env_on != 0)) return;
  env_on = on;
  cw_node_set(bd[STAGE], "environment", cw_val_bool(on));
}
static int32_t prop_int(cw_node n, const char *name) {
  cw_value v;
  return cw_node_get_prop(n, name, &v) && v.type == CW_T_INT ? (int32_t)v.i : -1;
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  tick++;
  if (tick == 2) {
    const char *res = cw_test_env("LE_RES");
    if (res && atoi(res) == 50) cw_gfx_set(CW_GFX_RESOLUTION, CW_GFX_RES_50);
  }
  if (tick == BASE_TICK) builds_base = cw_pipeline_pass_builds(); /* (the warm-up is over: a test's runs in its first frame) */
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  int r = tick >= START + ROUND ? 1 : 0, t = tick - START - ROUND * r;
  if (t >= 0) round_i = r;
  if (t == 0) { /* the fire and the half blocks, a cut */
    phase = 0;
    cw_backdrop_set_order(bd[FIRE], 0), cw_backdrop_set_order(bd[STAGE], 1);
    set_op(FIRE, 1), set_op(STAGE, 0);
    set_env(true);
    cw_pipeline_set_looks(p, half, (cw_look){ 0 }, 0, NULL);
  } else if (t >= FADE_AT && t < FADE_AT + FADE) {
    phase = 1;
    float lin = (float)(t - FADE_AT + 1) / FADE, k = lin * lin * (3 - 2 * lin);
    k_dbg = k;
    bool fire_lit = 1 - k > 0.0005f; /* (the game's own test: a world under 0.0005 is out) */
    if (fire_lit) set_op(FIRE, 1), set_op(STAGE, k);
    else {
      if (op[FIRE] > 0) { /* the environment-return frame: the fire out, the stage's environment on */
        ret_frame[r] = tick;
        filters_at_ret = prop_int(bd[STAGE], "env_filters");
      }
      set_op(FIRE, 0), set_op(STAGE, k);
    }
    set_env(!fire_lit);
    if (k < 1) cw_pipeline_set_looks(p, half, onebit, k, &(cw_look_mix){ .blend = CW_LOOK_DISSOLVE, .softness = 0.35f, .scale = 28 });
    else cw_pipeline_set_looks(p, onebit, (cw_look){ 0 }, 0, NULL);
  } else if (t >= FADE_AT + FADE) phase = 2;
  /* the filters on the environment-return frame and the next (both rendered before this tick) */
  if (ret_frame[r] && tick == ret_frame[r] + 2) ret_filters[r] = prop_int(bd[STAGE], "env_filters") - filters_at_ret;
  filters = prop_int(bd[STAGE], "env_filters");
  kept_uses = prop_int(bd[STAGE], "env_kept_uses");
  if (builds_base >= 0) late_builds = cw_pipeline_pass_builds() - builds_base;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc app = { .title = "look env end", .width = 1920, .height = 1080, .debug = true, .init = init, .fixed_update = fixed_update };
  return cw_scene_app("main.cwscene", &app);
}
