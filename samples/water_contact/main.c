/* water_contact: where things go into the water. A grey shin (a cylinder 0.12 m across) and a hull (a box 2.8 x 1.1 m
 * drawing 0.25 m) ride an open swell of four Gerstner waves summing to 2.16 m (a sea a craft races on), each held at
 * the drawn surface's height over its middle, so the contact moves with the waves through crests and troughs. The
 * camera follows the shin's contact: 3 m out and 2.2 m up (36 degrees down) by default.
 *
 * Test switches (the tests/water-contact.mjs gate):
 *   CW_TEST_WC_PHASE  crest | trough | rise | fall: the waves' clock is set so that the shin's contact is at that phase
 *                     on frame 90 (the highest, the lowest, mid-height going up, mid-height going down, within the
 *                     first minute of the clock)
 *   CW_TEST_WC_VIEW   steep (default) | graze (4 m out, 0.6 m up) | hull (the hull's side, 4.5 m out, 1.5 m up)
 *   CW_TEST_WC_NONE   1: the shin and the hull left out (the gate must then find no object)
 *   CW_TEST_WC_FOAM   1: every foam and lace layer on, as a racing game's sea has them (crest foam 1, lace 0.3, soft
 *                     foam, the foam pattern 7), and a slow wake at the shin each tick (0.5 m/s): foam must not
 *                     hide the contact
 *   CW_TEST_WC_TILT   degrees the shin leans sideways in the picture (about the view's axis, its contact where it
 *                     was): a slanted outline, along which a coarse refraction target steps (tests/water-contact-edge.mjs)
 * Each tick the shin and the hull stamp their collars (cw_water_collar: the ring of piled-up water and bubbles where
 * each stands through the surface; the hull's as three rings along it).
 *   CW_TEST_WATER_COLLAR 0: no collars (the gates' control)
 *   CW_TEST_WC_DECK   1: in the shin's place a dark craft's deck (Root/Deck, 1.1 x 2.8 m, 0.6 deep) 0.2 m under the
 *                     drawn surface, the craft seen end on as from behind (narrower than the ring the water finds the
 *                     ground by), the shin hidden: with every foam layer the wake floats over the deck, no dark slab
 * Debug state: wc.collar, wc.collar_foam (the collar layer's cells and its most bubbles), wc.tick, wc.h (the surface at the shin's near side, over the rest level, m), wc.hh (at the hull's near side). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

#define T_SHOT 90
static uint32_t tick, fine_cells;
static float fine_foam;
static float h_shin, h_hull;
static int view, foam, collar = 1, deck;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

/* the waves' clock for a phase at the shin (0, 0.06): scanned over the first minute in 20 ms steps */
static double phase_time(cw_node w, const char *ph) {
  double best = 0, bv = 0;
  int first = 1;
  for (int i = 1; i < 3000; i++) {
    double t = i * 0.02;
    float h = cw_water_height_at(w, 0, 0.06f, t), hp = cw_water_height_at(w, 0, 0.06f, t - 0.02);
    float v;
    if (!strcmp(ph, "crest")) v = h;
    else if (!strcmp(ph, "trough")) v = -h;
    else if (!strcmp(ph, "rise")) v = h > hp ? (h - hp) - fabsf(h) : -1e9f;
    else v = h < hp ? (hp - h) - fabsf(h) : -1e9f;
    if (first || v > bv) best = t, bv = v, first = 0;
  }
  return best;
}

static void init(void *u) {
  (void)u;
  cw_debug_u32("wc.tick", &tick);
  cw_debug_f32("wc.h", &h_shin);
  cw_debug_f32("wc.hh", &h_hull);
  cw_debug_u32("wc.collar", &fine_cells);
  cw_debug_f32("wc.collar_foam", &fine_foam);
}

