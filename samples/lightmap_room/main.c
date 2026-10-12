/* lightmap_room: baked lighting (M3.5). A room open at the front with a
 * window in its green right wall, a red left wall, two blocks; the sun comes
 * through the window, the hemisphere sky through the opening. LightmapGI
 * bakes the STATIC walls and blocks on the first frame (generated uv1, 64
 * rays a texel, 2 bounces, an SH probe grid 1.5 m apart), twice, and
 * compares the bytes; the sun stays real-time (three.c's shadow map). A
 * dynamic ball rolls through the room.
 *
 * Debug state: lm.tick, lm.hash_lo / lm.hash_hi (the bake's FNV-1a 64
 * fingerprint, compared on every host: the bake is the same bytes
 * everywhere), lm.same (the second bake matched the first), lm.texels,
 * lm.probes, lm.bake_ms (reported, never compared). */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t tick, same, texels, probes;
static uint32_t hash_lo, hash_hi;
static float bake_ms;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("lm.tick", &tick);
  cw_debug_u32("lm.hash_lo", &hash_lo);
  cw_debug_u32("lm.hash_hi", &hash_hi);
  cw_debug_i32("lm.same", &same);
  cw_debug_i32("lm.texels", &texels);
  cw_debug_i32("lm.probes", &probes);
  cw_debug_f32("lm.bake_ms", &bake_ms);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  float x = -3 + 6 * (float)(tick % 120) / 120.0f;
  cw_node3d_set_position(node("Room/Ball"), (t3_vec3){ x, 0.6f, 2 });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_node gi = node("Room/GI");
  cw_value v;
  if (cw_node_get_prop(gi, "hash", &v) && v.s && v.s[0]) {
    unsigned long long h = strtoull(v.s, NULL, 16);
    hash_lo = (uint32_t)h;
    hash_hi = (uint32_t)(h >> 32);
  }
  if (cw_node_get_prop(gi, "reproducible", &v)) same = v.b;
  if (cw_node_get_prop(gi, "texels", &v)) texels = (int32_t)v.i;
  if (cw_node_get_prop(gi, "probes", &v)) probes = (int32_t)v.i;
  if (cw_node_get_prop(gi, "bake_ms", &v)) bake_ms = (float)v.f;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "lightmap_room", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
