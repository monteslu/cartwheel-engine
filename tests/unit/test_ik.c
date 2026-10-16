/* Inverse kinematics: the pose layer, two-bone reach / pole plane / soften /
 * reached, aim with limits, CCD against three.js r186's CCDIKSolver
 * (tests/unit/ccdik_r186.h), FABRIK reach and constraints, and the skeleton
 * modifier nodes in a tree (no accumulation across ticks, influence, foot
 * placement on Box3D ground). Every solved pose folds into unit.checksum, so
 * every host must produce the same bits. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/ik.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/scene.h"
#include "ccdik_r186.h"
#include "test.h"

#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))
#define DT (1.0 / 60)

static t3_vec3 v3(float x, float y, float z) { return (t3_vec3){ x, y, z }; }
static t3_vec3 vsub(t3_vec3 a, t3_vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float vlen(t3_vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static float vdot(t3_vec3 a, t3_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static t3_vec3 vcross(t3_vec3 a, t3_vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static t3_vec3 vnorm(t3_vec3 a) { float l = vlen(a); return v3(a.x / l, a.y / l, a.z / l); }

/* three draws in a fixed order (argument evaluation order is unspecified) */
static t3_vec3 rand3(cw_rng *r, float y_lo) {
  float x = cw_rng_float(r) * 2 - 1;
  float y = y_lo < 0 ? cw_rng_float(r) * 2 - 1 : cw_rng_float(r);
  float z = cw_rng_float(r) * 2 - 1;
  return v3(x, y, z);
}

static void fold_pose(const cw_pose *p) { cw_test_checksum = cw_pose_hash(p, cw_test_checksum); }

/* a three-bone limb: a at the origin, b 1 up (slightly bent forward), c 1 above b */
static void limb(cw_pose *p) {
  cw_pose_init(p, 4);
  p->parent[1] = 0; p->parent[2] = 1; p->parent[3] = 2;
  p->pos[1] = v3(0, 0, 0);
  p->pos[2] = v3(0, 1, 0);
  p->pos[3] = v3(0, 1, 0);
  p->rot[1] = cw_quat_axis_angle_det(v3(1, 0, 0), 0.2f);
  p->rot[2] = cw_quat_axis_angle_det(v3(1, 0, 0), -0.4f);
  cw_pose_update_model(p);
}

static void test_pose(void) {
  cw_pose p;
  limb(&p);
  t3_vec3 c0 = cw_pose_model_position(&p, 3);
  /* a model rotation of the root carries its children */
  cw_pose_rotate_model(&p, 1, cw_quat_axis_angle_det(v3(0, 1, 0), 1.0f));
  t3_vec3 c1 = cw_pose_model_position(&p, 3);
  NEAR(vlen(c0), vlen(c1), 1e-6);
  NEAR(c0.y, c1.y, 1e-6);
  /* translate moves the subtree by exactly the offset in model space */
  p.rot[0] = cw_quat_axis_angle_det(v3(0, 0, 1), 0.7f);
  p.scl[0] = v3(2, 2, 2);
  cw_pose_update_model(&p);
  t3_vec3 b0 = cw_pose_model_position(&p, 2);
  cw_pose_translate_model(&p, 1, v3(0.25f, -0.5f, 1));
  t3_vec3 b1 = cw_pose_model_position(&p, 2);
  NEAR(b1.x - b0.x, 0.25, 1e-5);
  NEAR(b1.y - b0.y, -0.5, 1e-5);
  NEAR(b1.z - b0.z, 1, 1e-5);
  /* set_model_rotation round trips */
  t3_quat q = cw_quat_axis_angle_det(vnorm(v3(1, 2, 3)), 0.9f);
  cw_pose_set_model_rotation(&p, 2, q);
  t3_quat m = cw_pose_model_rotation(&p, 2);
  NEAR(fabsf(m.x * q.x + m.y * q.y + m.z * q.z + m.w * q.w), 1, 1e-6);
  CHECK(cw_pose_is_descendant(&p, 3, 1));
  CHECK(!cw_pose_is_descendant(&p, 1, 3));
  fold_pose(&p);
  cw_pose_free(&p);
}