/* the switches, read on tick 2 (on a cart a test's environment arrives with frame 1, after init) */
static void switches(void) {
  cw_node w = node("Root/Water");
  const char *ph = cw_test_env("CW_TEST_WC_PHASE");
  if (ph && *ph) cw_node_set(w, "time_offset", cw_val_float((float)(phase_time(w, ph) - T_SHOT * cw_fixed_dt())));
  const char *v = cw_test_env("CW_TEST_WC_VIEW");
  view = v && !strcmp(v, "graze") ? 1 : v && !strcmp(v, "hull") ? 2 : 0;
  const char *fm = cw_test_env("CW_TEST_WC_FOAM");
  foam = fm && atoi(fm) > 0;
  if (foam) {
    cw_node_set(w, "crest_foam", cw_val_float(1));
    cw_node_set(w, "foam_lace", cw_val_float(0.3f));
    cw_node_set(w, "foam_soft", cw_val_float(1));
    cw_node_set(w, "foam_scale", cw_val_float(7));
  }
  const char *tilt = cw_test_env("CW_TEST_WC_TILT");
  if (tilt && *tilt) cw_node_set(node("Root/Shin"), "rotation", cw_val_vec3(0, 0, (float)atof(tilt) * 3.14159265f / 180.0f));
  const char *dk = cw_test_env("CW_TEST_WC_DECK");
  deck = dk && atoi(dk) > 0;
  if (deck) {
    cw_node_set(node("Root/Deck"), "visible", cw_val_bool(true));
    cw_node_set(node("Root/Shin"), "visible", cw_val_bool(false));
  }
  const char *co = cw_test_env("CW_TEST_WATER_COLLAR");
  if (co && *co && atoi(co) == 0) collar = 0;
  const char *none = cw_test_env("CW_TEST_WC_NONE");
  if (none && atoi(none) > 0) {
    cw_node_set(node("Root/Shin"), "visible", cw_val_bool(false));
    cw_node_set(node("Root/Hull"), "visible", cw_val_bool(false));
    cw_node_set(node("Root/Deck"), "visible", cw_val_bool(false));
    collar = 0;
  }
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (tick == 2) switches();
  if (foam) cw_water_wake(node("Root/Water"), 0, 0, 0.5f, 0, deck ? 1.2f : 0.3f, 0.02f, 0.6f);
  /* #region collar */
  if (collar && tick >= 2) {
    cw_node w = node("Root/Water");
    if (!deck) cw_water_collar(w, 0, 0, 0.06f, 0, 0, 0.8f); /* (a sunk deck pierces nothing) */
    for (int k = -1; k <= 1; k++) cw_water_collar(w, -4 + 0.85f * (float)k, 0, 0.55f, 0, 0, 0.8f);
  }
  /* #endregion */
}

/* (in render, at the drawn surface's own time: the tick before plus alpha, as the water draws it) */
static void render(void *u, double alpha) {
  (void)u;
  cw_node w = node("Root/Water");
  double t = cw_water_time(w) + (alpha - 1.0) * cw_fixed_dt();
  h_shin = cw_water_height_at(w, 0, 0.06f, t); /* (on the near side, where the camera looks) */
  h_hull = cw_water_height_at(w, -4, 0.55f, t);
  {
    cw_water_stats st;
    cw_water_get_stats(cw_scene_app_tree(), &st);
    fine_cells = (uint32_t)st.collar_cells, fine_foam = st.collar_foam;
  }
  cw_node_set(node("Root/Shin"), "position", cw_val_vec3(0, h_shin, 0));
  cw_node_set(node("Root/Hull"), "position", cw_val_vec3(-4, h_hull + 0.05f, 0));
  if (deck) cw_node_set(node("Root/Deck"), "position", cw_val_vec3(0, h_shin - 0.5f, 0));
  static const float cam[3][3] = { { 0, 2.2f, 3 }, { 0, 0.6f, 4 }, { -4, 1.5f, 4.5f } };
  const float *c = cam[view];
  float hy = view == 2 ? h_hull : h_shin;
  /* (looking at the contact on the near side) */
  float pitch = -atan2f(c[1], c[2] - (view == 2 ? 0.55f : 0.06f));
  cw_node_set(node("Root/Camera"), "position", cw_val_vec3(c[0], hy + c[1], c[2]));
  cw_node_set(node("Root/Camera"), "rotation", cw_val_vec3(pitch, 0, 0));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "water_contact", .width = 1920, .height = 1080, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
