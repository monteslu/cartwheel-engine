/* ragdoll: knocked down, falls, gets up (cartwheel/ragdoll.h). Four skinned
 * robots (RobotExpressive, CC0) stand in a row. One after another each is
 * knocked over: its skeleton becomes a ragdoll (a capsule per bone, cone and
 * twist limits at the shoulders, hips and spine, hinges at the knees and
 * elbows) seeded from the pose it had and pushed by a blow. It falls onto the
 * floor, a kerb or a crate, comes to rest, then the pose blends from the
 * ragdoll back into the idle animation where the body lies.
 *
 * Pad south stops writing the ragdoll onto the skeleton (the gate's control:
 * the mesh then ignores the bodies and the follow probe fails).
 * Debug fields: made, rested, got_up, follow_err (the largest distance
 * between a part's body and its bone in the written pose), follow_ok,
 * hinge_max (largest hinge angle beyond its limit, radians), limits_ok,
 * on_crate (a robot came to rest on the crate), hash_win (every ragdoll's
 * transforms per 60-tick window), rag_us (host clock, never compared). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/ik.h"
#include "cartwheel/input.h"
#include "cartwheel/math.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/profile.h"
#include "cartwheel/ragdoll.h"
#include "cartwheel/scene_file.h"

#define ROBOTS 4
#define SCALE 0.38f

/* #region parts */
/* RobotExpressive's skeleton as a ragdoll: the body, torso and head, two
 * arms, two legs. Hinge axes are model-space at rest (the model faces +z). */
static const cw_ragdoll_part PARTS[] = {
  { .bone = "Body", .tip = "Torso", .radius = 0.13f },
  { .bone = "Torso", .tip = "Neck", .radius = 0.15f, .joint = CW_RAGDOLL_BALL, .swing = 0.4f, .lower = -0.3f, .upper = 0.3f },
  { .bone = "Head", .tip_offset = { 0, 1.0f, 0 }, .radius = 0.2f, .joint = CW_RAGDOLL_BALL, .swing = 0.6f, .lower = -0.6f, .upper = 0.6f },
  { .bone = "UpperArmL", .tip = "LowerArmL", .radius = 0.06f, .joint = CW_RAGDOLL_BALL, .swing = 1.6f, .lower = -0.6f, .upper = 0.6f },
  { .bone = "LowerArmL", .tip = "Palm2L", .radius = 0.06f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -2.2f, .upper = 0.1f },
  { .bone = "UpperArmR", .tip = "LowerArmR", .radius = 0.06f, .joint = CW_RAGDOLL_BALL, .swing = 1.6f, .lower = -0.6f, .upper = 0.6f },
  { .bone = "LowerArmR", .tip = "Palm2R", .radius = 0.06f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -2.2f, .upper = 0.1f },
  { .bone = "UpperLegL", .tip = "LowerLegL", .radius = 0.07f, .joint = CW_RAGDOLL_BALL, .swing = 1.2f, .lower = -0.4f, .upper = 0.4f },
  { .bone = "LowerLegL", .tip = "LowerLegL_end", .radius = 0.06f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -0.1f, .upper = 2.2f },
  { .bone = "UpperLegR", .tip = "LowerLegR", .radius = 0.07f, .joint = CW_RAGDOLL_BALL, .swing = 1.2f, .lower = -0.4f, .upper = 0.4f },
  { .bone = "LowerLegR", .tip = "LowerLegR_end", .radius = 0.06f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -0.1f, .upper = 2.2f },
  /* the robot's feet hang under its root bone (they are IK-driven), not under the shins: attach them to the shins */
  { .bone = "FootL", .attach = "LowerLegL", .tip_offset = { 0, 0, 0.6f }, .radius = 0.06f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -0.5f, .upper = 0.5f },
  { .bone = "FootR", .attach = "LowerLegR", .tip_offset = { 0, 0, 0.6f }, .radius = 0.06f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -0.5f, .upper = 0.5f },
};
#define NPARTS ((int)(sizeof PARTS / sizeof *PARTS))
/* #endregion */

enum { STAND, LIMP, BLEND };
typedef struct robot {
  cw_node mover, model, anim;
  t3_gltf *gltf;
  cw_pose pose;
  cw_ragdoll *rd;
  int mode, t, knock_at;
  float x, yaw;
} robot;