static void test_two_bone(void) {
  cw_rng rng;
  cw_rng_seed(&rng, 1234);
  float worst = 0;
  int reached = 0;
  for (int i = 0; i < 200; i++) {
    cw_pose p;
    limb(&p);
    /* targets in reach (0.2 .. 1.95 from the root), any direction */
    t3_vec3 d = vnorm(rand3(&rng, -1));
    float r = 0.2f + cw_rng_float(&rng) * 1.75f;
    t3_vec3 t = v3(d.x * r, d.y * r, d.z * r);
    bool ok = cw_ik_two_bone(&p, 1, 2, 3, t, NULL);
    reached += ok;
    float e = vlen(vsub(cw_pose_model_position(&p, 3), t));
    if (e > worst) worst = e;
    /* bone lengths are kept */
    NEAR(vlen(vsub(cw_pose_model_position(&p, 2), cw_pose_model_position(&p, 1))), 1, 1e-5);
    NEAR(vlen(vsub(cw_pose_model_position(&p, 3), cw_pose_model_position(&p, 2))), 1, 1e-5);
    fold_pose(&p);
    cw_pose_free(&p);
  }
  CHECK_EQ_INT(reached, 200);
  CHECK(worst < 1e-4f);
  cw_info("two-bone: worst end-effector error %.3g over 200 targets", worst);

  /* the pole plane: the middle joint lies in the plane of root, target, pole, on the pole's side */
  cw_pose p;
  limb(&p);
  cw_ik2_opts o = CW_IK2_OPTS_DEFAULT;
  o.use_pole = true;
  o.pole = v3(-3, 0.5f, 0.2f);
  t3_vec3 t = v3(0.3f, 1.2f, 0.4f);
  CHECK(cw_ik_two_bone(&p, 1, 2, 3, t, &o));
  t3_vec3 b = cw_pose_model_position(&p, 2);
  t3_vec3 n = vnorm(vcross(t, o.pole));
  NEAR(vdot(n, b), 0, 1e-5);
  t3_vec3 u = vnorm(t);
  t3_vec3 side = vsub(o.pole, v3(u.x * vdot(o.pole, u), u.y * vdot(o.pole, u), u.z * vdot(o.pole, u)));
  CHECK(vdot(side, b) > 0);
  NEAR(vlen(vsub(cw_pose_model_position(&p, 3), t)), 0, 1e-4);
  fold_pose(&p);
  cw_pose_free(&p);

  /* no pole: the incoming bend plane (x = 0 here) is kept */
  limb(&p);
  t = v3(0, 0.8f, 0.9f);
  cw_ik_two_bone(&p, 1, 2, 3, t, NULL);
  NEAR(cw_pose_model_position(&p, 2).x, 0, 1e-6);
  cw_pose_free(&p);

  /* out of reach: reached is false and the chain points straight at the target */
  limb(&p);
  t = v3(0, 1, 3);
  CHECK(!cw_ik_two_bone(&p, 1, 2, 3, t, NULL));
  t3_vec3 c = cw_pose_model_position(&p, 3);
  NEAR(vlen(c), 2, 1e-4);
  NEAR(vdot(vnorm(c), vnorm(t)), 1, 1e-6);
  fold_pose(&p);
  cw_pose_free(&p);

  /* soften: near full extension the end stops short, smoothly */
  limb(&p);
  o = CW_IK2_OPTS_DEFAULT;
  o.soften = 0.9f;
  t = v3(0, 1.95f, 0);
  cw_ik_two_bone(&p, 1, 2, 3, t, &o);
  float d1 = vlen(cw_pose_model_position(&p, 3));
  CHECK(d1 < 1.95f && d1 > 1.8f);
  cw_pose_free(&p);
  limb(&p);
  t = v3(0, 1.7f, 0); /* below the soften start: exact */
  cw_ik_two_bone(&p, 1, 2, 3, t, &o);
  NEAR(vlen(cw_pose_model_position(&p, 3)), 1.7, 1e-5);
  cw_pose_free(&p);

  /* weight 0.5 lands between the incoming pose and the solution; end rotation */
  limb(&p);
  t3_vec3 c_in = cw_pose_model_position(&p, 3);
  o = CW_IK2_OPTS_DEFAULT;
  o.weight = 0.5f;
  t = v3(1, 0.5f, 0.5f);
  cw_ik_two_bone(&p, 1, 2, 3, t, &o);
  t3_vec3 c_half = cw_pose_model_position(&p, 3);
  CHECK(vlen(vsub(c_half, t)) > 1e-3f && vlen(vsub(c_half, c_in)) > 1e-3f);
  cw_pose_free(&p);
  limb(&p);
  o = CW_IK2_OPTS_DEFAULT;
  o.use_end_rotation = true;
  o.end_rotation = cw_quat_axis_angle_det(v3(0, 0, 1), 1.2f);
  cw_ik_two_bone(&p, 1, 2, 3, t, &o);
  t3_quat er = cw_pose_model_rotation(&p, 3);
  NEAR(fabsf(er.x * o.end_rotation.x + er.y * o.end_rotation.y + er.z * o.end_rotation.z + er.w * o.end_rotation.w), 1, 1e-6);
  fold_pose(&p);
  cw_pose_free(&p);
}

