/* anim_skinned: a skinned glTF character (Khronos CesiumMan, CC-BY-4.0, from
 * deps/three.c's test assets) driven by an AnimationTree from pad input.
 *
 *   Tree root   add(state machine, look 2D blend space * look_amount)
 *   machine     "move": 1D blend space on `speed` (idle 0, walk 1, run 2 =
 *               the walk at 1.7x); "wave": a one-shot reached by travel
 *               (south button), back to "move" at its end
 *   look        additive 2D blend space on (look_x, look_y) over five neck
 *               poses, bone-masked to the neck joints (right stick)
 *   root motion the walk's forward torso motion moves Mover (turned by the
 *               left stick's x); the torso stays over the feet
 *
 * The glTF's single walk clip is sampled by cartwheel (deterministic) into
 * the other clips: idle and wave are walk poses with procedural bone
 * rotations, look poses are rest-relative neck turns. Debug fields: the
 * machine's state, parameters, Mover's transform, clip weights and the
 * tree's pose hash per 60-tick window. */
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/scene_file.h"

#define MODEL "Mover/Model"
static cw_node level, mover, tree;
static cw_blend_id sm, clips[8];
static t3_gltf *gltf;
static int32_t state, waves, entered, footsteps;
static float params[5], mover_pos[3], mover_yaw, weights[3], rm_total;
static uint32_t hash_win[8];

/* ── deterministic quaternion helpers (cw_sincos, not libm) ───────── */

static t3_quat qmul(t3_quat a, t3_quat b) {
  return (t3_quat){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static t3_quat qconj(t3_quat q) { return (t3_quat){ -q.x, -q.y, -q.z, q.w }; }
static t3_quat qaxis(t3_vec3 a, float angle) {
  float s, c;
  cw_sincos(angle * 0.5f, &s, &c);
  return (t3_quat){ a.x * s, a.y * s, a.z * s, c };
}
static t3_vec3 qrot(t3_quat q, t3_vec3 v) {
  t3_quat p = qmul(qmul(q, (t3_quat){ v.x, v.y, v.z, 0 }), qconj(q));
  return (t3_vec3){ p.x, p.y, p.z };
}
/* rest world rotation of a glTF object (its chain up to the glTF root) */
static t3_quat world_q(t3_object *o) {
  t3_quat q = { 0, 0, 0, 1 };
  for (; o && o != gltf->scene; o = o->parent) q = qmul(o->quaternion, q);
  return q;
}
/* a world-space axis in the bone's own frame: rotating its local rotation
 * about it turns the bone about that world axis */
static t3_vec3 local_axis(const char *bone, t3_vec3 world) {
  return qrot(qconj(world_q(t3_object_get_by_name(gltf->scene, bone))), world);
}

static cw_value vquat(t3_quat q) {
  cw_value v = { CW_T_QUAT };
  memcpy(v.v, &q, 16);
  return v;
}
static t3_quat asquat(const cw_value *v) { return (t3_quat){ v->v[0], v->v[1], v->v[2], v->v[3] }; }

static char path_buf[256];
static const char *bone_path(const char *bone, const char *prop) {
  snprintf(path_buf, sizeof path_buf, MODEL "@%s:%s", bone, prop);
  return path_buf;
}

/* a pose clip: every track of `src` held at time t */
static cw_anim pose_of(cw_anim src, float t, const char *name, float len, cw_loop_mode loop) {
  cw_anim a = cw_anim_new(name, len, loop);
  cw_anim_set_stored(a, false);
  for (int i = 0; i < cw_anim_track_count(src); i++) {
    cw_value v;
    cw_anim_sample(src, i, t, &v);
    int k = cw_anim_add_value_track(a, cw_anim_track_path(src, i), CW_INTERP_LINEAR);
    cw_anim_add_key(a, k, 0, v);
  }
  return a;
}

/* rotate a bone track of `a` about a world axis by angle(t) at n keys */
static void swing(cw_anim a, const char *bone, t3_vec3 world_axis, int n, float (*angle)(float t)) {
  int tr = cw_anim_track_find(a, bone_path(bone, "quaternion"));
  if (tr < 0) { cw_error("anim_skinned: no track for bone %s", bone); return; }
  cw_value base;
  cw_anim_sample(a, tr, 0, &base);
  t3_vec3 ax = local_axis(bone, world_axis);
  float len = cw_anim_length(a);
  for (int i = 0; i <= n; i++) {
    float t = len * (float)i / (float)n;
    cw_anim_add_key(a, tr, t, vquat(qmul(asquat(&base), qaxis(ax, angle(t)))));
  }
}

static float breathe(float t) { return 0.04f * cw_sin(t * CW_TAU / 3); }
static float wave_shoulder(float t) { float r = cw_smoothstep(0, 0.4f, t) * (1 - cw_smoothstep(1.8f, 2.2f, t)); return -2.2f * r; }
static float wave_elbow(float t) { float r = cw_smoothstep(0.2f, 0.6f, t) * (1 - cw_smoothstep(1.6f, 2.2f, t)); return r * (-0.9f + 0.5f * cw_sin(t * CW_TAU * 1.5f)); }

/* ── setup ──────────────────────────────────────────────────────────── */

static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) o->cast_shadow = true;
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}

