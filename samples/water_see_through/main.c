/* water_see_through: what lies just under the water seen through the surface, blurred and fading with depth
 * (Water3D see_through). Flat slabs lie with their tops 0.05, 0.2, 0.5, 1 and 2 m under the surface in a row, a box
 * stands half in the water, a craft's stern dips and a figure's legs stand in it, over a deep floor. The tests/
 * water-see-through.mjs gate reads the slabs' pixels against their depth by day and night, over GL and WebGPU.
 * Debug state: tick, water.see_light (the light scaling what shows through). */
#include "cartwheel/debug.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

static uint32_t tick;
static float see_light;

static void init(void *u) {
  (void)u;
  cw_debug_u32("water.tick", &tick);
  cw_debug_f32("water.see_light", &see_light);
}
static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_water_stats st;
  cw_water_get_stats(cw_scene_app_tree(), &st);
  see_light = st.see_light;
}
static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "water_see_through", .width = 960, .height = 540, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