static void test_aim(void) {
  cw_pose p;
  limb(&p);
  cw_aim_opts o = CW_AIM_OPTS_DEFAULT;
  o.forward = v3(0, 1, 0); /* the bone's local +Y runs along it */
  t3_vec3 t = v3(2, 3, -1);
  CHECK(cw_ik_aim(&p, 2, t, &o));
  t3_vec3 b = cw_pose_model_position(&p, 2), c = cw_pose_model_position(&p, 3);
  NEAR(vdot(vnorm(vsub(c, b)), vnorm(vsub(t, b))), 1, 1e-6);
  fold_pose(&p);
  cw_pose_free(&p);
  /* limited: stops exactly at the limit from the incoming direction */
  limb(&p);
  t3_vec3 f0 = vnorm(vsub(cw_pose_model_position(&p, 3), cw_pose_model_position(&p, 2)));
  o.limit_angle = 0.5f;
  t = v3(0, -3, 0.5f);
  CHECK(!cw_ik_aim(&p, 2, t, &o));
  t3_vec3 f1 = vnorm(vsub(cw_pose_model_position(&p, 3), cw_pose_model_position(&p, 2)));
  NEAR(vdot(f0, f1), cw_cos(0.5f), 1e-5);
  fold_pose(&p);
  cw_pose_free(&p);
}

static void test_ccd_r186(void) {
  float worst = 0;
  for (int k = 0; k < CCD_CASES; k++) {
    const ccd_case *cc = &ccd_cases[k];
    cw_pose p;
    cw_pose_init(&p, CCD_BONES);
    for (int i = 0; i < CCD_BONES; i++) {
      p.parent[i] = ccd_parent[i];
      p.pos[i] = v3(ccd_pos[i][0], ccd_pos[i][1], ccd_pos[i][2]);
      p.rot[i] = (t3_quat){ ccd_rot[i][0], ccd_rot[i][1], ccd_rot[i][2], ccd_rot[i][3] };
    }
    p.pos[6] = v3(cc->target[0], cc->target[1], cc->target[2]);
    cw_pose_update_model(&p);
    cw_ccdik_link links[4];
    for (int j = 0; j < 4; j++) {
      int idx = 4 - j;
      links[j] = (cw_ccdik_link){ .index = idx };
      if (cc->limitation_link == idx) {
        links[j].has_limitation = true;
        links[j].limitation = v3(cc->limitation[0], cc->limitation[1], cc->limitation[2]);
      }
      if (cc->rot_link == idx) {
        links[j].has_rotation_min = links[j].has_rotation_max = true;
        links[j].rotation_min = v3(cc->rot_min[0], cc->rot_min[1], cc->rot_min[2]);
        links[j].rotation_max = v3(cc->rot_max[0], cc->rot_max[1], cc->rot_max[2]);
      }
      links[j].disabled = cc->disabled_link == idx;
    }
    cw_ccdik ik = { .target = 6, .effector = 5, .links = links, .link_count = 4, .iteration = cc->iteration,
                    .has_min_angle = cc->has_min, .min_angle = cc->min_angle, .has_max_angle = cc->has_max,
                    .max_angle = cc->max_angle, .has_blend_factor = cc->has_blend, .blend_factor = cc->blend };
    cw_ccdik_solve(&p, &ik, 1, 1);
    for (int j = 0; j < 4; j++) {
      t3_quat q = p.rot[j + 1];
      const float *e = cc->q[j];
      float d = fmaxf(fmaxf(fabsf(q.x - e[0]), fabsf(q.y - e[1])), fmaxf(fabsf(q.z - e[2]), fabsf(q.w - e[3])));
      if (d > worst) worst = d;
      if (d > 1e-5f) cw_error("ccd case %d link %d: (%g %g %g %g) vs r186 (%g %g %g %g)", k, j + 1, q.x, q.y, q.z, q.w, e[0], e[1], e[2], e[3]);
    }
    fold_pose(&p);
    cw_pose_free(&p);
  }
  CHECK(worst <= 1e-5f);
  cw_info("ccd: worst quaternion component difference from three.js r186: %.3g over %d cases", worst, CCD_CASES);
}

