/* Skeleton3D and BoneAttachment3D (cartwheel/skeleton.h): binding in both
 * layouts, bone queries, rest and pose, attachments that follow, modifiers
 * and bone tracks through a Skeleton3D, the scene-file round trip, and
 * ownership (a freed skeleton lets go of its bones). */
#include <math.h>
#include <string.h>

#include "cartwheel/anim.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/skeleton.h"
#include "test.h"

#define DT (1.0 / 60)
#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

static inline t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static float dist(t3_vec3 a, t3_vec3 b) {
  float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
  return sqrtf(x * x + y * y + z * z);
}
static t3_vec3 mpos(const t3_mat4 *m) { return v3(m->e[12], m->e[13], m->e[14]); }

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* Hips at y 1 with two three-bone legs, and an arm: Shoulder, Elbow, Hand along +X */
static t3_object *make_rig(void) {
  t3_object *root = t3_group_new();
  t3_object *arm = t3_group_new(); /* a glTF armature node between the model and the bones */
  arm->name = "Armature";
  arm->position = v3(0, 0.1f, 0);
  t3_object_add(root, arm);
  t3_release(arm);
  t3_object *hips = t3_bone_new();
  hips->name = "Hips";
  hips->position = v3(0, 1, 0);
  t3_object_add(arm, hips);
  t3_release(hips);
  const char *names[3] = { "Shoulder", "Elbow", "Hand" };
  t3_object *par = hips;
  for (int i = 0; i < 3; i++) {
    t3_object *b = t3_bone_new();
    b->name = names[i];
    b->position = v3(i ? 0.4f : 0.2f, i ? 0 : 0.5f, 0);
    t3_object_add(par, b);
    t3_release(b);
    par = b;
  }
  t3_object *head = t3_bone_new();
  head->name = "Head";
  head->position = v3(0, 0.7f, 0);
  t3_object_add(hips, head);
  t3_release(head);
  return root;
}

/* the left column of skeleton.h: a Skeleton3D under the model's Node3D */
static cw_node make_model(cw_node parent, const char *name, t3_object **rig_out) {
  cw_node model = add(parent, "Node3D", name);
  t3_object *rig = make_rig();
  cw_node3d_set_object(model, rig);
  if (rig_out) *rig_out = rig;
  t3_release(rig);
  return model;
}

