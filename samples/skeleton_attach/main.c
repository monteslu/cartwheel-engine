/* skeleton_attach: a skinned model's bones as a node, and things that ride
 * them (cartwheel/skeleton.h). Three robots (RobotExpressive, CC0), each a
 * different use of Skeleton3D:
 *
 *   Mover    walks back and forth (its node is drawn interpolated between
 *            ticks). Its Skeleton3D sits under the model's Node3D; a sword
 *            (BoneAttachment3D on the left palm) and a hat (on the head)
 *            ride the bones, and a LookAtModifier3D under the skeleton turns
 *            the head to a firefly. The walk clip's bone tracks are addressed
 *            to the Skeleton3D node.
 *   Statue   a Skeleton3D that holds the model itself, with show_rest_only:
 *            its player runs the same walk, the skeleton holds the rest pose.
 *   Puppet   no animation: game code poses bones each tick through
 *            cw_skeleton3d_set_bone_pose (an arm waves a flag, the head nods).
 *
 * The probe runs in render, after the 3D draw: the sword's world matrix
 * against the palm bone's, as drawn that frame. Pad south is the gate's
 * control: the sword stops being a BoneAttachment3D and game code places it
 * from the bone transform it read a tick earlier, the classic one-tick lag,
 * and the probe fails. Debug fields: bones, attach_err / attach_ok, rest_bad
 * (statue bones off their rest), puppet_ok (poses read back as set), the
 * sword tip's world position, skeleton hashes per 60-tick window. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/anim.h"
#include "cartwheel/debug.h"
#include "cartwheel/input.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/skeleton.h"

static cw_node level, mover, model, skel, sword, tip_node, statue, puppet, puppet_skel, firefly, anim, statue_anim;
static t3_gltf *gltf[3];
static cw_action lag_action;
static float clock_s;
static int palm = -1;
static t3_mat4 palm_prev;

/* debug state */
static int32_t bones, attach_ok, attach_frames, rest_bad, puppet_ok = 1, lagging;
static float attach_err, tip[3];
static uint32_t hash_win[8], puppet_win[8];

static void set_shadows(t3_object *o) {
  if (o->type == T3_SKINNED_MESH || o->type == T3_MESH) o->cast_shadow = true;
  for (int i = 0; i < o->child_count; i++) set_shadows(o->children[i]);
}

static t3_gltf *load_into(cw_node n, const void *glb, size_t len) {
  t3_gltf *g = t3_gltf_parse(glb, len, NULL, NULL);
  cw_node3d_set_object(n, g->scene);
  set_shadows(g->scene);
  return g;
}

static void add_walk(cw_node player, t3_gltf *g, const char *path) {
  for (int k = 0; k < g->animation_count; k++) {
    if (strcmp(g->animations[k]->name, "Walking")) continue;
    cw_anim a = cw_anim_from_t3_clip(g->animations[k], path);
    cw_anim_set_loop(a, CW_LOOP_LINEAR);
    cw_anim_player_add(player, "walk", a);
    cw_anim_release(a);
  }
  cw_anim_player_play(player, "walk", 0);
}

static void init(void *u) {
  (void)u;
  lag_action = cw_action_add("lag", 0.5f);
  cw_action_bind_text(lag_action, "pad:south");
  level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
  mover = cw_node_get(level, "Mover");
  model = cw_node_get(mover, "Model");
  skel = cw_node_get(model, "Skeleton");
  sword = cw_node_get(skel, "Sword");
  tip_node = cw_node_get(sword, "Tip");
  statue = cw_node_get(level, "Statue");
  puppet = cw_node_get(level, "Puppet");
  puppet_skel = cw_node_get(puppet, "Skeleton");
  firefly = cw_node_get(level, "Firefly");
  anim = cw_node_get(level, "Anim");
  statue_anim = cw_node_get(level, "StatueAnim");
  size_t len;
  void *glb = cw_asset_load("RobotExpressive.glb", &len);
  if (!glb) { cw_error("skeleton_attach: RobotExpressive.glb missing from the assets"); return; }
  /* #region layouts */
  /* a model on a Node3D with a Skeleton3D under it; a Skeleton3D holding the model itself */
  gltf[0] = load_into(model, glb, len);
  gltf[1] = load_into(statue, glb, len);
  gltf[2] = load_into(puppet, glb, len);
  /* the first query binds a skeleton to the model just set: its rest pose is the file's, before any clip plays */
  bones = cw_skeleton3d_bone_count(skel) + 0 * (cw_skeleton3d_bone_count(statue) + cw_skeleton3d_bone_count(puppet_skel));
  /* bone tracks may name the Skeleton3D ("Mover/Model/Skeleton@Head:quaternion") or, as before, the model's node */
  add_walk(anim, gltf[0], "Mover/Model/Skeleton");
  add_walk(statue_anim, gltf[1], "Statue");
  /* #endregion */
  free(glb);

  cw_debug_i32("bones", &bones);
  cw_debug_i32("attach_ok", &attach_ok);
  cw_debug_i32("attach_frames", &attach_frames);
  cw_debug_f32("attach_err", &attach_err);
  cw_debug_i32("rest_bad", &rest_bad);
  cw_debug_i32("puppet_ok", &puppet_ok);
  cw_debug_i32("lagging", &lagging);
  cw_debug_f32_array("tip", tip, 3);
  cw_debug_field("hash_win", hash_win, CW_DBG_U32, 8);
  cw_debug_field("puppet_win", puppet_win, CW_DBG_U32, 8);
}