static void test_fabrik(void) {
  cw_rng rng;
  cw_rng_seed(&rng, 99);
  float worst = 0;
  for (int k = 0; k < 50; k++) {
    cw_pose p;
    cw_pose_init(&p, 6);
    for (int i = 1; i < 6; i++) {
      p.parent[i] = i - 1;
      p.pos[i] = v3(0, i == 1 ? 0 : 0.5f, 0);
      p.rot[i] = cw_quat_axis_angle_det(v3(1, 0, 0), 0.15f);
    }
    cw_pose_update_model(&p);
    int chain[5] = { 1, 2, 3, 4, 5 };
    t3_vec3 d = vnorm(rand3(&rng, 0));
    float r = 0.4f + cw_rng_float(&rng) * 1.4f;
    t3_vec3 t = v3(d.x * r, d.y * r, d.z * r);
    cw_chain_opts o = CW_CHAIN_OPTS_DEFAULT;
    o.max_iterations = 32;
    o.min_distance = 1e-4f;
    cw_ik_fabrik(&p, chain, 5, t, &o);
    float e = vlen(vsub(cw_pose_model_position(&p, 5), t));
    if (e > worst) worst = e;
    for (int i = 2; i < 6; i++) NEAR(vlen(vsub(cw_pose_model_position(&p, i), cw_pose_model_position(&p, i - 1))), 0.5, 1e-5);
    NEAR(vlen(cw_pose_model_position(&p, 1)), 0, 0);
    fold_pose(&p);
    cw_pose_free(&p);
  }
  CHECK(worst < 2e-3f);
  cw_info("fabrik: worst end error %.3g over 50 targets", worst);

  /* cones: every joint stays within its half-angle of its parent direction */
  cw_pose p;
  cw_pose_init(&p, 6);
  for (int i = 1; i < 6; i++) { p.parent[i] = i - 1; p.pos[i] = v3(0, i == 1 ? 0 : 0.5f, 0); }
  cw_pose_update_model(&p);
  int chain[5] = { 1, 2, 3, 4, 5 };
  cw_ik_joint j[5];
  for (int i = 0; i < 5; i++) j[i] = (cw_ik_joint){ .kind = CW_IK_JOINT_CONE, .half_angle = 0.3f };
  cw_chain_opts o = CW_CHAIN_OPTS_DEFAULT;
  o.joints = j;
  o.max_iterations = 16;
  cw_ik_fabrik(&p, chain, 5, v3(1.2f, 0.2f, 0), &o);
  for (int i = 2; i < 5; i++) {
    t3_vec3 a = vnorm(vsub(cw_pose_model_position(&p, i), cw_pose_model_position(&p, i - 1)));
    t3_vec3 b2 = vnorm(vsub(cw_pose_model_position(&p, i + 1), cw_pose_model_position(&p, i)));
    CHECK(vdot(a, b2) >= cw_cos(0.3f) - 1e-4f);
  }
  fold_pose(&p);
  cw_pose_free(&p);
  /* hinges about x: the chain stays in the x = 0 plane */
  cw_pose_init(&p, 6);
  for (int i = 1; i < 6; i++) { p.parent[i] = i - 1; p.pos[i] = v3(0, i == 1 ? 0 : 0.5f, 0); }
  cw_pose_update_model(&p);
  for (int i = 0; i < 5; i++) j[i] = (cw_ik_joint){ .kind = CW_IK_JOINT_HINGE, .axis = v3(1, 0, 0) };
  cw_ik_fabrik(&p, chain, 5, v3(0.6f, 1.0f, 0.8f), &o);
  for (int i = 1; i < 6; i++) NEAR(cw_pose_model_position(&p, i).x, 0, 1e-5);
  fold_pose(&p);
  cw_pose_free(&p);
}

/* ── nodes in a tree ───────────────────────────────────────────────── */

static cw_node add(cw_node parent, const char *type, const char *name) {
  cw_node n = cw_node_new(type, name);
  cw_node_add_child(parent, n);
  return n;
}