static void test_bind(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  /* a skeleton whose model has no bones yet: unbound, every query says so */
  cw_node empty = add(root, "Node3D", "Empty");
  cw_node es = add(empty, "Skeleton3D", "Skeleton");
  CHECK_EQ_INT(cw_skeleton3d_bone_count(es), 0);
  CHECK_EQ_INT(cw_skeleton3d_find_bone(es, "Hips"), -1);
  CHECK(cw_skeleton3d_bone_name(es, 0) == NULL);
  CHECK(!cw_skeleton3d_bone_pose(es, 0, NULL));
  CHECK(cw_skeleton3d_pose(es) == NULL);
  /* the model arrives later (a mesh resource that finished loading): it binds at the next tick */
  t3_object *rig = make_rig();
  t3_object_add(cw_node3d_object(empty), rig); /* as a MeshInstance3D adds its glTF instance */
  t3_release(rig);
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton3d_bone_count(es), 5);
  CHECK(cw_skeleton3d_model(es).id == empty.id);
  uint32_t g1 = cw_skeleton3d_generation(es);
  /* names, parents before children */
  CHECK_EQ_INT(cw_skeleton3d_find_bone(es, "Hips"), 0);
  int hand = cw_skeleton3d_find_bone(es, "Hand");
  CHECK(hand > 0);
  CHECK(!strcmp(cw_skeleton3d_bone_name(es, hand), "Hand"));
  CHECK_EQ_INT(cw_skeleton3d_bone_parent(es, 0), -1);
  for (int b = 1; b < 5; b++) CHECK(cw_skeleton3d_bone_parent(es, b) < b && cw_skeleton3d_bone_parent(es, b) >= 0);
  CHECK(cw_skeleton3d_bone_object(es, hand) == t3_object_get_by_name(rig, "Hand"));
  CHECK(cw_skeleton3d_bone_name(es, 5) == NULL);
  CHECK(cw_skeleton3d_bone_name(es, -1) == NULL);
  /* the model's objects are replaced: it binds to the new bones, and lets the old ones go */
  t3_object *old_hand = t3_retain(t3_object_get_by_name(rig, "Hand"));
  t3_object *rig2 = make_rig();
  t3_object_remove(cw_node3d_object(empty), rig);
  t3_object_add(cw_node3d_object(empty), rig2);
  t3_release(rig2);
  cw_tree_fixed_update(t, DT);
  CHECK(cw_skeleton3d_generation(es) != g1);
  CHECK(cw_skeleton3d_bone_object(es, hand) == t3_object_get_by_name(rig2, "Hand"));
  CHECK_EQ_INT(old_hand->_refs, 1); /* only ours: the skeleton dropped it (its parent is gone too) */
  t3_release(old_hand);

  /* the right column: the Skeleton3D holds the model */
  cw_node own = add(root, "Skeleton3D", "Own");
  t3_object *rig3 = make_rig();
  cw_node3d_set_object(own, rig3);
  t3_release(rig3);
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton3d_bone_count(own), 5);
  CHECK(cw_skeleton3d_model(own).id == own.id);
  /* an explicit model path */
  cw_node far = add(root, "Skeleton3D", "Far");
  cw_node_set(far, "model", cw_val_string("../Empty"));
  cw_tree_fixed_update(t, DT);
  CHECK(cw_skeleton3d_model(far).id == empty.id);
  cw_value v;
  CHECK(cw_node_get_prop(far, "bone_count", &v) && v.i == 5);
  CHECK(cw_node_get_prop(far, "bound", &v) && v.b);
  /* freeing a skeleton releases its bones: the rig's own references are all that is left */
  t3_object *h3 = t3_retain(t3_object_get_by_name(rig3, "Hips"));
  int refs_bound = h3->_refs;
  cw_node_free(own);
  cw_tree_fixed_update(t, DT);
  CHECK(refs_bound >= 2);
  CHECK_EQ_INT(h3->_refs, 1);
  t3_release(h3);
  cw_tree_free(t);
}