static robot R[ROBOTS];
static cw_node level;
static cw_action control_action;
static bool write_on = true;
static int32_t made, rested, got_up, follow_ok, limits_ok = 1, on_crate;
static float follow_err, hinge_max, rag_us, rest_at[ROBOTS][3];
static uint32_t hash_win[8];

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}
static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) o->cast_shadow = true;
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}
/* the model node's space in the world: the mover's place and yaw, the model node's scale */
static t3_mat4 model_to_world(const robot *r) {
  t3_mat4 m;
  float s, c;
  cw_sincos(r->yaw * 0.5f, &s, &c);
  t3_mat4_compose(&m, (t3_vec3){ r->x, 0, 0 }, (t3_quat){ 0, s, 0, c }, (t3_vec3){ SCALE, SCALE, SCALE });
  return m;
}

static void init(void *u) {
  (void)u;
  control_action = cw_action_add("toggle_write", 0.5f);
  cw_action_bind_text(control_action, "pad:south");
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  size_t len;
  void *glb = cw_asset_load("RobotExpressive.glb", &len);
  if (!glb) { cw_error("ragdoll: RobotExpressive.glb missing from the assets"); return; }
  for (int i = 0; i < ROBOTS; i++) {
    robot *r = &R[i];
    char name[32], path[48];
    snprintf(name, sizeof name, "Mover%d", i);
    r->mover = add(level, "Node3D", name);
    r->model = add(r->mover, "Node3D", "Model");
    cw_node3d_set_scale(r->model, (t3_vec3){ SCALE, SCALE, SCALE });
    snprintf(name, sizeof name, "Anim%d", i);
    r->anim = add(level, "AnimationPlayer", name);
    r->gltf = t3_gltf_parse(glb, len, NULL, NULL);
    cw_node3d_set_object(r->model, r->gltf->scene);
    set_shadows(r->gltf->scene);
    snprintf(path, sizeof path, "Mover%d/Model", i);
    for (int k = 0; k < r->gltf->animation_count; k++) {
      if (strcmp(r->gltf->animations[k]->name, "Idle")) continue;
      cw_anim a = cw_anim_from_t3_clip(r->gltf->animations[k], path);
      cw_anim_set_loop(a, CW_LOOP_LINEAR);
      cw_anim_player_add(r->anim, "idle", a);
      cw_anim_release(a);
    }
    cw_anim_player_play(r->anim, "idle", 0);
    cw_anim_player_seek(r->anim, 0.4f * (float)i, false);
    r->x = -2.4f + 1.6f * (float)i;
    r->yaw = 0.3f * (float)(i - 1);
    r->knock_at = 30 + 45 * i;
    cw_node3d_set_position(r->mover, (t3_vec3){ r->x, 0, 0 });
    cw_node3d_set_rotation(r->mover, (t3_vec3){ 0, r->yaw, 0 });
    cw_pose_bind(&r->pose, cw_node3d_object(r->model));
  }
  free(glb);
  cw_debug_i32("made", &made);
  cw_debug_i32("rested", &rested);
  cw_debug_i32("got_up", &got_up);
  cw_debug_f32("follow_err", &follow_err);
  cw_debug_i32("follow_ok", &follow_ok);
  cw_debug_f32("hinge_max", &hinge_max);
  cw_debug_i32("limits_ok", &limits_ok);
  cw_debug_i32("on_crate", &on_crate);
  cw_debug_f32_array("rest_at", &rest_at[0][0], ROBOTS * 3); /* where each body came to rest (hips) */
  cw_debug_field("hash_win", hash_win, CW_DBG_U32, 8);
  cw_debug_f32("rag_us", &rag_us); /* host clock: never compared */
}