/* a two-legged rig of three.c bones: Hips at y 1, each leg 0.5 + 0.45 down to an ankle */
static t3_object *make_rig(void) {
  t3_object *root = t3_group_new();
  t3_object *hips = t3_bone_new();
  hips->name = "Hips";
  hips->position = v3(0, 1, 0);
  t3_object_add(root, hips);
  const char *names[2][3] = { { "UpL", "LoL", "AnkL" }, { "UpR", "LoR", "AnkR" } };
  for (int s = 0; s < 2; s++) {
    t3_object *up = t3_bone_new(), *lo = t3_bone_new(), *an = t3_bone_new();
    up->name = names[s][0]; lo->name = names[s][1]; an->name = names[s][2];
    up->position = v3(s ? -0.15f : 0.15f, 0, 0);
    t3_object_set_quaternion(up, cw_quat_axis_angle_det(v3(1, 0, 0), 3.14159265f - 0.1f)); /* pointing down, knee slightly forward */
    lo->position = v3(0, 0.5f, 0);
    t3_object_set_quaternion(lo, cw_quat_axis_angle_det(v3(1, 0, 0), 0.2f));
    an->position = v3(0, 0.45f, 0);
    t3_object_add(hips, up);
    t3_object_add(up, lo);
    t3_object_add(lo, an);
  }
  return root;
}

static void test_nodes(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node model = add(root, "Node3D", "Model");
  cw_node3d_set_position(model, v3(2, 0, -1));
  cw_node3d_set_rotation(model, v3(0, 0.6f, 0));
  t3_object *rig = make_rig();
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node arm = add(model, "TwoBoneIK3D", "Reach");
  cw_node_set(arm, "root_bone", cw_val_string("UpL"));
  cw_node_set(arm, "middle_bone", cw_val_string("LoL"));
  cw_node_set(arm, "end_bone", cw_val_string("AnkL"));
  cw_node goal = add(root, "Node3D", "Goal");
  cw_node3d_set_position(goal, v3(2.3f, 0.4f, -0.8f));
  cw_node_set(arm, "target", cw_val_string("../../Goal"));
  cw_tree_fixed_update(t, DT);
  t3_vec3 a = cw_skeleton_bone_world(model, "AnkL");
  NEAR(vlen(vsub(a, v3(2.3f, 0.4f, -0.8f))), 0, 1e-4);
  cw_value rv;
  CHECK(cw_node_get_prop(arm, "reached", &rv) && rv.b);
  /* nothing animates the rig: the next ticks start from the same pose, so the same result */
  uint32_t h1 = cw_skeleton_hash(model);
  const cw_pose *pose = cw_skeleton_pose(model);
  CHECK(pose != NULL);
  t3_quat q1 = pose->rot[cw_pose_find(pose, "UpL")];
  cw_tree_fixed_update(t, DT);
  cw_tree_fixed_update(t, DT);
  t3_quat q3 = pose->rot[cw_pose_find(pose, "UpL")];
  CHECK(!memcmp(&q1, &q3, sizeof q1));
  CHECK(cw_skeleton_hash(model) != h1);
  /* influence 0: the animated (rest) pose comes back */
  cw_node_set(arm, "influence", cw_val_float(0));
  cw_tree_fixed_update(t, DT);
  t3_object *up = t3_object_get_by_name(rig, "UpL");
  t3_quat rest = cw_quat_axis_angle_det(v3(1, 0, 0), 3.14159265f - 0.1f);
  CHECK(!memcmp(&up->quaternion, &rest, sizeof rest));
  /* half influence lands between */
  cw_node_set(arm, "influence", cw_val_float(0.5f));
  cw_tree_fixed_update(t, DT);
  t3_vec3 half = cw_skeleton_bone_world(model, "AnkL");
  CHECK(vlen(vsub(half, v3(2.3f, 0.4f, -0.8f))) > 1e-3f);
  cw_node_set(arm, "influence", cw_val_float(1));
  /* game code that poses a bone itself keeps that as the animated value */
  cw_node_set(arm, "active", cw_val_bool(false));
  cw_tree_fixed_update(t, DT);
  FOLD(h1);
  uint32_t h = cw_skeleton_hash(model);
  FOLD(h);

  /* look-at with a limit */
  cw_node look = add(model, "LookAtModifier3D", "Look");
  cw_node_set(look, "bone", cw_val_string("Hips"));
  cw_node_set(look, "forward_axis", cw_val_vec3(0, 0, 1));
  cw_node_set(look, "limit_angle", cw_val_float(0.5f));
  cw_node_set(look, "target_position", cw_val_vec3(10, 1, -1)); /* far to the side: outside the limit */
  cw_tree_fixed_update(t, DT);
  cw_value il;
  CHECK(cw_node_get_prop(look, "in_limit", &il) && !il.b);
  cw_node_set(look, "target_position", cw_val_vec3(2 + 3 * cw_sin(0.6f), 1, -1 + 3 * cw_cos(0.6f))); /* straight ahead */
  cw_tree_fixed_update(t, DT);
  CHECK(cw_node_get_prop(look, "in_limit", &il) && il.b);
  cw_node_free(look);
  cw_tree_free(t);
}