static void test_pose(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  t3_object *rig;
  cw_node model = make_model(root, "Model", &rig);
  cw_node3d_set_position(model, v3(3, 0, -2));
  cw_node3d_set_rotation(model, v3(0, 1.5707963f, 0));
  cw_node3d_set_scale(model, v3(2, 2, 2));
  cw_node sk = add(model, "Skeleton3D", "Skeleton");
  CHECK_EQ_INT(cw_skeleton3d_bone_count(sk), 5); /* the bones were there: bound on entering the tree */
  int sh = cw_skeleton3d_find_bone(sk, "Shoulder"), el = cw_skeleton3d_find_bone(sk, "Elbow"),
      hand = cw_skeleton3d_find_bone(sk, "Hand");
  /* model space: the armature offset, the hips, then the arm along +X */
  t3_mat4 m;
  CHECK(cw_skeleton3d_bone_model_transform(sk, hand, &m));
  NEAR(dist(mpos(&m), v3(1.0f, 1.6f, 0)), 0, 1e-6);
  /* world: scaled by 2, turned a quarter about Y (+X goes to -Z), moved */
  CHECK(cw_skeleton3d_bone_global_transform(sk, hand, &m));
  NEAR(dist(mpos(&m), v3(3, 3.2f, -4)), 0, 1e-5);
  /* pose: read, set, read back; rest untouched */
  cw_bone_xform x, rest;
  CHECK(cw_skeleton3d_bone_pose(sk, el, &x));
  NEAR(x.position.x, 0.4f, 0);
  t3_quat bend = cw_quat_axis_angle_det(v3(0, 0, 1), 1.5707963f); /* the forearm points up */
  cw_skeleton3d_set_bone_pose(sk, el, NULL, &bend, NULL);
  CHECK(cw_skeleton3d_bone_pose(sk, el, &x));
  CHECK(!memcmp(&x.rotation, &t3_object_get_by_name(rig, "Elbow")->quaternion, sizeof(t3_quat)));
  NEAR(x.position.x, 0.4f, 0);
  CHECK(cw_skeleton3d_bone_model_transform(sk, hand, &m));
  NEAR(dist(mpos(&m), v3(0.6f, 2.0f, 0)), 0, 1e-6);
  CHECK(cw_skeleton3d_bone_rest(sk, el, &rest));
  NEAR(rest.rotation.w, 1, 0);
  CHECK(cw_skeleton3d_bone_rest_model_transform(sk, hand, &m));
  NEAR(dist(mpos(&m), v3(1.0f, 1.6f, 0)), 0, 1e-6);
  uint32_t h_bent = cw_skeleton3d_hash(sk);
  FOLD(h_bent);
  /* a cw_pose of the same bones, for the solvers */
  cw_pose *p = cw_skeleton3d_pose(sk);
  CHECK(p && p->count == 5);
  NEAR(dist(cw_pose_model_position(p, hand), v3(0.6f, 2.0f, 0)), 0, 1e-6);
  /* reset one bone, then set and reset all */
  cw_skeleton3d_reset_bone_pose(sk, el);
  uint32_t h_rest = cw_skeleton3d_hash(sk);
  CHECK(h_rest != h_bent);
  t3_vec3 up = v3(0, 0.3f, 0);
  cw_skeleton3d_set_bone_pose(sk, sh, &up, &bend, NULL);
  cw_skeleton3d_reset_bone_pose(sk, -1);
  CHECK_EQ_INT(cw_skeleton3d_hash(sk), h_rest);
  /* show_rest_only holds the rest pose against whatever wrote the bones this tick */
  cw_skeleton3d_set_bone_pose(sk, el, NULL, &bend, NULL);
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton3d_hash(sk), h_bent);
  cw_node_set(sk, "show_rest_only", cw_val_bool(true));
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton3d_hash(sk), h_rest);
  /* a new rest */
  cw_bone_xform nr = { v3(0.4f, 0, 0), bend, v3(1, 1, 1) };
  cw_skeleton3d_set_bone_rest(sk, el, &nr);
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton3d_hash(sk), h_bent);
  /* out of range: refused, nothing written */
  cw_skeleton3d_set_bone_pose(sk, 99, &up, NULL, NULL);
  CHECK(!cw_skeleton3d_bone_pose(sk, 99, &x));
  CHECK_EQ_INT(cw_skeleton3d_hash(sk), h_bent);
  cw_tree_free(t);
}

