/* fog_valley: height fog. A low morning sun behind a colonnade, a
 * valley of far blocks, the ProceduralSky colouring the fog. The run is a
 * schedule of the HeightFog node's properties:
 *   seg 0   frames  1-20  analytic height fog (sky-coloured, the sun lobe)
 *   seg 1   frames 21-40  dense ground fog (steep height falloff)
 *   seg 2   frames 41-60  fog off (the control image)
 *
 * Debug state: fog.seg, fog.on (the height_fog pass ran last frame). */
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t tick, seg = -1, fog_on;

static void apply(int s) {
  cw_node f = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Valley/Fog");
  static const float density[] = { 0.02f, 0.25f, 0 }, falloff[] = { 0.1f, 0.9f, 0.1f };
  cw_node_set(f, "density", cw_val_float(density[s]));
  cw_node_set(f, "height_falloff", cw_val_float(falloff[s]));
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("fog.seg", &seg);
  cw_debug_i32("fog.on", &fog_on);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  int s = tick <= 20 ? 0 : tick <= 40 ? 1 : 2;
  if (s != seg) {
    seg = s;
    apply(s);
  }
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(cw_scene_app_tree()), &st);
  fog_on = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].name, "height_fog")) fog_on = st[i].ran;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fog_valley", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