/* #region puppet */
/* pose bones from game code: the left arm waves the flag, the head nods */
static void pose_puppet(void) {
  int arm = cw_skeleton3d_find_bone(puppet_skel, "UpperArmL"), head = cw_skeleton3d_find_bone(puppet_skel, "Head");
  if (arm < 0 || head < 0) return;
  cw_bone_xform rest;
  float s, c;
  cw_sincos(clock_s * 2.2f, &s, &c);
  cw_skeleton3d_bone_rest(puppet_skel, arm, &rest);
  t3_quat swing = t3_quat_multiply(rest.rotation, cw_quat_axis_angle_det((t3_vec3){ 0, 0, 1 }, -1.1f + 0.5f * s));
  cw_skeleton3d_set_bone_pose(puppet_skel, arm, NULL, &swing, NULL);
  cw_skeleton3d_bone_rest(puppet_skel, head, &rest);
  t3_quat nod = t3_quat_multiply(rest.rotation, cw_quat_axis_angle_det((t3_vec3){ 1, 0, 0 }, 0.25f * c));
  cw_skeleton3d_set_bone_pose(puppet_skel, head, NULL, &nod, NULL);
  /* what was set is what the skeleton reports */
  cw_bone_xform now;
  if (!cw_skeleton3d_bone_pose(puppet_skel, head, &now) || memcmp(&now.rotation, &nod, sizeof nod)) puppet_ok = 0;
}
/* #endregion */

static void fixed(void *u, double dt) {
  (void)u;
  if (!gltf[0]) return;
  float fdt = (float)dt;
  uint64_t tick = cw_tree_tick(cw_scene_app_tree());
  clock_s += fdt;
  bones = cw_skeleton3d_bone_count(skel);
  if (palm < 0) palm = cw_skeleton3d_find_bone(skel, "Palm2L");

  /* the statue: every bone at rest although its player walks */
  int nb = cw_skeleton3d_bone_count(statue);
  for (int b = 0; b < nb && tick > 2; b++) {
    cw_bone_xform p, r;
    cw_skeleton3d_bone_pose(statue, b, &p);
    cw_skeleton3d_bone_rest(statue, b, &r);
    if (memcmp(&p, &r, sizeof p)) rest_bad++;
  }

  /* the control: place the sword by hand from the transform read a tick ago */
  if (cw_action_just_pressed(0, lag_action)) {
    lagging = 1;
    cw_node_set(sword, "bone_name", cw_val_string(""));
  }
  if (lagging && palm >= 0) {
    t3_vec3 p, s;
    t3_quat q;
    t3_mat4_decompose(&palm_prev, &p, &q, &s);
    cw_node3d_set_position(sword, p);
    cw_node3d_set_quaternion(sword, q);
    cw_node3d_set_scale(sword, s);
  }
  if (palm >= 0) cw_skeleton3d_bone_model_transform(skel, palm, &palm_prev);

  /* the mover paces left and right, facing the way it goes */
  float s, c;
  cw_sincos(clock_s * 0.9f, &s, &c);
  cw_node3d_snapshot(mover);
  cw_node3d_set_position(mover, (t3_vec3){ -1.6f + 0.9f * s, 0, 0.4f });
  cw_node3d_set_rotation(mover, (t3_vec3){ 0, c >= 0 ? 1.5707963f : -1.5707963f, 0 });
  cw_sincos(clock_s * 0.7f, &s, &c);
  cw_node3d_set_position(firefly, (t3_vec3){ -1.6f + 1.4f * s, 1.3f + 0.3f * c, 1.6f });

  pose_puppet();

  t3_vec3 tp = cw_node3d_global_position(tip_node);
  tip[0] = tp.x; tip[1] = tp.y; tip[2] = tp.z;
  if (tick / 60 < 8) {
    hash_win[tick / 60] = cw_skeleton3d_hash(skel);
    puppet_win[tick / 60] = cw_skeleton3d_hash(puppet_skel);
  }
}

/* #region probe */
/* after the 3D draw: the sword's world matrix against the palm bone's, as three.c drew them this frame */
static void render(void *u, double alpha) {
  (void)u, (void)alpha;
  t3_object *bone = palm >= 0 ? cw_skeleton3d_bone_object(skel, palm) : NULL, *att = cw_node3d_object(sword);
  if (!bone || !att || cw_frame_index() < 10) return;
  float worst = 0;
  for (int i = 0; i < 16; i++) worst = fmaxf(worst, fabsf(bone->matrix_world.e[i] - att->matrix_world.e[i]));
  if (worst > attach_err) attach_err = worst;
  attach_frames++;
  attach_ok = attach_err < 1e-4f; /* a tenth of a millimetre; a tick of lag is several millimetres */
}
/* #endregion */

/* the nodes hold the glTF scenes (retained); drop the app's references */
static void shutdown(void *u) {
  (void)u;
  for (int i = 0; i < 3; i++)
    if (gltf[i]) { t3_release(gltf[i]); gltf[i] = NULL; }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "skeleton_attach", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render, .shutdown = shutdown };
  return cw_scene_app("main.cwscene", &base);
}