static void test_attachment(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  t3_object *rig;
  cw_node mover = add(root, "Node3D", "Mover");
  cw_node model = make_model(mover, "Model", &rig);
  cw_node3d_set_scale(model, v3(0.5f, 0.5f, 0.5f));
  cw_node sk = add(model, "Skeleton3D", "Skeleton");
  cw_node sword = add(sk, "BoneAttachment3D", "Sword");
  cw_node_set(sword, "bone_name", cw_val_string("Hand"));
  cw_node tip = add(sword, "Node3D", "Tip");
  cw_node3d_set_position(tip, v3(0, 1, 0));
  int hand = cw_skeleton3d_find_bone(sk, "Hand");
  CHECK_EQ_INT(cw_bone_attachment3d_bone(sword), hand);
  CHECK(cw_bone_attachment3d_skeleton(sword).id == sk.id);
  t3_mat4 bw;
  float worst = 0;
  for (int k = 0; k < 30; k++) {
    /* the model moves and turns, the arm swings: all in the tick, before the attachment phase */
    cw_node3d_set_position(mover, v3(0.3f * (float)k, 0, 0.1f * (float)k));
    cw_node3d_set_rotation(mover, v3(0, 0.2f * (float)k, 0));
    t3_quat q = cw_quat_axis_angle_det(v3(0, 0, 1), 0.1f * (float)k);
    cw_skeleton3d_set_bone_pose(sk, cw_skeleton3d_find_bone(sk, "Shoulder"), NULL, &q, NULL);
    cw_tree_fixed_update(t, DT);
    cw_skeleton3d_bone_global_transform(sk, hand, &bw);
    const t3_mat4 *aw = cw_node3d_global_transform(sword);
    float d = dist(mpos(aw), mpos(&bw));
    for (int e = 0; e < 12; e++) d = fmaxf(d, fabsf(aw->e[e] - bw.e[e])); /* the axes too: rotation and scale */
    if (d > worst) worst = d;
    t3_vec3 tp = cw_node3d_global_position(tip);
    FOLD(tp);
  }
  NEAR(worst, 0, 2e-5);
  /* a bone moved after the tick (the app's own fixed_update, an UPDATE-mode player): the frame's attachment has it */
  t3_quat q = cw_quat_axis_angle_det(v3(0, 0, 1), -1.0f);
  cw_skeleton3d_set_bone_pose(sk, cw_skeleton3d_find_bone(sk, "Elbow"), NULL, &q, NULL);
  cw_skeleton3d_bone_global_transform(sk, hand, &bw);
  CHECK(dist(cw_node3d_global_position(sword), mpos(&bw)) > 0.05f); /* not yet */
  cw_tree_pre_render(t, 1.0);
  NEAR(dist(cw_node3d_global_position(sword), mpos(&bw)), 0, 2e-5);
  /* the three.c object the renderer draws took it in the same pre_render */
  t3_object *so = cw_node3d_object(sword);
  t3_vec3 lp = cw_node3d_position(sword);
  CHECK(!memcmp(&so->position, &lp, sizeof lp));
  /* another bone by name; an unknown bone leaves it where it was and says -1 */
  cw_node_set(sword, "bone_name", cw_val_string("Head"));
  cw_tree_fixed_update(t, DT);
  cw_skeleton3d_bone_global_transform(sk, cw_skeleton3d_find_bone(sk, "Head"), &bw);
  NEAR(dist(cw_node3d_global_position(sword), mpos(&bw)), 0, 2e-5);
  cw_node_set(sword, "bone_name", cw_val_string("Tail"));
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_bone_attachment3d_bone(sword), -1);
  /* an attachment outside the model (a `skeleton` path), with a transformed parent */
  cw_node shelf = add(root, "Node3D", "Shelf");
  cw_node3d_set_position(shelf, v3(-4, 2, 1));
  cw_node3d_set_rotation(shelf, v3(0.3f, 0.4f, 0));
  cw_node ext = add(shelf, "BoneAttachment3D", "Ext");
  cw_node_set(ext, "skeleton", cw_val_string("../../Mover/Model/Skeleton"));
  cw_node_set(ext, "bone_name", cw_val_string("Hand"));
  cw_tree_fixed_update(t, DT);
  cw_skeleton3d_bone_global_transform(sk, hand, &bw);
  NEAR(dist(cw_node3d_global_position(ext), mpos(&bw)), 0, 5e-5);
  /* a Skeleton3D with its own offset under the model: attachments still land on the bone */
  cw_node3d_set_position(sk, v3(0.5f, -0.25f, 2));
  cw_node3d_set_rotation(sk, v3(0, 0.7f, 0.2f));
  cw_node_set(sword, "bone_name", cw_val_string("Hand"));
  cw_tree_fixed_update(t, DT);
  NEAR(dist(cw_node3d_global_position(sword), mpos(&bw)), 0, 5e-5);
  cw_tree_free(t);
}

