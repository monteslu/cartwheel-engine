/* anim_ik: limbs that reach and plant. Skinned glTF robots (RobotExpressive,
 * CC0) walk back and forth over a flight of stairs and a slope; skeleton
 * modifiers (cartwheel/ik.h) adjust their walk cycle every tick:
 *
 *   Feet       FootPlacement3D: a ray per foot through Box3D, the hips
 *              lowered when a foot must reach down, two-bone legs, feet
 *              turned onto the ground normal
 *   Reach      TwoBoneIK3D: the front robot's right hand grabs a handle on
 *              a post as it passes (influence blended in and out by distance)
 *   LookNeck,  LookAtModifier3D: neck (40%) then head follow a firefly,
 *   LookHead   within angle limits
 *
 * The front robot's nodes come from main.cwscene; three more walk behind it,
 * built in code the same way (a crowd: each model's modifiers solve as one
 * job). The walk is authored on flat ground; the course height under each
 * robot comes from a ray at its centre. Pad south toggles foot placement (the
 * gate's control). Debug fields: tick, the front robot's position, per-foot
 * ground error in stance (the probe: the ankle's height above the ground
 * under it against its animated height above flat ground), the largest
 * per-tick pelvis move, the hand's distance to the handle while fully
 * reaching, look-at ticks in limit, pose hashes per 60-tick window (the front
 * robot, and every robot folded), and microseconds per character. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/ik.h"
#include "cartwheel/input.h"
#include "cartwheel/physics3d.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene_file.h"

#define ROBOTS 4
#define SPEED 1.36f      /* m/s: the walk cycle's stride at this scale */
#define X_MIN (-5.0f)
#define X_MAX 5.5f
#define HANDLE_X (-3.5f)

typedef struct robot {
  cw_node mover, model, anim, feet, reach, look_head;
  t3_gltf *gltf;
  float x, root_y, yaw, turn_t;
  int dir;
  bool turning;
} robot;

static robot R[ROBOTS];
static cw_node level, firefly, camera, grip;
static cw_action toggle_action;
static float reach_w, clock_s;

/* debug state */
static float dbg_pos[3], foot_err[2], pelvis_step, pelvis_prev, reach_err, ik_us;
static int32_t feet_on = 1, feet_ok, reach_ok, stance_ticks, reach_ticks, look_in, turns;
static uint32_t hash_win[8], crowd_win[8];
static double us_sum;
static int us_n;

static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) o->cast_shadow = true;
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}

static float ground_at(float x, float z, float from_y) {
  cw_world3d w = cw_physics3d_world(cw_scene_app_tree());
  cw_ray_hit3d hit;
  if (!cw_world3d_raycast(w, (t3_vec3){ x, from_y, z }, (t3_vec3){ 0, -4, 0 }, NULL, &hit)) return 0;
  return hit.point.y;
}

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* #region crowd */
/* the robots behind: the front robot's modifiers, made in code */
static void make_robot(int i) {
  char name[32];
  snprintf(name, sizeof name, "Mover%d", i);
  robot *r = &R[i];
  r->mover = add(level, "Node3D", name);
  r->model = add(r->mover, "Node3D", "Model");
  cw_node3d_set_scale(r->model, (t3_vec3){ 0.38f, 0.38f, 0.38f });
  r->feet = add(r->model, "FootPlacement3D", "Feet");
  cw_node_set(r->feet, "legs", cw_val_string("UpperLegL,LowerLegL,LowerLegL_end,FootL;UpperLegR,LowerLegR,LowerLegR_end,FootR"));
  cw_node_set(r->feet, "pelvis", cw_val_string("Body"));
  cw_node_set(r->feet, "foot_height", cw_val_float(0.025));
  cw_node_set(r->feet, "max_slope", cw_val_float(0.5));
  cw_node_set(r->feet, "blend_speed", cw_val_float(14));
  r->look_head = add(r->model, "LookAtModifier3D", "LookHead");
  cw_node_set(r->look_head, "bone", cw_val_string("Head"));
  cw_node_set(r->look_head, "target", cw_val_string("../../../Firefly"));
  cw_node_set(r->look_head, "limit_angle", cw_val_float(1.1));
  snprintf(name, sizeof name, "Anim%d", i);
  r->anim = add(level, "AnimationPlayer", name);
}
/* #endregion */