static void on_state(cw_node e, const cw_value *a, int n, void *u) {
  (void)e; (void)u;
  entered++;
  if (n && !strcmp(a[0].s, "wave")) waves++;
}

static void on_event(cw_node t, const char *event, const char *animation, float key_time, void *u) {
  (void)t; (void)animation; (void)key_time; (void)u;
  if (!strcmp(event, "footstep")) footsteps++;
}

static cw_action wave_action; /* pad South: just_pressed keeps a tap shorter than a tick */

static void init(void *u) {
  (void)u;
  wave_action = cw_action_add("wave", 0.5f);
  cw_action_bind_text(wave_action, "pad:south");
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  mover = cw_node_get(level, "Mover");
  tree = cw_node_get(level, "Tree");
  cw_node anim = cw_node_get(level, "Anim");
  size_t len;
  void *glb = cw_asset_load("CesiumMan.glb", &len);
  if (!glb) { cw_error("anim_skinned: CesiumMan.glb missing from the assets"); return; }
  /* kept until shutdown: its objects use names the t3_gltf owns */
  gltf = t3_gltf_parse(glb, len, NULL, NULL);
  free(glb);
  cw_node3d_set_object(cw_node_get(mover, "Model"), gltf->scene);
  set_shadows(gltf->scene);

  /* clips */
  cw_anim walk = cw_anim_from_t3_clip(gltf->animations[0], MODEL);
  /* the clip walks in place: give the torso a forward stride (armature x is
   * the character's forward; 1.4 m per 2 s cycle) for root motion */
  int torso = cw_anim_track_find(walk, bone_path("Skeleton_torso_joint_1", "position"));
  float wl = cw_anim_length(walk);
  cw_value first;
  cw_anim_sample(walk, torso, 0, &first);
  cw_anim_add_key(walk, torso, 0, first);
  for (int k = 0; k < cw_anim_key_count(walk, torso); k++) {
    float t = cw_anim_key_time(walk, torso, k);
    cw_value v;
    cw_anim_sample(walk, torso, t, &v);
    v.v[0] += 1.4f * t / wl;
    cw_anim_add_key(walk, torso, t, v);
  }
  cw_anim idle = pose_of(walk, 0.55f, "idle", 3, CW_LOOP_LINEAR);
  /* the idle pose stands on the spot: no stride */
  cw_value still;
  cw_anim_sample(idle, torso, 0, &still);
  still.v[0] = first.v[0];
  cw_anim_add_key(idle, torso, 0, still);
  swing(idle, "Skeleton_torso_joint_2", (t3_vec3){ 1, 0, 0 }, 12, breathe);
  cw_anim wave = pose_of(idle, 0, "wave", 2.2f, CW_LOOP_NONE);
  swing(wave, "Skeleton_arm_joint_R", (t3_vec3){ 0, 0, 1 }, 22, wave_shoulder);
  swing(wave, "Skeleton_arm_joint_R__2_", (t3_vec3){ 0, 0, 1 }, 22, wave_elbow);
  const char *look_names[5] = { "look_c", "look_l", "look_r", "look_u", "look_d" };
  t3_vec3 look_axis[5] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 0, 0 } };
  float look_angle[5] = { 0, 0.7f, -0.7f, -0.45f, 0.45f };
  const char *neck[2] = { "Skeleton_neck_joint_1", "Skeleton_neck_joint_2" };
  cw_anim look[5];
  for (int i = 0; i < 5; i++) {
    look[i] = cw_anim_new(look_names[i], 1, CW_LOOP_LINEAR);
    cw_anim_set_stored(look[i], false);
    for (int b = 0; b < 2; b++) {
      t3_object *o = t3_object_get_by_name(gltf->scene, neck[b]);
      int k = cw_anim_add_value_track(look[i], bone_path(neck[b], "quaternion"), CW_INTERP_LINEAR);
      cw_anim_add_key(look[i], k, 0, vquat(qmul(o->quaternion, qaxis(local_axis(neck[b], look_axis[i]), look_angle[i] * 0.5f))));
    }
  }
  /* gameplay events on a glTF clip: a footstep at each foot's contact (a sound, a dust puff, a game's own
   * hit window), delivered to a C callback below */
  cw_anim_add_event(walk, 0.25f * wl, "footstep");
  cw_anim_add_event(walk, 0.75f * wl, "footstep");
  cw_anim_player_add(anim, "walk", walk);
  cw_anim_player_add(anim, "idle", idle);
  cw_anim_player_add(anim, "wave", wave);
  for (int i = 0; i < 5; i++) { cw_anim_player_add(anim, look_names[i], look[i]); cw_anim_release(look[i]); }
  cw_anim_release(walk);
  cw_anim_release(idle);
  cw_anim_release(wave);

  /* #region graph */
  /* the graph */
  cw_blend_id move = cw_animtree_blend1d(tree, "speed");
  clips[0] = cw_animtree_clip(tree, "idle");
  clips[1] = cw_animtree_clip(tree, "walk");
  clips[2] = cw_animtree_clip(tree, "walk");
  cw_animtree_clip_set_speed(tree, clips[2], 1.7f);
  cw_animtree_blend1d_add(tree, move, clips[0], 0);
  cw_animtree_blend1d_add(tree, move, clips[1], 1);
  cw_animtree_blend1d_add(tree, move, clips[2], 2);
  sm = cw_animtree_state_machine(tree);
  cw_animtree_sm_add_state(tree, sm, "move", move);
  cw_animtree_sm_add_state(tree, sm, "wave", clips[3] = cw_animtree_clip(tree, "wave"));
  cw_animtree_sm_add_transition(tree, sm, "move", "wave", &(cw_transition){ .xfade = 0.25f, .advance = CW_ADVANCE_ENABLED });
  cw_animtree_sm_add_transition(tree, sm, "wave", "move", &(cw_transition){ .xfade = 0.3f, .switch_mode = CW_SWITCH_AT_END });
  cw_blend_id lookbs = cw_animtree_blend2d(tree, "look_x", "look_y");
  float lp[5][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, 1 }, { 0, -1 } };
  for (int i = 0; i < 5; i++) cw_animtree_blend2d_add(tree, lookbs, cw_animtree_clip(tree, look_names[i]), lp[i][0], lp[i][1]);
  cw_animtree_set_filter(tree, lookbs, neck, 2);
  cw_animtree_set_root(tree, cw_animtree_add(tree, sm, lookbs, "look_amount"));
  cw_animtree_set_param(tree, "look_amount", 1);
  /* #endregion */
  cw_animtree_on_event(tree, on_event, NULL);
  cw_connect(tree, "state_changed", (cw_node){ 0 }, NULL, on_state, NULL, 0);

  cw_debug_i32("state", &state);
  cw_debug_i32("waves", &waves);
  cw_debug_i32("footsteps", &footsteps);
  cw_debug_i32("entered", &entered);
  cw_debug_f32_array("params", params, 5);
  cw_debug_f32_array("mover", mover_pos, 3);
  cw_debug_f32("yaw", &mover_yaw);
  cw_debug_f32_array("weights", weights, 3);
  cw_debug_f32("root_motion", &rm_total);
  cw_debug_field("hash_win", hash_win, CW_DBG_U32, 8);
}