/* modifiers under a Skeleton3D solve exactly as modifiers under the model do */
static uint32_t reach_run(bool under_skeleton, float *err) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node model = make_model(root, "Model", NULL);
  cw_node3d_set_position(model, v3(1, 0, 2));
  cw_node3d_set_rotation(model, v3(0, 0.4f, 0));
  cw_node sk = add(model, "Skeleton3D", "Skeleton");
  cw_node3d_set_position(sk, v3(5, 5, 5)); /* the skeleton node's own transform is not model space */
  cw_node goal = add(root, "Node3D", "Goal");
  cw_node3d_set_position(goal, v3(1.5f, 1.9f, 1.8f));
  cw_node holder = under_skeleton ? sk : model;
  cw_node arm = add(holder, "TwoBoneIK3D", "Reach");
  cw_node_set(arm, "root_bone", cw_val_string("Shoulder"));
  cw_node_set(arm, "middle_bone", cw_val_string("Elbow"));
  cw_node_set(arm, "end_bone", cw_val_string("Hand"));
  cw_node_set(arm, "use_pole", cw_val_bool(true));
  cw_node_set(arm, "pole_position", cw_val_vec3(1, 4, 2));
  cw_node_set(arm, "target", cw_val_string(under_skeleton ? "../../../Goal" : "../../Goal"));
  for (int k = 0; k < 4; k++) cw_tree_fixed_update(t, DT);
  t3_mat4 m;
  cw_skeleton3d_bone_global_transform(sk, cw_skeleton3d_find_bone(sk, "Hand"), &m);
  *err = dist(mpos(&m), v3(1.5f, 1.9f, 1.8f));
  cw_value rv;
  CHECK(cw_node_get_prop(arm, "reached", &rv) && rv.b);
  CHECK(cw_skeleton_pose(holder) != NULL);
  uint32_t h = cw_skeleton3d_hash(sk);
  cw_tree_free(t);
  return h;
}

static void test_modifiers(void) {
  float e1, e2;
  uint32_t a = reach_run(false, &e1), b = reach_run(true, &e2);
  NEAR(e1, 0, 1e-4);
  NEAR(e2, 0, 1e-4);
  CHECK_EQ_INT(a, b);
  FOLD(a);
}

/* a bone track addressed to the Skeleton3D writes the model's bones */
static void test_tracks(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  t3_object *rig;
  cw_node model = make_model(root, "Model", &rig);
  cw_node sk = add(model, "Skeleton3D", "Skeleton");
  cw_node hat = add(sk, "BoneAttachment3D", "Hat");
  cw_node_set(hat, "bone_name", cw_val_string("Hand"));
  cw_node player = add(root, "AnimationPlayer", "Anim");
  cw_anim a = cw_anim_new("wave", 1, CW_LOOP_LINEAR);
  int tr = cw_anim_add_value_track(a, "Model/Skeleton@Shoulder:quaternion", CW_INTERP_LINEAR);
  t3_quat q0 = { 0, 0, 0, 1 }, q1 = cw_quat_axis_angle_det(v3(0, 0, 1), 1.2f);
  cw_value k0 = { CW_T_QUAT }, k1 = { CW_T_QUAT };
  memcpy(k0.v, &q0, 16);
  memcpy(k1.v, &q1, 16);
  cw_anim_add_key(a, tr, 0, k0);
  cw_anim_add_key(a, tr, 1, k1);
  cw_anim_player_add(player, "wave", a);
  cw_anim_release(a);
  cw_anim_player_play(player, "wave", 0);
  for (int k = 0; k < 30; k++) cw_tree_fixed_update(t, DT);
  cw_bone_xform x;
  CHECK(cw_skeleton3d_bone_pose(sk, cw_skeleton3d_find_bone(sk, "Shoulder"), &x));
  CHECK(x.rotation.z > 0.2f); /* half way through the wave */
  /* the attachment took this tick's animated bone, not last tick's */
  t3_mat4 bw;
  cw_skeleton3d_bone_global_transform(sk, cw_skeleton3d_find_bone(sk, "Hand"), &bw);
  NEAR(dist(cw_node3d_global_position(hat), mpos(&bw)), 0, 2e-5);
  uint32_t h = cw_skeleton3d_hash(sk);
  FOLD(h);
  cw_tree_free(t);
}

