/* style_switch: a rhythm game's style changes, timed. A note highway (the backdrop sample's) over three Backdrop worlds
 * that start at opacity 0 with their worlds hidden (a fire canyon: flames, embers and haze; a stage: three volumetric
 * spots through haze, fire on its towers; a night sea: Water3D under the moon, lighting the highway as its
 * environment), and three looks made in init (ASCII and half blocks with a palette at the SCENE stage, a one-bit Bayer
 * dither with the tone options at the DISPLAY stage). The highway's screen-space reflections (SSR with ssr_backdrop)
 * are off until the sea comes in; init gives the engine those settings to prewarm (cw_post_prewarm). On a schedule of
 * ticks it shows each world and each look for the first time, then does every change again with everything already
 * shown once:
 *
 *   event 0, tick  60  the fire fades in (its world shown, opacity 0 to 1 over 20 ticks)
 *   event 1, tick 120  the plain scene to ASCII (a fade)
 *   event 2, tick 180  ASCII to half blocks (a dissolve)
 *   event 3, tick 240  half blocks to the one-bit dither (sharpened, outlined: a fade across the stages)
 *   event 4, tick 300  the stage fades in over the fire (then the fire is hidden)
 *   event 5, tick 360  the dither to the plain scene
 *   event 6, tick 420  the sea fades in over the stage with SSR and ssr_backdrop turned on (the stage then hidden; at
 *                      the event's end the stage is back, the sea hidden and SSR off)
 *   events 7 to 13, ticks 480 to 840: the same changes again (fire over the stage, ...), already warm
 *
 * Gate backdrop-look-prewarm times every frame through romdev (GL and WebGPU) and holds the frames of each change to
 * the run's usual frame; its control is the engine's warm-up without the prewarm (CW_TEST_NO_PREWARM=1).
 *
 * Switches (cw_test_env): SW_KEEP_VISIBLE=1 leaves the worlds shown at opacity 0 (no hiding).
 * Debug state: sw.tick, sw.event (the last event begun; -1 before the first), sw.fire_renders, sw.stage_renders,
 * sw.sea_renders, sw.prewarms (prewarm renders of the worlds, the looks and the post passes so far). */
#include <math.h>
#include <stdlib.h>

#include "cartwheel/backdrop.h"
#include "cartwheel/debug.h"
#include "cartwheel/look.h"
#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

enum { NBD = 3, EVENTS = 7, ROUND = 420 }; /* worlds; changes a round, ticks between two rounds' changes */
static int32_t tick, event = -1, fire_renders, stage_renders, sea_renders, prewarms;
static cw_node game, post, bd[NBD], bd_root[NBD];
static cw_look looks[4]; /* 0 none, 1 ASCII, 2 half blocks, 3 one bit */
static float op[NBD];
static bool keep_visible, read_switches;

static void init(void *u) {
  (void)u;
  game = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Game");
  bd[0] = cw_node_get(game, "Fire");
  bd[1] = cw_node_get(game, "Stage");
  bd[2] = cw_node_get(game, "Sea");
  post = cw_node_get(game, "Post");
  for (int i = 0; i < NBD; i++) {
    bd_root[i] = cw_backdrop_root(bd[i]);
    cw_node_set(bd_root[i], "visible", cw_val_bool(false)); /* hidden while out of sight, as a game does */
  }
  cw_look_desc a = cw_look_default(CW_LOOK_ASCII);
  a.ascii.rows = 60;
  looks[1] = cw_look_new(&a);
  static const float PAL[] = { 0, 0, 0, 0.003f, 0.005f, 0.04f, 0.02f, 0.05f, 0.9f, 0.25f, 0.4f, 1, 1, 0.6f, 0.42f, 1, 1, 1,
                               1, 0.06f, 0.08f, 1, 0.65f, 0.05f, 0.05f, 1, 0.2f, 0.2f, 0.8f, 1, 1, 0.3f, 0.6f, 0.4f, 0.12f, 0.15f };
  cw_look_desc h = cw_look_default(CW_LOOK_HALFBLOCK);
  h.halfblock.rows = 90, h.halfblock.palette = PAL, h.halfblock.palette_count = (int)(sizeof PAL / sizeof PAL[0] / 3);
  looks[2] = cw_look_new(&h);
  cw_look_desc b = cw_look_default(CW_LOOK_DITHER_ORDERED);
  b.stage = CW_LOOK_STAGE_DISPLAY;
  b.dither.pattern = CW_DITHER_BAYER;
  b.dither.pixel = 2;
  b.dither.light[0] = 1.0f, b.dither.light[1] = 0.93f, b.dither.light[2] = 0.8f;
  b.dither.dark[0] = 0.012f, b.dither.dark[1] = 0.01f, b.dither.dark[2] = 0.016f;
  b.dither.hue_value = 1.15f, b.dither.grey_gain = 0.8f, b.dither.sharpen = 0.9f, b.dither.outline = 1;
  b.dither.gamma = 1.25f, b.dither.contrast = 1.25f;
  looks[3] = cw_look_new(&b);
  /* the post passes the sea brings (the highway's sharp reflections of itself and of the backdrops): prewarmed by the
   * engine at the end of its warm-up, as it prewarms the worlds and the looks */
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  cw_post_settings later = *cw_pipeline_settings(p);
  later.ssr = later.ssr_backdrop = true;
  cw_post_prewarm(p, &later);
  cw_debug_i32("sw.tick", &tick);
  cw_debug_i32("sw.event", &event);
  cw_debug_i32("sw.fire_renders", &fire_renders);
  cw_debug_i32("sw.stage_renders", &stage_renders);
  cw_debug_i32("sw.sea_renders", &sea_renders);
  cw_debug_i32("sw.prewarms", &prewarms);
}

