/* subviewport: pictures rendered into textures (cartwheel/subviewport.h),
 * and every way to show one. A small yard with a red box, a green ball and a
 * blue pillar, and five SubViewport nodes, all from main.cwscene:
 *
 *   Monitor   a camera of this world looking straight down, each frame, on a
 *             screen in the yard (a MeshInstance3D's viewport_texture)
 *   Photo     a camera of this world, ONCE: a still on the frame to the right
 *   Banner    a world of its own that is a 2D canvas (banner.cwscene), as the
 *             colour of a sign the yard's sun lights
 *   Portrait  a world of its own in 3D (portrait.cwscene: its own camera,
 *             light and background), in a UI panel (TextureRect) and as a
 *             Sprite2D, both by "viewport://Portrait"
 *   Hidden    WHEN_VISIBLE, on a panel that starts behind the main camera:
 *             it renders once, then only after the panel comes into view
 *
 * Pad south is the gate's control: the Monitor stops updating and its screen
 * holds a frozen picture while the yard keeps moving. Debug fields: renders
 * per viewport, monitor_live / photo_once / hidden_idle / hidden_woke (what
 * the gate holds), and each viewport's last CPU ms (never compared). */
#include <stdio.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/subviewport.h"

static cw_node world, level, red, ball, behind, monitor, photo, banner, portrait, hidden;
static cw_action freeze_action;
static float clock_s;

static int32_t renders[5], monitor_live, photo_once, hidden_idle, hidden_woke, frozen, hidden_at_200, monitor_at_60;
static float cpu_ms[5];

static void init(void *u) {
  (void)u;
  freeze_action = cw_action_add("freeze", 0.5f);
  cw_action_bind_text(freeze_action, "pad:south");
  world = cw_node_get(cw_tree_root(cw_scene_app_tree()), "World");
  level = cw_node_get(world, "Level");
  red = cw_node_get(level, "RedBox");
  ball = cw_node_get(level, "GreenBall");
  behind = cw_node_get(level, "Behind");
  monitor = cw_node_get(world, "Monitor");
  photo = cw_node_get(world, "Photo");
  banner = cw_node_get(world, "Banner");
  portrait = cw_node_get(world, "Portrait");
  hidden = cw_node_get(world, "Hidden");
  cw_debug_field("renders", renders, CW_DBG_I32, 5);
  cw_debug_i32("monitor_live", &monitor_live);
  cw_debug_i32("photo_once", &photo_once);
  cw_debug_i32("hidden_idle", &hidden_idle);
  cw_debug_i32("hidden_woke", &hidden_woke);
  cw_debug_i32("frozen", &frozen);
  cw_debug_f32_array("cpu_ms", cpu_ms, 5); /* host clock: never compared */
}

static void fixed(void *u, double dt) {
  (void)u;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  clock_s += (float)dt;
  float s, c;
  cw_sincos(clock_s * 1.3f, &s, &c);
  cw_node3d_set_rotation(red, (t3_vec3){ 0, clock_s * 0.9f, 0 });
  cw_node3d_set_position(ball, (t3_vec3){ 0.6f + 1.5f * c, 0.45f, 1.2f + 1.1f * s });

  /* #region own_world */
  /* a world of its own is another tree: reach into it through the viewport */
  cw_node turntable = cw_node_get(cw_subviewport_root(portrait), "Turntable");
  cw_node3d_set_rotation(turntable, (t3_vec3){ 0, clock_s * 1.1f, 0 });
  cw_node slider = cw_node_get(cw_subviewport_root(banner), "Slider");
  cw_node_set(slider, "position", cw_val_vec2(130 + 110 * (1 + s), 112));
  /* #endregion */

  /* the panel that wears the Hidden viewport comes round into view */
  if (tick == 240) cw_node3d_set_position(behind, (t3_vec3){ -0.2f, 0.6f, 3.4f });

  if (cw_action_just_pressed(0, freeze_action)) {
    frozen = 1;
    cw_node_set(monitor, "update_mode", cw_val_enum("DISABLED"));
  }

  cw_node all[5] = { monitor, photo, banner, portrait, hidden };
  for (int i = 0; i < 5; i++) {
    renders[i] = cw_subviewport_renders(all[i]);
    cpu_ms[i] = cw_subviewport_last_cpu_ms(all[i]);
  }
  if (tick == 60) monitor_at_60 = renders[0];
  if (tick == 200) hidden_at_200 = renders[4];
  if (tick > 60) monitor_live = renders[0] > monitor_at_60 + (int32_t)(tick - 60) / 2; /* still rendering, most frames */
  photo_once = renders[1] == 1;
  if (tick >= 200) hidden_idle = hidden_at_200 == 1;        /* one render to fill it, none while nothing showed it */
  if (tick > 300) hidden_woke = renders[4] > hidden_at_200 + 30; /* and it runs once it is in view */
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "subviewport", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