static void test_foot_placement(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  /* ground at y 0 under the right foot, a 0.2 m step under the left */
  cw_node floor = add(root, "StaticBody3D", "Floor");
  cw_node fs = add(floor, "CollisionShape3D", "Shape");
  cw_node_set(fs, "size", cw_val_vec3(10, 1, 10));
  cw_node3d_set_position(floor, v3(0, -0.5f, 0));
  cw_node step = add(root, "StaticBody3D", "Step");
  cw_node ss = add(step, "CollisionShape3D", "Shape");
  cw_node_set(ss, "size", cw_val_vec3(1, 0.2f, 2));
  cw_node3d_set_position(step, v3(0.6f, 0.1f, 0));
  cw_node model = add(root, "Node3D", "Model");
  t3_object *rig = make_rig();
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node feet = add(model, "FootPlacement3D", "Feet");
  cw_node_set(feet, "legs", cw_val_string("UpL,LoL,AnkL;UpR,LoR,AnkR"));
  cw_node_set(feet, "pelvis", cw_val_string("Hips"));
  cw_node_set(feet, "foot_height", cw_val_float(0.0f));
  cw_node_set(feet, "blend_speed", cw_val_float(0)); /* at once */
  for (int i = 0; i < 3; i++) cw_tree_fixed_update(t, DT);
  cw_foot_state l, r;
  CHECK(cw_foot_placement_leg(feet, 0, &l) && cw_foot_placement_leg(feet, 1, &r));
  CHECK(l.hit && r.hit);
  NEAR(l.ground.y, 0.2, 1e-4);
  NEAR(r.ground.y, 0, 1e-4);
  /* the animated ankles hang at y = 1 - 0.5cos(0.1) - 0.45cos(0.1 - 0.2)...; each lands at its offset */
  const cw_pose *p = cw_skeleton_pose(model);
  t3_object *al = t3_object_get_by_name(rig, "AnkL");
  (void)al;
  t3_vec3 wl = cw_skeleton_bone_world(model, "AnkL"), wr = cw_skeleton_bone_world(model, "AnkR");
  /* rest ankle height (no IK) */
  cw_node_set(feet, "active", cw_val_bool(false));
  cw_tree_fixed_update(t, DT);
  cw_tree_fixed_update(t, DT);
  t3_object *ar = t3_object_get_by_name(rig, "AnkR");
  t3_object_update_matrix_world(rig, true);
  float rest_y = ar->matrix_world.e[13];
  cw_info("foot: wl %g wr %g rest %g", wl.y, wr.y, rest_y);
  NEAR(wl.y - rest_y, 0.2, 1e-3);
  NEAR(wr.y - rest_y, 0, 1e-3);
  CHECK(cw_foot_placement_pelvis_offset(feet) <= 0);
  (void)p;
  /* a ground lower than the model's origin pulls the pelvis down */
  cw_node3d_set_position(model, v3(0, 0.15f, 0));
  cw_node_set(feet, "active", cw_val_bool(true));
  for (int i = 0; i < 3; i++) cw_tree_fixed_update(t, DT);
  NEAR(cw_foot_placement_pelvis_offset(feet), -0.15, 1e-4);
  wr = cw_skeleton_bone_world(model, "AnkR");
  NEAR(wr.y - rest_y, 0, 2e-3);
  FOLD(wl);
  FOLD(wr);
  uint32_t h = cw_skeleton_hash(model);
  FOLD(h);
  cw_tree_free(t);
}

CW_TEST_SUITE("ik", { "pose", test_pose }, { "two_bone", test_two_bone }, { "aim", test_aim }, { "ccd_r186", test_ccd_r186 },
              { "fabrik", test_fabrik }, { "nodes", test_nodes }, { "foot_placement", test_foot_placement })