static void test_scene_file(void) {
  static const char *text =
    "[scene format=1]\n\n"
    "[node name=\"Model\" type=\"Node3D\"]\n\n"
    "[node name=\"Skeleton\" parent=\".\" type=\"Skeleton3D\"]\n"
    "model = \"..\"\n"
    "show_rest_only = true\n\n"
    "[node name=\"Sword\" parent=\"Skeleton\" type=\"BoneAttachment3D\"]\n"
    "bone_name = \"Hand\"\n\n"
    "[node name=\"Blade\" parent=\"Skeleton/Sword\" type=\"MeshInstance3D\"]\n"
    "position = vec3(0, 0.5, 0)\n\n"
    "[node name=\"Far\" parent=\".\" type=\"BoneAttachment3D\"]\n"
    "bone_name = \"Head\"\n"
    "skeleton = \"../Skeleton\"\n";
  cw_packed_scene *ps = cw_scene_parse(text, NULL);
  CHECK(ps != NULL);
  if (!ps) return;
  cw_node n = cw_scene_instance(ps);
  cw_value v;
  CHECK(cw_node_is(cw_node_get(n, "Skeleton"), "Skeleton3D"));
  CHECK(cw_node_get_prop(cw_node_get(n, "Skeleton"), "show_rest_only", &v) && v.b);
  CHECK(cw_node_get_prop(cw_node_get(n, "Skeleton"), "model", &v) && !strcmp(v.s, ".."));
  CHECK(cw_node_get_prop(cw_node_get(n, "Skeleton/Sword"), "bone_name", &v) && !strcmp(v.s, "Hand"));
  /* in a tree with a model, the loaded nodes work */
  cw_tree *t = cw_tree_new();
  t3_object *rig = make_rig();
  cw_node3d_set_object(n, rig);
  t3_release(rig);
  cw_node_add_child(cw_tree_root(t), n);
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton3d_bone_count(cw_node_get(n, "Skeleton")), 5);
  CHECK(cw_bone_attachment3d_bone(cw_node_get(n, "Skeleton/Sword")) >= 0);
  CHECK(cw_bone_attachment3d_bone(cw_node_get(n, "Far")) >= 0);
  /* save, load the saved text, save again: the same text. The read-only counts are not in it; an attachment's
   * transform is (it is a Node3D), and is overwritten from its bone on load */
  char *s1 = cw_scene_save(n);
  CHECK(strstr(s1, "type=\"Skeleton3D\"") && strstr(s1, "type=\"BoneAttachment3D\""));
  CHECK(strstr(s1, "show_rest_only = true") && strstr(s1, "bone_name = \"Hand\"") && strstr(s1, "skeleton = \"../Skeleton\""));
  CHECK(!strstr(s1, "bone_count") && !strstr(s1, "bone_idx") && !strstr(s1, "bound ="));
  cw_packed_scene *ps2 = cw_scene_parse(s1, NULL);
  CHECK(ps2 != NULL);
  if (ps2) {
    cw_node n2 = cw_scene_instance(ps2);
    t3_object *rig2 = make_rig();
    cw_node3d_set_object(n2, rig2);
    t3_release(rig2);
    cw_node_add_child(add(cw_tree_root(t), "Node3D", "Other"), n2);
    cw_tree_fixed_update(t, DT);
    char *s2 = cw_scene_save(n2);
    CHECK(!strcmp(s1, s2));
    if (strcmp(s1, s2)) cw_error("first save:\n%s\nsecond save:\n%s", s1, s2);
    uint32_t h = cw_hash32(s2, strlen(s2), CW_HASH32_INIT);
    FOLD(h);
    cw_free(s2);
    cw_packed_scene_free(ps2);
  }
  cw_free(s1);
  cw_tree_free(t);
  cw_packed_scene_free(ps);
}

CW_TEST_SUITE("skeleton", { "bind", test_bind }, { "pose", test_pose }, { "attachment", test_attachment },
              { "modifiers", test_modifiers }, { "tracks", test_tracks }, { "scene_file", test_scene_file })