static void load_robot(int i, const void *glb, size_t len) {
  robot *r = &R[i];
  char path[32];
  snprintf(path, sizeof path, "%s/Model", cw_node_name(r->mover));
  r->gltf = t3_gltf_parse(glb, len, NULL, NULL);
  cw_node3d_set_object(r->model, r->gltf->scene);
  set_shadows(r->gltf->scene);
  for (int k = 0; k < r->gltf->animation_count; k++) {
    const char *name = r->gltf->animations[k]->name;
    if (strcmp(name, "Walking") && strcmp(name, "Idle")) continue;
    cw_anim a = cw_anim_from_t3_clip(r->gltf->animations[k], path);
    cw_anim_set_loop(a, CW_LOOP_LINEAR);
    cw_anim_player_add(r->anim, !strcmp(name, "Walking") ? "walk" : "idle", a);
    cw_anim_release(a);
  }
  /* the robots behind start further along the course, out of step */
  r->x = X_MIN + 2.3f * (float)i;
  r->dir = 1;
  r->yaw = 1.5707963f;
  cw_node3d_set_position(r->mover, (t3_vec3){ r->x, 0, -1.6f * (float)i });
  cw_node3d_set_rotation(r->mover, (t3_vec3){ 0, r->yaw, 0 });
  cw_anim_player_play(r->anim, "walk", 0);
  cw_anim_player_seek(r->anim, 0.23f * (float)i, false);
}

static void init(void *u) {
  (void)u;
  toggle_action = cw_action_add("toggle_feet", 0.5f);
  cw_action_bind_text(toggle_action, "pad:south");
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  firefly = cw_node_get(level, "Firefly");
  camera = cw_node_get(level, "Camera");
  grip = cw_node_get(level, "Handle/Grip");
  R[0].mover = cw_node_get(level, "Mover");
  R[0].model = cw_node_get(level, "Mover/Model");
  R[0].anim = cw_node_get(level, "Anim");
  R[0].feet = cw_node_get(R[0].model, "Feet");
  R[0].reach = cw_node_get(R[0].model, "Reach");
  R[0].look_head = cw_node_get(R[0].model, "LookHead");
  for (int i = 1; i < ROBOTS; i++) make_robot(i);
  size_t len;
  void *glb = cw_asset_load("RobotExpressive.glb", &len);
  if (!glb) { cw_error("anim_ik: RobotExpressive.glb missing from the assets"); return; }
  for (int i = 0; i < ROBOTS; i++) load_robot(i, glb, len);
  free(glb);

  cw_debug_f32_array("pos", dbg_pos, 3);
  cw_debug_i32("turns", &turns);
  cw_debug_i32("feet_on", &feet_on);
  cw_debug_i32("feet_ok", &feet_ok);
  cw_debug_i32("stance_ticks", &stance_ticks);
  cw_debug_f32_array("foot_err", foot_err, 2);
  cw_debug_f32("pelvis_step", &pelvis_step);
  cw_debug_i32("reach_ticks", &reach_ticks);
  cw_debug_f32("reach_err", &reach_err);
  cw_debug_i32("reach_ok", &reach_ok);
  cw_debug_i32("look_in", &look_in);
  cw_debug_field("hash_win", hash_win, CW_DBG_U32, 8);
  cw_debug_field("crowd_win", crowd_win, CW_DBG_U32, 8);
  cw_debug_f32("ik_us", &ik_us); /* host clock: never compared */
}

/* #region probe */
/* in stance, the ankle sits above the ground under it by its animated lift */
static void probe_feet(uint64_t tick) {
  static const char *ankles[2] = { "LowerLegL_end", "LowerLegR_end" };
  for (int i = 0; i < 2; i++) {
    cw_foot_state st;
    if (!cw_foot_placement_leg(R[0].feet, i, &st) || !st.hit) continue;
    if (st.lift > 0.03f || tick < 30 || R[0].turning) continue; /* swing, warm-up, turning on the spot */
    t3_vec3 a = cw_skeleton_bone_world(R[0].model, ankles[i]);
    float g = ground_at(a.x, a.z, a.y + 0.5f);
    float err = fabsf((a.y - g) - st.lift);
    if (err > foot_err[i]) foot_err[i] = err;
    stance_ticks++;
  }
  feet_ok = stance_ticks > 0 && foot_err[0] < 0.01f && foot_err[1] < 0.01f;
}
/* #endregion */

