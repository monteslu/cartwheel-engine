/* cloth_demo: cloth and ropes as scene nodes (cartwheel/cloth.h), built
 * from main.cwscene and run by the scene runner.
 *
 *   Flag    a Cloth3D pinned along its pole edge in a gusty wind
 *   Hero    a KinematicBody3D (the Circler type below) walking a circle; its
 *           Cape is a Cloth3D pinned at the shoulders that follows the hero,
 *           stays near its back (skin_distance) and collides with its capsule
 *   Rope    a Rope3D from a beam holding a 20 kg RigidBody3D crate, which
 *           is shoved sideways at 1.5 s and swings on it
 *   Chain   a Rope3D in LINKS mode: Box3D capsule links holding a weight,
 *           shoved at 2.5 s
 *
 * Input-free: every host runs the same ticks. Debug fields: flag.hash,
 * cape.hash, rope.hash, chain.hash (the nodes' simulation hashes), crate.pos,
 * weight.pos, hero.pos, rope.tension (N), cape.min_gap (the closest a cape
 * particle came to the hero's capsule axis, mm), cloth.us (the three cloths'
 * step time, microseconds, not compared), phys.hash. */
#include <math.h>

#include "cartwheel/cloth.h"
#include "cartwheel/debug.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static struct {
  uint32_t flag_hash, cape_hash, rope_hash, chain_hash, phys_hash;
  float crate[3], weight[3], hero[3];
  float tension;
  int32_t cape_gap_mm;
  float cloth_us;
  int32_t tick;
} G;

/* ── Circler: a kinematic body walking a circle, facing where it goes ─── */

/* #region circler */
typedef struct circler {
  t3_vec3 centre;
  int32_t ticks;
} circler;

static void circler_ready(cw_node n, void *data) { ((circler *)data)->centre = cw_node3d_position(n); }

static void circler_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  circler *c = data;
  c->ticks++;
  /* 1.6 m circle at 1.2 rad/s after a second of standing still */
  float a = c->ticks < 60 ? 0 : (float)(c->ticks - 60) * (1.2f / 60.0f);
  float s, co;
  cw_sincos(a, &s, &co);
  cw_node3d_set_position(n, (t3_vec3){ c->centre.x + 1.6f * co - 1.6f, c->centre.y, c->centre.z + 1.6f * s });
  /* facing (-z) along the velocity (-sin, 0, cos): the cape hangs behind */
  cw_node3d_set_rotation(n, (t3_vec3){ 0, cw_atan2(s, -co), 0 });
}
/* #endregion */

static cw_node_type circler_type = { .name = "Circler", .parent = "KinematicBody3D", .data_size = sizeof(circler),
                                     .doc = "Walks a circle (sample).", .ready = circler_ready,
                                     .fixed_update = circler_fixed };

/* ── app ──────────────────────────────────────────────────────────────── */

static void init(void *u) {
  (void)u;
  cw_debug_u32("flag.hash", &G.flag_hash);
  cw_debug_u32("cape.hash", &G.cape_hash);
  cw_debug_u32("rope.hash", &G.rope_hash);
  cw_debug_u32("chain.hash", &G.chain_hash);
  cw_debug_u32("phys.hash", &G.phys_hash);
  cw_debug_f32_array("crate.pos", G.crate, 3);
  cw_debug_f32_array("weight.pos", G.weight, 3);
  cw_debug_f32_array("hero.pos", G.hero, 3);
  cw_debug_f32("rope.tension", &G.tension);
  cw_debug_i32("cape.min_gap", &G.cape_gap_mm);
  cw_debug_f32("cloth.us", &G.cloth_us);
  G.cape_gap_mm = 1000000;
}

static void put3(float *o, t3_vec3 v) { o[0] = v.x, o[1] = v.y, o[2] = v.z; }

static uint32_t node_hash(cw_node n) {
  cw_value v;
  return cw_node_get_prop(n, "hash", &v) ? (uint32_t)v.i : 0;
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t);
  cw_node crate = cw_node_get(root, "Level/Crate"), weight = cw_node_get(root, "Level/Weight");
  cw_node hero = cw_node_get(root, "Level/Hero"), cape = cw_node_get(hero, "Cape");
  /* #region shove */
  if (G.tick == 90) cw_rigid_body3d_apply_impulse(crate, (t3_vec3){ 60, 0, 25 });
  if (G.tick == 150) cw_rigid_body3d_apply_impulse(weight, (t3_vec3){ -30, 0, 20 });
  /* #endregion */
  /* these are last tick's results: the tree steps after this callback */
  G.flag_hash = node_hash(cw_node_get(root, "Level/Flag"));
  G.cape_hash = node_hash(cape);
  G.rope_hash = node_hash(cw_node_get(root, "Level/Rope"));
  G.chain_hash = node_hash(cw_node_get(root, "Level/Chain"));
  cw_value v;
  if (cw_node_get_prop(cw_node_get(root, "Level/Rope"), "tension", &v)) G.tension = (float)v.f;
  put3(G.crate, cw_node3d_position(crate));
  put3(G.weight, cw_node3d_position(weight));
  t3_vec3 hp = cw_node3d_position(hero);
  put3(G.hero, hp);
  /* the cape never goes through the hero: its particles stay out of the capsule (radius 0.3) */
  int n;
  const t3_vec3 *x = cw_cloth_positions(cw_cloth3d_get(cape), &n);
  float us = 0;
  for (int i = 0; i < n; i++) {
    float dy = x[i].y - hp.y;
    if (dy < -0.6f) dy += 0.6f;
    else if (dy > 0.6f) dy -= 0.6f;
    else dy = 0;
    float dx = x[i].x - hp.x, dz = x[i].z - hp.z;
    int32_t mm = (int32_t)(sqrtf(dx * dx + dy * dy + dz * dz) * 1000);
    if (G.tick > 30 && mm < G.cape_gap_mm) G.cape_gap_mm = mm;
  }
  const char *names[] = { "Level/Flag", "Level/Hero/Cape", "Level/Rope" };
  for (int i = 0; i < 3; i++) {
    cw_cloth c = i == 0 ? cw_cloth3d_get(cw_node_get(root, names[i])) : i == 1 ? cw_cloth3d_get(cape) : cw_rope3d_get(cw_node_get(root, names[i]));
    us += cw_cloth_stats_get(c).step_us;
  }
  G.cloth_us = us;
  G.phys_hash = cw_world3d_stats(cw_physics3d_world(t)).hash;
  G.tick++;
}

/* Every node type this game defines and nothing else: the editor calls it
 * to open the game's scenes, cwhot after a reload. */
void cw_game_register_types(void);
void cw_game_register_types(void) { cw_node_type_register(&circler_type); }

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  cw_game_register_types();
  static cw_app_desc base = { .title = "cloth_demo", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
