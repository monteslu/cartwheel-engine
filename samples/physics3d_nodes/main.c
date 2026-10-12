/* physics3d_nodes: the 3D physics nodes, built ONLY from .cwscene files
 * (main, crate, walker) and run by the scene runner. The C here is what
 * scripts would hold: the Walker type (a CharacterBody3D driven by pad 0)
 * and the Oscillator type (a KinematicBody3D whose node moves; the body
 * follows and carries a crate), plus counting the goal Area3D's
 * body_entered signal.
 *
 * The scene: a static floor (BOX), a ramp whose collider is a TRIMESH made
 * from its MeshInstance3D, a block to climb onto, a stack of RigidBody3D
 * crates the walker barges through (push_mass), a rock whose CONVEX hull
 * comes from a sphere mesh, the lift with a crate riding it, and the goal
 * Area3D (body_entered / body_exited) on the block.
 *
 * Built twice: `physics3d_nodes` reads the pad (tests/physics3d-nodes-input.json),
 * `physics3d_nodes_auto` plays the same script itself (input-free hosts).
 *
 * Debug fields: walker.pos, walker.floor, walker.jumps, goal.entered,
 * goal.exited, goal.inside, crates.hash (every tick's crate + rock node
 * transforms folded), rider.pos, rock.pos, ray.hit, phys.hash. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#ifdef NODES3D_AUTOPILOT
static const struct { int frame; uint32_t buttons; float lx, ly; } SCRIPT[] = {
#include "script.inc"
};
#endif

static struct {
  float pos[3], rider[3], rock[3];
  int32_t floor, jumps, entered, exited, inside, ray;
  uint32_t crates_hash, phys_hash;
  int32_t tick;
} G;

/* ── Walker ─────────────────────────────────────────────────────────── */

typedef struct walker {
  uint32_t prev_buttons; /* the autopilot's buttons last tick: its jump edge */
  t3_vec3 run;
} walker;

static cw_action jump_action; /* pad South: just_pressed keeps a tap shorter than a tick */

/* the stick, and whether jump went down this tick */
static bool read_input(float *lx, float *ly, uint32_t *prev_buttons) {
#ifdef NODES3D_AUTOPILOT
  int frame = G.tick + 1;
  uint32_t buttons = 0;
  *lx = *ly = 0;
  for (size_t i = 0; i < CW_ARRAY_LEN(SCRIPT) && SCRIPT[i].frame <= frame; i++) {
    *lx = SCRIPT[i].lx;
    *ly = SCRIPT[i].ly;
    buttons = SCRIPT[i].buttons;
  }
  bool jump = (buttons & (1u << CW_PAD_SOUTH)) && !(*prev_buttons & (1u << CW_PAD_SOUTH));
  *prev_buttons = buttons;
  return jump;
#else
  (void)prev_buttons;
  const cw_pad_state *p = &cw_input_raw()->pads[0];
  *lx = p->axes[CW_AXIS_LX];
  *ly = p->axes[CW_AXIS_LY];
  return cw_action_just_pressed(0, jump_action);
#endif
}

/* #region walker */
static void walker_fixed(cw_node n, void *data, double dt) {
  walker *w = data;
  float lx, ly;
  bool jump = read_input(&lx, &ly, &w->prev_buttons);
  cw_value v;
  cw_node_get_prop(n, "velocity", &v);
  t3_vec3 vel = { v.v[0], v.v[1], v.v[2] };
  cw_node_get_prop(n, "on_floor", &v);
  const float speed = 4, accel = 30, gravity = 18;
  float k = accel * (float)dt;
  w->run.x += CW_CLAMP(lx * speed - w->run.x, -k, k);
  w->run.z += CW_CLAMP(ly * speed - w->run.z, -k, k);
  vel.x = w->run.x;
  vel.z = w->run.z;
  if (jump && v.b) {
    vel.y = 6.5f;
    G.jumps++;
  }
  vel.y -= gravity * (float)dt;
  cw_node_set(n, "velocity", cw_val_vec3(vel.x, vel.y, vel.z));
  cw_character3d_move_and_slide(n, (float)dt);
}
/* #endregion */
static cw_node_type walker_type = { .name = "Walker", .parent = "CharacterBody3D", .data_size = sizeof(walker),
                                    .doc = "physics3d_nodes' walker: pad 0 walks and jumps.", .fixed_update = walker_fixed };

/* ── Oscillator: a kinematic node that moves itself ────────────────── */