/* input drives the tree's parameters before the tree runs (the app's fixed
 * callback runs after the tree: this tick's input applies next tick) */
/* hosts quantize sticks differently (0.50002 vs 0.49998): snap to 1/64 */
static float axis(const cw_pad_state *pad, int a) { return floorf(pad->axes[a] * 64 + 0.5f) / 64; }

static void fixed(void *u, double dt) {
  (void)u;
  if (!gltf) return;
  /* #region params */
  const cw_pad_state *pad = &cw_input_raw()->pads[0];
  float ly = axis(pad, CW_AXIS_LY), lx = axis(pad, CW_AXIS_LX);
  float speed = ly < 0 ? -ly * 2 : 0;
  cw_animtree_set_param(tree, "speed", speed);
  cw_animtree_set_param(tree, "look_x", axis(pad, CW_AXIS_RX));
  cw_animtree_set_param(tree, "look_y", -axis(pad, CW_AXIS_RY));
  if (cw_action_just_pressed(0, wave_action)) cw_animtree_sm_travel(tree, sm, "wave");
  /* #endregion */
  mover_yaw -= lx * 1.5f * (float)dt;
  cw_node3d_set_rotation(mover, (t3_vec3){ 0, mover_yaw, 0 });

  const char *cur = cw_animtree_sm_current(tree, sm);
  state = cur && !strcmp(cur, "wave") ? 1 : 0;
  if (cw_animtree_sm_next(tree, sm)) state |= 2;
  params[0] = speed;
  params[1] = cw_animtree_get_param(tree, "look_x");
  params[2] = cw_animtree_get_param(tree, "look_y");
  t3_vec3 p = cw_node3d_position(mover);
  mover_pos[0] = p.x; mover_pos[1] = p.y; mover_pos[2] = p.z;
  for (int i = 0; i < 3; i++) weights[i] = cw_animtree_clip_weight(tree, clips[i]);
  t3_vec3 rm = cw_animtree_root_motion(tree);
  rm_total += sqrtf(rm.x * rm.x + rm.z * rm.z);
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  if (tick / 60 < 8) hash_win[tick / 60] = cw_animtree_hash(tree);
}

/* drop the app's reference: the Model node still holds the glTF scene
 * (retained), and the runner frees the tree right after this, releasing
 * the rest (scene, skinned mesh, skeleton) */
static void shutdown(void *u) {
  (void)u;
  if (!gltf) return;
  t3_release(gltf);
  gltf = NULL;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "anim_skinned", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