/* the prewarms made so far (both worlds' and the looks'), read in the app's render: the warm-up's last render runs it
 * too, after the frame's prewarms */
static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  if (!bd[0].id) return;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  prewarms = cw_pipeline_look_prewarms(p) + cw_pipeline_post_prewarms(p);
  for (int i = 0; i < NBD; i++) prewarms += cw_backdrop_prewarms(bd[i]);
}

/* a backdrop's opacity; its world shown while above 0 */
static void set_op(int i, float o) {
  if (o > 0 && !(op[i] > 0)) {
    cw_node_set(bd_root[i], "visible", cw_val_bool(true));
    for (int c = 0; c < cw_node_child_count(bd_root[i]); c++) { /* a fire coming in starts already burning */
      cw_node ch = cw_node_child(bd_root[i], c);
      if (cw_node_is(ch, "GPUParticles3D")) cw_particles_restart(ch);
    }
  }
  if (!(o > 0) && op[i] > 0 && !keep_visible) cw_node_set(bd_root[i], "visible", cw_val_bool(false));
  op[i] = o;
  cw_backdrop_set_opacity(bd[i], o);
}

/* the looks' change at tick k of a 20 tick blend from look `from` to look `to` */
static void looks_at(int from, int to, int k, cw_look_blend how) {
  float t = CW_CLAMP(k / 20.0f, 0.0f, 1.0f);
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  if (t >= 1) cw_pipeline_set_looks(p, looks[to], (cw_look){ 0 }, 0, NULL);
  else cw_pipeline_set_looks(p, looks[from], looks[to], t, &(cw_look_mix){ .blend = how, .softness = 0.35f, .scale = 28 });
}

static void fixed_update(void *u, double dt) {
  (void)u, (void)dt;
  if (!read_switches && cw_frame_index() >= 1) {
    read_switches = true;
    const char *kv = cw_test_env("SW_KEEP_VISIBLE");
    keep_visible = kv && atoi(kv);
    if (keep_visible)
      for (int i = 0; i < NBD; i++) cw_node_set(bd_root[i], "visible", cw_val_bool(true));
  }
  tick++;
  /* the two rounds: the same changes ROUND ticks apart */
  int r = tick >= ROUND + 60 ? 1 : 0, t = tick - ROUND * r;
  int e = t >= 60 ? (t - 60) / 60 : -1;
  if (e >= 0 && e < EVENTS) event = e + EVENTS * r;
  int k = e >= 0 ? (t - 60) % 60 : 0;
  switch (e) {
  case 0:
    if (!r) set_op(0, CW_MIN(1.0f, (k + 1) / 20.0f));
    else {
      cw_backdrop_set_order(bd[0], 1), cw_backdrop_set_order(bd[1], 0);
      set_op(0, CW_MIN(1.0f, (k + 1) / 20.0f));
      if (k == 19) set_op(1, 0);
    }
    break;
  case 1: looks_at(0, 1, k + 1, CW_LOOK_FADE); break;
  case 2: looks_at(1, 2, k + 1, CW_LOOK_DISSOLVE); break;
  case 3: looks_at(2, 3, k + 1, CW_LOOK_FADE); break;
  case 4: /* the stage over the fire, then the fire hidden */
    cw_backdrop_set_order(bd[0], 0), cw_backdrop_set_order(bd[1], 1);
    set_op(1, CW_MIN(1.0f, (k + 1) / 20.0f));
    if (k == 19) set_op(0, 0);
    break;
  case 5: looks_at(3, 0, k + 1, CW_LOOK_FADE); break;
  case 6: /* the sea over the stage, the highway's reflections on with it; at the end all back as it was */
    if (k == 0) cw_node_set(post, "ssr", cw_val_bool(true)), cw_node_set(post, "ssr_backdrop", cw_val_bool(true));
    if (k < 59) set_op(2, CW_MIN(1.0f, (k + 1) / 20.0f));
    if (k == 19) set_op(1, 0);
    if (k == 59) {
      set_op(1, 1), set_op(2, 0);
      cw_node_set(post, "ssr", cw_val_bool(false)), cw_node_set(post, "ssr_backdrop", cw_val_bool(false));
    }
    break;
  default: break;
  }
  fire_renders = cw_backdrop_renders(bd[0]);
  stage_renders = cw_backdrop_renders(bd[1]);
  sea_renders = cw_backdrop_renders(bd[2]);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc app = { .title = "style switch", .width = 1920, .height = 1080, .debug = true, .init = init, .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &app);
}
