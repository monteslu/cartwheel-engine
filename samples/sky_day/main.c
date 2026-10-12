/* sky_day: the procedural sky and the day / night cycle. An outdoor
 * scene under a ProceduralSky that drives the sun (direction, colour,
 * energy) and is the background and the image-based environment (the balls
 * range from mirror chrome to rough paint). The run visits fixed times of
 * day, then lets the cycle run:
 *   seg 0   frames   1-30   noon
 *   seg 1   frames  31-60   16:30
 *   seg 2   frames  61-90   18:43, the sun just above the horizon (red light, long shadows)
 *   seg 3   frames  91-120  22:00, night (the sun light off, the night sky)
 *   seg 4   frames 121-     sunrise onward, day_speed 2 hours a second
 *
 * Debug state: sky.seg, sky.minutes (time of day x 60, rounded), sky.elev
 * (sun elevation, centidegrees), sky.light (the driven light's energy x
 * 1000), sky.updates (sky cubes rendered so far: only when the sun moved more
 * than env_update_angle). */
#include <math.h>

#include "cartwheel/debug.h"
#include "cartwheel/environment.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t tick, seg = -1, minutes, elev, light, updates;

static void set_time(float h, float speed) {
  cw_node sky = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Outdoor/Sky");
  cw_node_set(sky, "time_of_day", cw_val_float(h));
  cw_node_set(sky, "day_speed", cw_val_float(speed));
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("sky.seg", &seg);
  cw_debug_i32("sky.minutes", &minutes);
  cw_debug_i32("sky.elev", &elev);
  cw_debug_i32("sky.light", &light);
  cw_debug_i32("sky.updates", &updates);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  int s = tick <= 30 ? 0 : tick <= 60 ? 1 : tick <= 90 ? 2 : tick <= 120 ? 3 : 4;
  if (s != seg) {
    seg = s;
    static const float times[] = { 12, 16.5f, 18.72f, 22, 5.2f };
    set_time(times[s], s == 4 ? 2.0f : 0);
  }
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_sky_state st;
  cw_sky_get_state(cw_scene_app_tree(), &st);
  minutes = (int32_t)lrintf(st.time_of_day * 60);
  elev = (int32_t)lrintf(st.sun_elevation * 100);
  light = (int32_t)lrintf(st.sun_energy * 1000);
  updates = st.cube_updates;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "sky_day", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