/* #region knock */
/* the blow: the whole body carried back and to the side, the chest pushed harder; robot 3 falls back onto the crate */
static void knock(robot *r, int i) {
  cw_pose_capture(&r->pose);
  t3_mat4 m = model_to_world(r);
  cw_ragdoll_def def = cw_ragdoll_default_def(PARTS, NPARTS);
  def.joint_friction = 1.5f;
  r->rd = cw_ragdoll_create(cw_physics3d_world(cw_scene_app_tree()), &def, &r->pose, &m);
  if (!r->rd) return;
  made++;
  float side = i == 2 ? 0.15f : (i & 1) ? -1.2f : 1.0f;
  cw_ragdoll_set_velocity(r->rd, (t3_vec3){ side, 1.5f, -2.2f }, (t3_vec3){ -3.0f, 0, -side });
  cw_xform3 chest = cw_ragdoll_part_transform(r->rd, 1);
  cw_ragdoll_apply_impulse(r->rd, 1, (t3_vec3){ side * 4, 2, -8 }, chest.p);
  r->mode = LIMP, r->t = 0;
}
/* #endregion */

/* the probe: in the written pose every part bone sits where its body is */
static void probe(robot *r) {
  t3_mat4 m = model_to_world(r);
  for (int p = 0; p < NPARTS; p++) {
    int b = cw_pose_find(&r->pose, PARTS[p].bone);
    t3_vec3 bw = t3_vec3_apply_mat4(cw_pose_model_position(&r->pose, b), &m);
    t3_vec3 rw = cw_ragdoll_part_transform(r->rd, p).p;
    float d = t3_vec3_distance(bw, rw);
    if (d > follow_err) follow_err = d;
    if (PARTS[p].joint == CW_RAGDOLL_HINGE) {
      float a = cw_ragdoll_joint_angle(r->rd, p);
      float over = a > PARTS[p].upper ? a - PARTS[p].upper : a < PARTS[p].lower ? PARTS[p].lower - a : 0;
      if (over > hinge_max) hinge_max = over;
    }
  }
}

static void fixed(void *u, double dt) {
  (void)u, (void)dt;
  if (!R[0].gltf) return;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  if (cw_action_just_pressed(0, control_action)) write_on = !write_on;
  uint64_t t0 = cw_profile_now_ns();
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < ROBOTS; i++) {
    robot *r = &R[i];
    if (r->mode == STAND && (int)tick == r->knock_at) knock(r, i);
    if (!r->rd) continue;
    r->t++;
    if (r->mode == LIMP && (cw_ragdoll_resting(r->rd, 0.08f) || r->t > 200)) {
      if (cw_ragdoll_resting(r->rd, 0.08f)) rested++;
      { t3_vec3 hp0 = cw_ragdoll_part_transform(r->rd, 0).p; rest_at[i][0] = hp0.x, rest_at[i][1] = hp0.y, rest_at[i][2] = hp0.z; }
      if (i == 2 && cw_ragdoll_part_transform(r->rd, 0).p.y > 0.42f) on_crate = 1;
      /* get up where the body lies: the mover goes under the ragdoll's hips (the pose still shows the ragdoll) */
      t3_vec3 hp = cw_ragdoll_part_transform(r->rd, 0).p;
      r->x = hp.x;
      cw_node3d_set_position(r->mover, (t3_vec3){ r->x, 0, 0 });
      r->mode = BLEND, r->t = 0;
    }
    float w = r->mode == LIMP ? 1 : 1 - cw_smoothstep(0, 30, (float)r->t);
    cw_pose_capture(&r->pose);
    t3_mat4 m = model_to_world(r);
    if (write_on) cw_ragdoll_blend_pose(r->rd, &r->pose, &m, w);
    if (r->mode == LIMP) probe(r);
    cw_pose_flush(&r->pose);
    h = cw_ragdoll_hash(r->rd, h);
    if (r->mode == BLEND && w <= 0) {
      cw_ragdoll_destroy(r->rd);
      r->rd = NULL;
      r->mode = STAND;
      got_up++;
    }
  }
  rag_us = (float)(cw_profile_now_ns() - t0) * 1e-3f;
  if (tick / 60 < 8) hash_win[tick / 60] = h;
  follow_ok = made == ROBOTS && follow_err < 0.08f;
  limits_ok = hinge_max < 0.2f;
}

static void shutdown(void *u) {
  (void)u;
  for (int i = 0; i < ROBOTS; i++) {
    cw_ragdoll_destroy(R[i].rd);
    R[i].rd = NULL;
    cw_pose_free(&R[i].pose);
    if (R[i].gltf) { t3_release(R[i].gltf); R[i].gltf = NULL; }
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "ragdoll", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
