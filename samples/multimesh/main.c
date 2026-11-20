/* multimesh: many instances of one mesh from a buffer the game fills
 * (cartwheel/multimesh.h). Three MultiMeshInstance3D nodes from main.cwscene:
 *
 *   Field   20000 coloured blocks scattered over a 300 m field, filled once
 *           in code, sorted into 24 m cells that are culled one by one: the
 *           camera turns, and visible_instances follows what is in view
 *   Posts   a grid filled by the scene file alone (fill = GRID): no code
 *   Swarm   400 spheres whose buffer is rewritten every tick, in one draw
 *           (chunk_size = 0), casting shadows
 *
 * Every instance casts and receives the sun's shadow. `count <n>` over the
 * debug channel refills the field with n instances (the 100000-instance
 * measurement in the guide). Pad south is the gate's control: the field's
 * visible_instance_count drops to 0 and nothing of it is drawn. Debug
 * fields: instances, chunks, visible_chunks, visible_instances, draws, the
 * probes the gate holds (all_drawn, culled, swarm_live), the buffer's hash,
 * build_ms (host clock, never compared). */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/multimesh.h"
#include "cartwheel/scene_file.h"

#define SWARM 400

static cw_node level, field, posts, swarm, camera;
static cw_action hide_action;
static float clock_s;
static int want_count = 20000;

static int32_t instances, chunks, visible_chunks, visible_instances, draws, posts_n, all_drawn, culled, swarm_live, min_visible = 1 << 30,
               max_visible, swarm_builds0;
static uint32_t buffer_hash;
static float build_ms;

/* #region fill */
/* fill the field: positions, a turn about Y, a height, and a colour per instance */
static void fill_field(int n) {
  cw_rng r;
  cw_rng_seed(&r, 7);
  cw_multimesh_set_instance_count(field, n);
  float *m = cw_multimesh_transforms(field);
  buffer_hash = CW_HASH32_INIT;
  for (int i = 0; i < n; i++, m += 16) {
    float x = cw_rng_range(&r, -150, 150);
    float z = cw_rng_range(&r, -260, 60);
    float turn = cw_rng_range(&r, 0, 6.2831853f);
    float h = cw_rng_range(&r, 0.5f, 2.2f);
    float s, c;
    cw_sincos(turn, &s, &c);
    t3_mat4 t = { { c, 0, -s, 0, 0, h, 0, 0, s, 0, c, 0, x, 0, z, 1 } };
    memcpy(m, t.e, sizeof t.e);
    float hue = cw_rng_range(&r, 0, 1);
    cw_multimesh_set_color(field, i, (t3_color){ 0.25f + 0.6f * hue, 0.55f - 0.3f * hue, 0.15f + 0.2f * (1 - hue) });
    buffer_hash = cw_hash32(t.e, sizeof t.e, buffer_hash);
  }
  cw_multimesh_touch(field);
}
/* #endregion */

static void cmd_count(int argc, const char **argv, cw_reply *r) {
  want_count = argc > 1 ? atoi(argv[1]) : 20000;
  cw_reply_printf(r, "{\"count\":%d}", want_count);
}

static void init(void *u) {
  (void)u;
  hide_action = cw_action_add("hide", 0.5f);
  cw_action_bind_text(hide_action, "pad:south");
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  field = cw_node_get(level, "Field");
  posts = cw_node_get(level, "Posts");
  swarm = cw_node_get(level, "Swarm");
  camera = cw_node_get(level, "Camera");
  fill_field(want_count);
  cw_multimesh_set_instance_count(swarm, SWARM);
  for (int i = 0; i < SWARM; i++) {
    float k = (float)i / SWARM;
    cw_multimesh_set_color(swarm, i, (t3_color){ 0.9f, 0.3f + 0.6f * k, 0.1f + 0.8f * (1 - k) });
  }
  cw_debug_command("count", "count <n>: refill the field with n instances", cmd_count);
  cw_debug_i32("instances", &instances);
  cw_debug_i32("chunks", &chunks);
  cw_debug_i32("visible_chunks", &visible_chunks);
  cw_debug_i32("visible_instances", &visible_instances);
  cw_debug_i32("draws", &draws);
  cw_debug_i32("posts", &posts_n);
  cw_debug_i32("all_drawn", &all_drawn);
  cw_debug_i32("culled", &culled);
  cw_debug_i32("swarm_live", &swarm_live);
  cw_debug_field("buffer_hash", &buffer_hash, CW_DBG_U32, 1);
  cw_debug_f32("build_ms", &build_ms); /* host clock: never compared */
  cw_debug_i32("min_visible", &min_visible);
  cw_debug_i32("max_visible", &max_visible);
}

static void fixed(void *u, double dt) {
  (void)u;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  clock_s += (float)dt;
  static int filled = 20000;
  if (want_count != filled) {
    filled = want_count;
    fill_field(filled);
  }

  /* #region swarm */
  /* a buffer rewritten every tick: write the array in place, then touch */
  float *m = cw_multimesh_transforms(swarm);
  for (int i = 0; i < SWARM; i++, m += 16) {
    float a = clock_s * (0.6f + 0.002f * (float)i) + 0.37f * (float)i, s, c, s2, c2;
    cw_sincos(a, &s, &c);
    cw_sincos(a * 0.7f + (float)i, &s2, &c2);
    float rad = 5 + 3 * s2;
    m[12] = rad * c;
    m[13] = 2.5f * c2;
    m[14] = rad * s;
  }
  cw_multimesh_touch(swarm);
  /* #endregion */

  /* the camera pans across the field: the cells in view change */
  float s, c;
  cw_sincos(clock_s * 0.35f, &s, &c);
  cw_node3d_look_at(camera, (t3_vec3){ 60 * s, 0, -30 + 10 * c }, (t3_vec3){ 0, 1, 0 });

  if (cw_action_just_pressed(0, hide_action)) cw_node_set(field, "visible_instance_count", cw_val_int(0));

  cw_multimesh_stats st, sw;
  cw_multimesh_get_stats(field, &st);
  cw_multimesh_get_stats(swarm, &sw);
  instances = st.instances;
  chunks = st.chunks;
  visible_chunks = st.visible_chunks;
  visible_instances = st.visible_instances;
  draws = st.draws;
  build_ms = st.build_ms;
  posts_n = cw_multimesh_instance_count(posts);
  if (tick > 5) {
    if (visible_instances < min_visible) min_visible = visible_instances;
    if (visible_instances > max_visible) max_visible = visible_instances;
    all_drawn = instances == filled && chunks > 20 && st.builds == 1 + (filled != 20000); /* built once: a still buffer costs no rebuilds */
    /* never the whole field, never nothing, and the pan changes what is in view */
    culled = max_visible < filled * 9 / 10 && min_visible > filled / 50 && max_visible > min_visible + filled / 20;
    if (tick == 30) swarm_builds0 = sw.builds;
    if (tick > 60) swarm_live = sw.chunks == 1 && sw.draws == 1 && sw.builds > swarm_builds0 + 20;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "multimesh", .width = 640, .height = 360, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