typedef struct osc {
  int32_t ticks;
  t3_vec3 base;
} osc;
static void osc_ready(cw_node n, void *data) { ((osc *)data)->base = cw_node3d_position(n); }
/* #region oscillator */
static void osc_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  osc *o = data;
  o->ticks++;
  float x = o->ticks < 60 ? 0 : 2.0f * cw_sin((float)(o->ticks - 60) * 0.02f);
  cw_node3d_set_position(n, (t3_vec3){ o->base.x + x, o->base.y, o->base.z });
}
/* #endregion */
static cw_node_type osc_type = { .name = "Oscillator", .parent = "KinematicBody3D", .data_size = sizeof(osc),
                                 .doc = "Slides along x (sample).", .ready = osc_ready, .fixed_update = osc_fixed };

/* ── app ────────────────────────────────────────────────────────────── */

static void goal_in(cw_node e, const cw_value *a, int c, void *u) {
  (void)e; (void)a; (void)c; (void)u;
  G.entered++;
}
static void goal_out(cw_node e, const cw_value *a, int c, void *u) {
  (void)e; (void)a; (void)c; (void)u;
  G.exited++;
}

static void init(void *u) {
  (void)u;
  jump_action = cw_action_add("jump", 0.5f);
  cw_action_bind_text(jump_action, "pad:south");
  cw_debug_f32_array("walker.pos", G.pos, 3);
  cw_debug_i32("walker.floor", &G.floor);
  cw_debug_i32("walker.jumps", &G.jumps);
  cw_debug_i32("goal.entered", &G.entered);
  cw_debug_i32("goal.exited", &G.exited);
  cw_debug_i32("goal.inside", &G.inside);
  cw_debug_u32("crates.hash", &G.crates_hash);
  cw_debug_f32_array("rider.pos", G.rider, 3);
  cw_debug_f32_array("rock.pos", G.rock, 3);
  cw_debug_i32("ray.hit", &G.ray);
  cw_debug_u32("phys.hash", &G.phys_hash);
  G.crates_hash = CW_HASH32_INIT;
  cw_node goal = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level/Goal");
  cw_connect(goal, "body_entered", goal, NULL, goal_in, NULL, 0);
  cw_connect(goal, "body_exited", goal, NULL, goal_out, NULL, 0);
}

static void put3(float *o, t3_vec3 v) {
  o[0] = v.x;
  o[1] = v.y;
  o[2] = v.z;
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  cw_tree *t = cw_scene_app_tree();
  cw_node root = cw_tree_root(t);
  cw_node p = cw_node_get(root, "Level/Player");
  put3(G.pos, cw_node3d_position(p));
  cw_value v;
  cw_node_get_prop(p, "on_floor", &v);
  G.floor = v.b;
  cw_node_get_prop(cw_node_get(p, "Down"), "colliding", &v);
  G.ray = v.b;
  cw_node_get_prop(cw_node_get(root, "Level/Goal"), "overlap_count", &v);
  G.inside = (int32_t)v.i;
  cw_node *crates;
  int n = cw_tree_group(t, "crates", &crates);
  for (int i = 0; i < n; i++) {
    t3_vec3 cp = cw_node3d_position(crates[i]);
    t3_quat cq = cw_node3d_quaternion(crates[i]);
    G.crates_hash = cw_hash32(&cp, sizeof cp, G.crates_hash);
    G.crates_hash = cw_hash32(&cq, sizeof cq, G.crates_hash);
  }
  cw_node rock = cw_node_get(root, "Level/Rock");
  put3(G.rock, cw_node3d_position(rock));
  G.crates_hash = cw_hash32(G.rock, sizeof G.rock, G.crates_hash);
  put3(G.rider, cw_node3d_position(cw_node_get(root, "Level/Rider")));
  G.phys_hash = cw_world3d_stats(cw_physics3d_world(t)).hash;
  if (getenv("N3_TRACE"))
    cw_info("t=%d pos=%.2f,%.2f,%.2f floor=%d in=%d/%d rider=%.2f,%.2f rock=%.2f,%.2f,%.2f", G.tick, G.pos[0], G.pos[1], G.pos[2],
            G.floor, G.entered, G.exited, G.rider[0], G.rider[1], G.rock[0], G.rock[1], G.rock[2]);
  G.tick++;
}

/* Every node type this game defines and nothing else (no scene, no state):
 * the editor calls it to open the game's scenes, cwhot after a reload. */
void cw_game_register_types(void);
void cw_game_register_types(void) {
  cw_node_type_register(&walker_type);
  cw_node_type_register(&osc_type);
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  cw_scene_register_builtin_types();
  cw_game_register_types();
  static cw_app_desc base = { .title = "physics3d_nodes", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