static void walk(robot *r, float dt) {
  if (r->turning) {
    r->turn_t += dt;
    float k = cw_smoothstep(0, 1.2f, r->turn_t);
    r->yaw = r->dir > 0 ? -1.5707963f + 3.1415927f * k : 1.5707963f + 3.1415927f * k;
    if (r->turn_t >= 1.2f) {
      r->turning = false;
      r->yaw = r->dir > 0 ? 1.5707963f : -1.5707963f;
      cw_anim_player_play(r->anim, "walk", 0.25f);
    }
  } else {
    r->x += (float)r->dir * SPEED * dt;
    if ((r->dir > 0 && r->x >= X_MAX) || (r->dir < 0 && r->x <= X_MIN)) {
      r->dir = -r->dir;
      r->turning = true;
      r->turn_t = 0;
      if (r == &R[0]) turns++;
      cw_anim_player_play(r->anim, "idle", 0.25f);
    }
  }
  float z = cw_node3d_position(r->mover).z;
  float g = ground_at(r->x, z, 2);
  r->root_y += (g - r->root_y) * (1 - cw_exp(-10 * dt));
  cw_node3d_set_position(r->mover, (t3_vec3){ r->x, r->root_y, z });
  cw_node3d_set_rotation(r->mover, (t3_vec3){ 0, r->yaw, 0 });
}

static void fixed(void *u, double dt) {
  (void)u;
  if (!R[0].gltf) return;
  float fdt = (float)dt;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  /* this tick's modifiers already ran (after the nodes, before this
   * callback): probe them, then move everything for the next tick */
  probe_feet(tick);
  float pel = cw_foot_placement_pelvis_offset(R[0].feet);
  if (tick > 1 && fabsf(pel - pelvis_prev) > pelvis_step) pelvis_step = fabsf(pel - pelvis_prev);
  pelvis_prev = pel;
  if (reach_w >= 0.999f) {
    t3_vec3 h = cw_skeleton_bone_world(R[0].model, "Palm2R"), g = cw_node3d_global_position(grip);
    float d = sqrtf((h.x - g.x) * (h.x - g.x) + (h.y - g.y) * (h.y - g.y) + (h.z - g.z) * (h.z - g.z));
    if (d > reach_err) reach_err = d;
    reach_ticks++;
  }
  reach_ok = reach_ticks > 0 && reach_err < 0.001f;
  cw_value il;
  if (cw_node_get_prop(R[0].look_head, "in_limit", &il) && il.b) look_in++;
  uint32_t crowd = CW_HASH32_INIT;
  for (int i = 0; i < ROBOTS; i++) {
    uint32_t h = cw_skeleton_hash(R[i].model);
    crowd = cw_hash32(&h, sizeof h, crowd);
  }
  if (tick / 60 < 8) {
    hash_win[tick / 60] = cw_skeleton_hash(R[0].model);
    crowd_win[tick / 60] = crowd;
  }
  if (tick > 1) {
    us_sum += cw_skeleton_last_us(R[0].model);
    us_n++;
    ik_us = (float)(us_sum / us_n);
  }

  if (cw_action_just_pressed(0, toggle_action)) {
    feet_on = !feet_on;
    for (int i = 0; i < ROBOTS; i++) cw_node_set(R[i].feet, "active", cw_val_bool(feet_on));
  }
  if (tick <= 1)
    for (int i = 0; i < ROBOTS; i++) R[i].root_y = ground_at(R[i].x, cw_node3d_position(R[i].mover).z, 2);
  for (int i = 0; i < ROBOTS; i++) walk(&R[i], fdt);

  /* #region reach */
  /* the right hand reaches for the handle while walking past it (+x only) */
  float dx = fabsf(R[0].x - HANDLE_X);
  float want = R[0].dir > 0 && !R[0].turning ? 1 - cw_smoothstep(0.25f, 0.75f, dx) : 0;
  reach_w = cw_move_toward(reach_w, want, 4 * fdt);
  cw_node_set(R[0].reach, "influence", cw_val_float(reach_w));
  /* #endregion */

  /* the firefly circles the front robot, sometimes behind it (the look-at limits hold) */
  clock_s += fdt;
  float s1, c1, s2, c2;
  cw_sincos(clock_s * 0.8f, &s1, &c1);
  cw_sincos(clock_s * 1.7f, &s2, &c2);
  cw_node3d_set_position(firefly, (t3_vec3){ R[0].x + 1.6f * s1, R[0].root_y + 1.25f + 0.35f * s2, 1.6f * c1 });
  /* the camera follows along the course */
  t3_vec3 cp = cw_node3d_position(camera);
  cw_node3d_set_position(camera, (t3_vec3){ cp.x + (R[0].x - cp.x) * (1 - cw_exp(-3 * fdt)), cp.y, cp.z });
  dbg_pos[0] = R[0].x;
  dbg_pos[1] = R[0].root_y;
  dbg_pos[2] = R[0].yaw;
}

/* the Model nodes hold the glTF scenes (retained); drop the app's references */
static void shutdown(void *u) {
  (void)u;
  for (int i = 0; i < ROBOTS; i++)
    if (R[i].gltf) {
      t3_release(R[i].gltf);
      R[i].gltf = NULL;
    }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_ik", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
