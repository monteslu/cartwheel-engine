/* Inverse kinematics: the pose layer, two-bone reach / pole plane / soften /
 * reached, aim with limits, CCD against three.js r187's CCDIKSolver
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
      if (d > 1e-5f) cw_error("ccd case %d link %d: (%g %g %g %g) vs r187 (%g %g %g %g)", k, j + 1, q.x, q.y, q.z, q.w, e[0], e[1], e[2], e[3]);
    }
    fold_pose(&p);
    cw_pose_free(&p);
  }
  CHECK(worst <= 1e-5f);
  cw_info("ccd: worst quaternion component difference from three.js r187: %.3g over %d cases", worst, CCD_CASES);
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

/* a crowd of 12 rigs (past the job-pool threshold): every model solves as
 * one job, and the poses must not depend on the worker count
 * (tests/ik-determinism.mjs runs this suite at CW_WORKERS 0, 3 and 7) */
static void test_crowd(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node models[12];
  for (int i = 0; i < 12; i++) {
    char name[16];
    snprintf(name, sizeof name, "M%d", i);
    models[i] = add(root, "Node3D", name);
    cw_node3d_set_position(models[i], v3((float)i, 0, 0));
    t3_object *rig = make_rig();
    cw_node3d_set_object(models[i], rig);
    t3_release(rig);
    cw_node r = add(models[i], "TwoBoneIK3D", "Reach");
    cw_node_set(r, "root_bone", cw_val_string("UpR"));
    cw_node_set(r, "middle_bone", cw_val_string("LoR"));
    cw_node_set(r, "end_bone", cw_val_string("AnkR"));
    cw_node_set(r, "use_pole", cw_val_bool(true));
    cw_node_set(r, "pole_position", cw_val_vec3((float)i, 0.5f, 2));
    cw_node l = add(models[i], "LookAtModifier3D", "Look");
    cw_node_set(l, "bone", cw_val_string("Hips"));
    cw_node_set(l, "limit_angle", cw_val_float(0.4f + 0.05f * (float)i));
  }
  for (int k = 0; k < 20; k++) {
    for (int i = 0; i < 12; i++) {
      float s, c;
      cw_sincos(0.3f * (float)k + (float)i, &s, &c);
      cw_node_set(cw_node_get(models[i], "Reach"), "target_position", cw_val_vec3((float)i - 0.2f + 0.3f * s, 0.4f, 0.3f * c));
      cw_node_set(cw_node_get(models[i], "Look"), "target_position", cw_val_vec3((float)i + 2 * s, 1, 2 * c));
    }
    cw_tree_fixed_update(t, DT);
  }
  int reached = 0;
  for (int i = 0; i < 12; i++) {
    uint32_t h = cw_skeleton_hash(models[i]);
    FOLD(h);
    cw_value rv;
    if (cw_node_get_prop(cw_node_get(models[i], "Reach"), "reached", &rv) && rv.b) reached++;
  }
  CHECK_EQ_INT(reached, 12);
  cw_tree_free(t);
}

/* ── SpringBone3D ──────────────────────────────────────────────────── */

/* a head bone at y 1.5 with a four-bone tail hanging down behind it (0.1 m segments) and an end marker */
static t3_object *make_tail_rig(void) {
  t3_object *root = t3_group_new();
  t3_object *head = t3_bone_new();
  head->name = "Head";
  head->position = v3(0, 1.5f, 0);
  t3_object_add(root, head);
  t3_object *parent = head;
  static const char *names[5] = { "T0", "T1", "T2", "T3", "T_end" };
  for (int i = 0; i < 5; i++) {
    t3_object *b = t3_bone_new();
    b->name = names[i];
    b->position = i ? v3(0, -0.1f, -0.01f) : v3(0, 0, -0.1f);
    t3_object_add(parent, b);
    parent = b;
  }
  return root;
}

static float tip_dist(cw_node chain) {
  cw_value v;
  return cw_node_get_prop(chain, "tip_distance", &v) ? (float)v.f : -1;
}

static void test_spring(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node model = add(root, "Node3D", "Model");
  t3_object *rig = make_tail_rig();
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node sb = add(model, "SpringBone3D", "Springs");
  cw_node ch = add(sb, "SpringBoneChain3D", "Tail");
  cw_node_set(ch, "root_bone", cw_val_string("T0"));
  cw_node_set(ch, "end_bone", cw_val_string("T3"));
  /* still: the tail stays on its animated pose */
  for (int k = 0; k < 30; k++) cw_tree_fixed_update(t, DT);
  NEAR(tip_dist(ch), 0, 1e-5);
  t3_vec3 j;
  CHECK(cw_spring_chain3d_joint_world(ch, 3, &j) && !cw_spring_chain3d_joint_world(ch, 4, &j));
  /* a steady walk: the tail trails at the start, then settles back onto the animated pose */
  float peak = 0, x = 0;
  for (int k = 0; k < 240; k++) {
    x += 1.4f * (float)DT;
    cw_node3d_set_position(model, v3(x, 0, 0));
    cw_tree_fixed_update(t, DT);
    if (tip_dist(ch) > peak) peak = tip_dist(ch);
  }
  CHECK(peak > 0.02f);
  CHECK(tip_dist(ch) < 0.002f);
  fprintf(stderr, "spring walk: peak %.4f end %.5f\n", peak, tip_dist(ch));
  /* a stop: it swings forward, then settles within two and a half seconds */
  peak = 0;
  for (int k = 0; k < 150; k++) {
    cw_tree_fixed_update(t, DT);
    if (tip_dist(ch) > peak) peak = tip_dist(ch);
  }
  fprintf(stderr, "spring stop: peak %.4f end %.5f\n", peak, tip_dist(ch));
  CHECK(peak > 0.02f);
  CHECK(tip_dist(ch) < 0.005f);
  /* gravity sags it, by about strength / stiffness per joint */
  cw_node_set(ch, "gravity_strength", cw_val_float(1.2));
  for (int k = 0; k < 120; k++) cw_tree_fixed_update(t, DT);
  float sag = tip_dist(ch);
  fprintf(stderr, "spring sag: %.4f\n", sag);
  CHECK(sag > 0.005f && sag < 0.1f);
  cw_node_set(ch, "gravity_strength", cw_val_float(0));
  for (int k = 0; k < 120; k++) cw_tree_fixed_update(t, DT);
  /* a teleport: the tail moves with the model, no whip; with the check off it whips */
  cw_node3d_set_position(model, v3(x + 5, 0, 0));
  cw_tree_fixed_update(t, DT);
  NEAR(tip_dist(ch), 0, 1e-3);
  cw_node_set(sb, "teleport_distance", cw_val_float(0));
  cw_node3d_set_position(model, v3(x, 0, 0));
  cw_tree_fixed_update(t, DT);
  CHECK(tip_dist(ch) > 0.05f);
  /* reset: back on the animated pose at once */
  cw_spring_bone3d_reset(sb);
  cw_tree_fixed_update(t, DT);
  NEAR(tip_dist(ch), 0, 1e-6);
  cw_value rv;
  CHECK(cw_node_get_prop(sb, "resets", &rv) && rv.i == 2);
  /* a sphere where the tail hangs pushes it out of the sphere (and its radius) */
  cw_node sh = add(sb, "SpringBoneCollision3D", "Ball");
  cw_node_set(sh, "bone", cw_val_string("Head"));
  cw_node_set(sh, "radius", cw_val_float(0.08));
  cw_node_set(sh, "offset", cw_val_vec3(0, -0.25f, -0.06f));
  for (int k = 0; k < 60; k++) cw_tree_fixed_update(t, DT);
  t3_vec3 a, b;
  float r;
  CHECK(cw_spring_collision3d_world(sh, &a, &b, &r));
  NEAR(r, 0.08, 1e-6);
  for (int i = 0; cw_spring_chain3d_joint_world(ch, i, &j); i++) CHECK(vlen(vsub(j, a)) >= 0.08f + 0.02f - 1e-4f);
  CHECK(tip_dist(ch) > 0.01f);
  uint32_t h = cw_skeleton_hash(model);
  FOLD(h);
  cw_tree_free(t);
}

/* ── SpringBone3D skirt mode ───────────────────────────────────────── */

/* hips at y 1 with a leg (a capsule shape's bone) hanging from x 0.1, and eight chains of three bones round the
 * hips flaring from a 0.2 m ring to 0.34 m at the hem (0.75 m down) */
static t3_object *make_skirt_rig(t3_object **leg) {
  t3_object *root = t3_group_new();
  t3_object *hips = t3_bone_new();
  hips->name = "Hips";
  hips->position = v3(0, 1, 0);
  t3_object_add(root, hips);
  *leg = t3_bone_new();
  (*leg)->name = "Leg";
  (*leg)->position = v3(0.1f, -0.05f, 0);
  t3_object_add(hips, *leg);
  static char names[8][4][8];
  for (int k = 0; k < 8; k++) {
    float a = (float)k * 0.7853982f, s = sinf(a), c = cosf(a);
    t3_object *parent = hips;
    t3_vec3 prev = v3(0, 1, 0);
    for (int i = 0; i < 4; i++) {
      float r = 0.2f + 0.045f * (float)i, y = 1 - 0.25f * (float)i;
      t3_vec3 at = v3(r * s, y, r * c);
      t3_object *b = t3_bone_new();
      snprintf(names[k][i], sizeof names[k][i], i < 3 ? "S%d_%d" : "S%d_e", k, i);
      b->name = names[k][i];
      b->position = vsub(at, prev);
      t3_object_add(parent, b);
      parent = b;
      prev = at;
    }
  }
  return root;
}

/* the deepest any joint or link midpoint of the skirt sits inside the leg's capsule (m) */
static float skirt_pen(cw_node *ch, cw_node shape) {
  t3_vec3 a, b, j[8][3];
  float r, worst = -1;
  CHECK(cw_spring_collision3d_world(shape, &a, &b, &r));
  for (int k = 0; k < 8; k++)
    for (int i = 0; i < 3; i++) cw_spring_chain3d_joint_world(ch[k], i, &j[k][i]);
  for (int k = 0; k < 8; k++)
    for (int i = 0; i < 3; i++)
      for (int m = 0; m < 2; m++) {
        t3_vec3 q = m ? v3((j[k][i].x + j[(k + 1) % 8][i].x) / 2, (j[k][i].y + j[(k + 1) % 8][i].y) / 2, (j[k][i].z + j[(k + 1) % 8][i].z) / 2) : j[k][i];
        t3_vec3 ab = vsub(b, a);
        float t = vdot(vsub(q, a), ab) / vdot(ab, ab);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float d = r - vlen(vsub(q, v3(a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t)));
        if (d > worst) worst = d;
      }
  return worst;
}

/* a leg kicks forward through the front of the skirt and back: in skirt mode the panels go round it (nothing of the
 * skirt inside it); chains on their own (OFF, the control) let it through between two of them */
static float skirt_kick(const char *mode, uint32_t *hash) {
  cw_tree *t = cw_tree_new();
  cw_node model = add(cw_tree_root(t), "Node3D", "Model");
  t3_object *leg;
  t3_object *rig = make_skirt_rig(&leg);
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node sb = add(model, "SpringBone3D", "Skirt");
  cw_node_set(sb, "skirt", cw_val_enum(mode));
  cw_node ch[8];
  for (int k = 0; k < 8; k++) {
    char nm[8], r0[8], e[8];
    snprintf(nm, sizeof nm, "C%d", k);
    snprintf(r0, sizeof r0, "S%d_0", k);
    snprintf(e, sizeof e, "S%d_2", k);
    ch[k] = add(sb, "SpringBoneChain3D", nm);
    cw_node_set(ch[k], "root_bone", cw_val_string(r0));
    cw_node_set(ch[k], "end_bone", cw_val_string(e));
    cw_node_set(ch[k], "stiffness", cw_val_float(200));
    cw_node_set(ch[k], "damping", cw_val_float(6));
  }
  cw_node sh = add(sb, "SpringBoneCollision3D", "LegShape");
  cw_node_set(sh, "bone", cw_val_string("Leg"));
  cw_node_set(sh, "shape", cw_val_enum("CAPSULE"));
  cw_node_set(sh, "radius", cw_val_float(0.08));
  cw_node_set(sh, "tail", cw_val_vec3(0, -0.85f, 0));
  /* still, with nothing touching: the skirt stays on its animated pose */
  for (int k = 0; k < 30; k++) cw_tree_fixed_update(t, DT);
  for (int k = 0; k < 8; k++) NEAR(tip_dist(ch[k]), 0, 1e-5);
  float worst = -1;
  for (int k = 0; k < 120; k++) {
    /* the leg swings 0.6 rad forward and back in two seconds (the bone written by the game: its animated pose) */
    float ang = 0.6f * sinf(3.1415927f * (float)k / 60);
    ang = ang > 0 ? ang : 0;
    leg->quaternion = cw_quat_axis_angle_det(v3(1, 0, 0), -ang);
    cw_tree_fixed_update(t, DT);
    float p = skirt_pen(ch, sh);
    if (p > worst) worst = p;
  }
  /* then still: back on the pose */
  for (int k = 0; k < 240; k++) cw_tree_fixed_update(t, DT);
  float rest = 0;
  for (int k = 0; k < 8; k++) rest = tip_dist(ch[k]) > rest ? tip_dist(ch[k]) : rest;
  fprintf(stderr, "skirt %s: deepest %.4f m, rest %.5f m\n", mode, worst, rest);
  CHECK(rest < 0.01f);
  *hash = cw_skeleton_hash(model);
  cw_tree_free(t);
  return worst;
}

static void test_skirt(void) {
  uint32_t h_ring, h_off, h_open;
  float ring = skirt_kick("RING", &h_ring), open = skirt_kick("OPEN", &h_open), off = skirt_kick("OFF", &h_off);
  CHECK(ring <= 0.002f);  /* a joint or link of the skirt at most 2 mm inside the leg */
  CHECK(open <= 0.002f);  /* (OPEN leaves C7 to C0 unlinked: the leg kicks between C0 and C1) */
  CHECK(off > 0.03f);     /* control: on their own the chains let the leg through */
  FOLD(h_ring);
  FOLD(h_open);
  FOLD(h_off);
}

/* ── the rest pose is the skin's bind pose ───────────────────────────── */

static t3_quat qmul_t(t3_quat a, t3_quat b) {
  return (t3_quat){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

/* Hips -> Neck -> Head, skinned (inverse bind matrices from this pose), the head facing +Z at bind */
static t3_object *make_skinned_head(t3_object **head_out) {
  t3_object *root = t3_group_new();
  t3_object *hips = t3_bone_new(), *neck = t3_bone_new(), *head = t3_bone_new();
  hips->name = "Hips"; neck->name = "Neck"; head->name = "Head";
  hips->position = v3(0, 1, 0);
  neck->position = v3(0, 0.5f, 0);
  head->position = v3(0, 0.15f, 0);
  t3_object_add(root, hips);
  t3_object_add(hips, neck);
  t3_object_add(neck, head);
  t3_object_update_world_matrix(root, false, true);
  t3_object *bones[3] = { hips, neck, head };
  t3_skeleton *sk = t3_skeleton_new(bones, 3, NULL);
  t3_geometry *g = t3_box_geometry_new(0.2f, 0.2f, 0.2f, 1, 1, 1);
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  t3_skinned_mesh *sm = t3_skinned_mesh_new(g, m);
  t3_skinned_mesh_bind(sm, sk, NULL);
  t3_object_add(root, (t3_object *)sm);
  t3_release(sm);
  t3_release(sk);
  t3_release(g);
  t3_release(m);
  *head_out = head;
  return root;
}

/* the head's model-space +Z after the modifiers, against the way to the target (radians) */
static float look_error(cw_node model, t3_vec3 target_model) {
  const cw_pose *p = cw_skeleton_pose(model);
  int h = cw_pose_find(p, "Head");
  t3_quat q = cw_pose_model_rotation(p, h);
  /* q applied to (0, 0, 1) */
  t3_vec3 f = v3(2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y));
  t3_vec3 d = vnorm(vsub(target_model, cw_pose_model_position(p, h)));
  float c = vdot(vnorm(f), d);
  return cw_acos(c > 1 ? 1 : c);
}

/* A game that poses (or animates) the head before the modifiers first bind: the look-at still measures its forward
 * axis from the bind pose (CW_TEST_IK_REST_CURRENT=1, the transforms at bind time, is the control the gate runs). */
static void test_bind_rest(void) {
  cw_tree *t = cw_tree_new();
  cw_node model = add(cw_tree_root(t), "Node3D", "Model");
  t3_object *head;
  t3_object *rig = make_skinned_head(&head);
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  /* the first animated frame: the head turned 13 degrees and pitched 15, before any modifier phase */
  t3_object_set_quaternion(head, qmul_t(cw_quat_axis_angle_det(v3(0, 1, 0), 0.2269f), cw_quat_axis_angle_det(v3(1, 0, 0), 0.2618f)));
  cw_node look = add(model, "LookAtModifier3D", "Look");
  cw_node_set(look, "bone", cw_val_string("Head"));
  cw_node_set(look, "forward_axis", cw_val_vec3(0, 0, 1));
  t3_vec3 target = v3(0.6f, 1.9f, 4);
  cw_node_set(look, "target_position", cw_val_vec3(target.x, target.y, target.z));
  for (int k = 0; k < 3; k++) cw_tree_fixed_update(t, DT);
  float err = look_error(model, target);
  cw_info("bind rest: look-at error %.6f rad (the head bound turned 13 and pitched 15 degrees)", err);
  CHECK(err < 1e-3f); /* (the control, CW_TEST_IK_REST_CURRENT=1, is off by about the turn: this fails) */
  cw_tree_free(t);
}

/* ── a manual advance in the app's fixed_update: the modifiers solve it the same tick ─────────────── */

/* the rotation between two quaternions (radians) */
static float qangle(t3_quat a, t3_quat b) {
  float c = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 2 * cw_acos(c > 1 ? 1 : c);
}

/* The scene runner's order: the tree's fixed tick (nodes, physics, the modifier phase), then the app's fixed_update,
 * which advances a MANUAL animation (here: writes the head's animated rotation), then cw_skeleton_modifiers_refresh.
 * A look-at 70 degrees to the side turns the head drawn that tick by 70 degrees off the animated head. Control:
 * CW_TEST_IK_NO_REFRESH=1, the head drawn as the app left it: 0. */
static void test_manual_refresh(void) {
  cw_tree *t = cw_tree_new();
  cw_node model = add(cw_tree_root(t), "Node3D", "Model");
  t3_object *head;
  t3_object *rig = make_skinned_head(&head);
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node look = add(model, "LookAtModifier3D", "Look");
  cw_node_set(look, "bone", cw_val_string("Head"));
  cw_node_set(look, "forward_axis", cw_val_vec3(0, 0, 1));
  /* the head at (0, 1.65, 0): the target 70 degrees to its left, level */
  float s70, c70;
  cw_sincos(1.2217305f, &s70, &c70);
  cw_node_set(look, "target_position", cw_val_vec3(3 * s70, 1.65f, 3 * c70));
  float worst = 10, worst_err = 0;
  int solved = 0;
  for (int k = 0; k < 30; k++) {
    cw_tree_fixed_update(t, DT);
    /* the app's fixed_update: the animation steps (a small nod that changes every tick) */
    t3_quat anim = cw_quat_axis_angle_det(v3(1, 0, 0), 0.05f * cw_sin(0.2f * (float)k));
    t3_object_set_quaternion(head, anim);
    solved += cw_skeleton_modifiers_refresh(t);
    CHECK_EQ_INT(cw_skeleton_modifiers_refresh(t), 0); /* once a phase */
    float turn = qangle(head->quaternion, anim); /* what is drawn against what the animation gave */
    if (turn < worst) worst = turn;
    float e = fabsf(turn - 1.2217305f);
    if (k > 0 && e > worst_err) worst_err = e;
  }
  cw_info("manual refresh: the head drawn turns %.4f rad (least over 30 ticks) off the animated head, worst error "
          "%.5f against 70 deg, %d refreshes", worst, worst_err, solved);
  CHECK(worst > 1.1f); /* (control CW_TEST_IK_NO_REFRESH=1: 0, this fails) */
  CHECK(worst_err < 0.06f);
  CHECK_EQ_INT(solved, 30);
  /* a tick where the app writes nothing: no second solve */
  cw_tree_fixed_update(t, DT);
  CHECK_EQ_INT(cw_skeleton_modifiers_refresh(t), 0);
  FOLD(worst);
  cw_tree_free(t);
}

/* Foot placement eases once a tick however often a model solves in it: a tree whose app rewrites the animated pose
 * after every phase (a refresh each tick) ends with the same legs, bit for bit, as one that never refreshes. */
static void foot_run(bool rewrite, t3_quat *out, float *pel) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node floor = add(root, "StaticBody3D", "Floor");
  cw_node fs = add(floor, "CollisionShape3D", "Shape");
  cw_node_set(fs, "size", cw_val_vec3(10, 1, 10));
  cw_node3d_set_position(floor, v3(0, -0.5f, 0));
  cw_node step = add(root, "StaticBody3D", "Step");
  cw_node ss = add(step, "CollisionShape3D", "Shape");
  cw_node_set(ss, "size", cw_val_vec3(1, 0.2f, 2));
  cw_node3d_set_position(step, v3(0.6f, 0.1f, 0));
  cw_node model = add(root, "Node3D", "Model");
  cw_node3d_set_position(model, v3(0, 0.12f, 0));
  t3_object *rig = make_rig();
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node feet = add(model, "FootPlacement3D", "Feet");
  cw_node_set(feet, "legs", cw_val_string("UpL,LoL,AnkL;UpR,LoR,AnkR"));
  cw_node_set(feet, "pelvis", cw_val_string("Hips"));
  cw_node_set(feet, "foot_height", cw_val_float(0.02f));
  cw_node_set(feet, "blend_speed", cw_val_float(6));
  t3_object *bones[2] = { t3_object_get_by_name(rig, "LoL"), t3_object_get_by_name(rig, "LoR") };
  for (int k = 0; k < 20; k++) {
    /* the animation: the knees bend a little more each tick (written before the phase, as a FIXED player does) */
    for (int b = 0; b < 2; b++) t3_object_set_quaternion(bones[b], cw_quat_axis_angle_det(v3(1, 0, 0), 0.2f + 0.01f * (float)k));
    cw_tree_fixed_update(t, DT);
    if (rewrite) {
      for (int b = 0; b < 2; b++) t3_object_set_quaternion(bones[b], cw_quat_axis_angle_det(v3(1, 0, 0), 0.2f + 0.01f * (float)k));
      cw_skeleton_modifiers_refresh(t);
    }
  }
  static const char *const names[4] = { "UpL", "LoL", "UpR", "LoR" };
  for (int i = 0; i < 4; i++) out[i] = t3_object_get_by_name(rig, names[i])->quaternion;
  *pel = cw_foot_placement_pelvis_offset(feet);
  cw_tree_free(t);
}
static void test_refresh_once_a_tick(void) {
  t3_quat a[4], b[4];
  float pa, pb;
  foot_run(false, a, &pa);
  foot_run(true, b, &pb);
  CHECK(pa < -0.01f);
  CHECK(!memcmp(&pa, &pb, sizeof pa));
  CHECK(!memcmp(a, b, sizeof a));
}

/* ── a high foot never flips the leg round ─────────────────────────────
 * The right leg's animation sweeps the foot high: a kick forward and up with the leg near straight, then the heel
 * drawn up behind to the hip's height with the knee folded, then down, over ground 0.2 m higher than the model's
 * origin under one foot (so the targets move off the animated ankles and the pelvis drops). Each tick the upper
 * leg's drawn rotation may move at most 12 degrees more than the animation moved it. Control:
 * CW_TEST_IK2_OLD_BEND=1, the bend side from the upper bone's direction off the new reach line: the knee flips. */
static void test_foot_high_sweep(void) {
  cw_tree *t = cw_tree_new();
  cw_node root = cw_tree_root(t);
  cw_node floor = add(root, "StaticBody3D", "Floor");
  cw_node fs = add(floor, "CollisionShape3D", "Shape");
  cw_node_set(fs, "size", cw_val_vec3(10, 1, 10));
  cw_node3d_set_position(floor, v3(0, -0.5f, 0));
  cw_node step = add(root, "StaticBody3D", "Step");
  cw_node ss = add(step, "CollisionShape3D", "Shape");
  cw_node_set(ss, "size", cw_val_vec3(1, 0.4f, 4));
  cw_node3d_set_position(step, v3(-0.6f, 0.0f, 0));
  cw_node model = add(root, "Node3D", "Model");
  cw_node3d_set_position(model, v3(0, 0.25f, 0));
  t3_object *rig = make_rig();
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  cw_node feet = add(model, "FootPlacement3D", "Feet");
  cw_node_set(feet, "legs", cw_val_string("UpL,LoL,AnkL;UpR,LoR,AnkR"));
  cw_node_set(feet, "pelvis", cw_val_string("Hips"));
  cw_node_set(feet, "foot_height", cw_val_float(0.03f));
  cw_node_set(feet, "max_step_down", cw_val_float(0.6f));
  cw_node_set(feet, "ray_up", cw_val_float(0.6f));
  cw_node_set(feet, "blend_speed", cw_val_float(4));
  t3_object *up = t3_object_get_by_name(rig, "UpR"), *lo = t3_object_get_by_name(rig, "LoR");
  const int N = 360;
  float worst = 0, worst_at = 0;
  t3_quat prev_drawn = { 0, 0, 0, 1 }, prev_anim = { 0, 0, 0, 1 };
  for (int k = 0; k < N; k++) {
    /* the thigh's angle about x (pi - 0.1: down, the knee forward; less: forward and up) and the knee's bend */
    float u = (float)k / (float)(N - 1), th, kn;
    if (u < 0.33f) { float w = cw_sin(3.14159265f * u / 0.33f); th = 3.04159f - 1.9f * w; kn = 0.02f + 0.03f * w; }
    else if (u < 0.66f) { float w = cw_sin(3.14159265f * (u - 0.33f) / 0.33f); th = 3.04159f - 0.5f * w; kn = 0.02f + 2.7f * w; }
    else { float w = cw_sin(3.14159265f * (u - 0.66f) / 0.34f); th = 3.04159f + 0.6f * w; kn = 0.02f + 2.2f * w; }
    t3_quat qa = cw_quat_axis_angle_det(v3(1, 0, 0), th);
    t3_object_set_quaternion(up, qa);
    t3_object_set_quaternion(lo, cw_quat_axis_angle_det(v3(1, 0, 0), kn));
    cw_tree_fixed_update(t, DT);
    t3_quat qd = up->quaternion;
    if (k > 3) {
      float jump = qangle(qd, prev_drawn) - qangle(qa, prev_anim);
      if (jump > worst) worst = jump, worst_at = u;
    }
    prev_drawn = qd, prev_anim = qa;
  }
  cw_info("foot high sweep: the upper leg turns at most %.2f deg a tick past the animation (at %.2f of the sweep)",
          worst * 57.29578f, worst_at);
  CHECK(worst < 0.2094f); /* 12 degrees (control CW_TEST_IK2_OLD_BEND=1: the knee flips, this fails) */
  FOLD(worst);
  cw_tree_free(t);
}

/* ── SpringBone3D: collisions that hold, no flip at resonance, a step-up that does not fling ──────── */

/* the swing of the tail's first bone off its animated (rest) direction, radians */
static float swing0(cw_node model) {
  const cw_pose *p = cw_skeleton_pose(model);
  int b = cw_pose_find(p, "T0");
  return qangle(p->rot[b], (t3_quat){ 0, 0, 0, 1 });
}
/* the deepest any joint of the chain is inside the shape (m; negative: outside) */
static float chain_pen(cw_node ch, cw_node shape) {
  float worst = -1;
  t3_vec3 j, a, b;
  float r;
  if (!cw_spring_collision3d_world(shape, &a, &b, &r)) return worst;
  for (int i = 0; cw_spring_chain3d_joint_world(ch, i, &j); i++) {
    t3_vec3 ab = vsub(b, a), aj = vsub(j, a);
    float ll = vdot(ab, ab), t = ll > 0 ? vdot(aj, ab) / ll : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    t3_vec3 q = v3(a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t);
    float d = r - vlen(vsub(j, q));
    if (d > worst) worst = d;
  }
  return worst;
}
static cw_node spring_model(cw_tree *t, cw_node *sb, cw_node *ch) {
  cw_node model = add(cw_tree_root(t), "Node3D", "Model");
  t3_object *rig = make_tail_rig();
  cw_node3d_set_object(model, rig);
  t3_release(rig);
  *sb = add(model, "SpringBone3D", "Springs");
  *ch = add(*sb, "SpringBoneChain3D", "Hair");
  cw_node_set(*ch, "root_bone", cw_val_string("T0"));
  cw_node_set(*ch, "end_bone", cw_val_string("T2"));
  return model;
}

/* A short hair chain (6 mm joints) against a neck capsule right under it, the head bobbing as a run does: no joint
 * ever goes more than 1 mm into the capsule. Control: CW_TEST_SPRING_OLD_COLLIDE=1, the old order (two rounds of
 * length then push, then the length again): the last length pass pulls joints back in. */
static void test_spring_collide(void) {
  cw_tree *t = cw_tree_new();
  cw_node sb, ch;
  cw_node model = spring_model(t, &sb, &ch);
  cw_node_set(ch, "radius", cw_val_float(0.006f));
  cw_node_set(ch, "stiffness", cw_val_float(40));
  cw_node_set(ch, "damping", cw_val_float(2));
  cw_node_set(ch, "gravity_strength", cw_val_float(9.8f));
  cw_node neck = add(sb, "SpringBoneCollision3D", "Neck");
  cw_node_set(neck, "bone", cw_val_string("Head"));
  cw_node_set(neck, "shape", cw_val_enum("CAPSULE"));
  cw_node_set(neck, "radius", cw_val_float(0.05f));
  cw_node_set(neck, "offset", cw_val_vec3(0, -0.3f, -0.06f));
  cw_node_set(neck, "tail", cw_val_vec3(0, 0.02f, -0.06f));
  float worst = -1, x = 0;
  for (int k = 0; k < 300; k++) {
    float sy, cy;
    cw_sincos((float)k * 0.85f, &sy, &cy); /* about 8 bobs a second at 60 ticks: a run */
    x += 5.4f * (float)DT;
    cw_node3d_set_position(model, v3(x, 0.06f * sy, 0));
    cw_tree_fixed_update(t, DT);
    float p = chain_pen(ch, neck);
    if (k > 2 && p > worst) worst = p;
  }
  cw_info("spring collide: deepest joint %.4f m into the neck capsule (head bob at a run)", worst);
  CHECK(worst <= 0.001f); /* (control CW_TEST_SPRING_OLD_COLLIDE=1: centimetres in, this fails) */
  FOLD(worst);
  cw_tree_free(t);
}

/* A stiff short chain (stiffness 300, damping 8: well under critical, 2 sqrt(300) = 35) driven by a bob at its own
 * sway rate: it never swings more than max_angle (2 rad, the default) off the animated direction, so never over its
 * bone. Control in the same case: max_angle 0 (no limit) swings past it. */
static float resonance_swing(float max_angle, float damping) {
  cw_tree *t = cw_tree_new();
  cw_node sb, ch;
  cw_node model = spring_model(t, &sb, &ch);
  cw_node_set(ch, "stiffness", cw_val_float(300));
  cw_node_set(ch, "damping", cw_val_float(damping));
  cw_node_set(ch, "max_angle", cw_val_float(max_angle));
  float worst = 0, x = 0;
  for (int k = 0; k < 400; k++) {
    float sy, cy;
    cw_sincos((float)k * 0.2887f, &sy, &cy); /* sqrt(300) rad/s at 60 ticks: the chain's own rate */
    x += 5.4f * (float)DT;
    cw_node3d_set_position(model, v3(x, 0.16f * sy, 0.16f * cy));
    cw_tree_fixed_update(t, DT);
    float a = swing0(model);
    if (a > worst) worst = a;
  }
  cw_tree_free(t);
  return worst;
}
static void test_spring_resonance(void) {
  float held = resonance_swing(2.0f, 8), free_ = resonance_swing(0, 8), crit = resonance_swing(0, 35);
  cw_info("spring resonance: the first bone swings at most %.3f rad off its animated direction with max_angle 2 "
          "(no limit: %.3f; no limit at damping 35, near critical: %.3f)", held, free_, crit);
  CHECK(held <= 2.0f + 1e-3f);
  CHECK(free_ > 2.0f); /* (the case drives the chain past the limit: what max_angle holds) */
  CHECK(crit < 2.0f);  /* damping near critical (2 sqrt(stiffness)) keeps it from flipping on its own */
  FOLD(held);
}

/* A controller stepping up a 0.41 m ledge in one tick: with the default teleport_distance (0.25 m) the chains move
 * with the model (no fling); the old default (1 m, and no max_angle) flings the tail over its bone. */
static float step_swing(float teleport, float max_angle) {
  cw_tree *t = cw_tree_new();
  cw_node sb, ch;
  cw_node model = spring_model(t, &sb, &ch);
  if (teleport >= 0) cw_node_set(sb, "teleport_distance", cw_val_float(teleport));
  if (max_angle >= 0) cw_node_set(ch, "max_angle", cw_val_float(max_angle));
  for (int k = 0; k < 30; k++) cw_tree_fixed_update(t, DT);
  cw_node3d_set_position(model, v3(0, 0.41f, 0));
  float worst = 0;
  for (int k = 0; k < 60; k++) {
    cw_tree_fixed_update(t, DT);
    float a = swing0(model);
    if (a > worst) worst = a;
  }
  cw_tree_free(t);
  return worst;
}
static void test_spring_step_up(void) {
  float now = step_swing(-1, -1), old = step_swing(1, 0);
  cw_info("spring step-up 0.41 m: the first bone swings at most %.3f rad (defaults); the old defaults %.3f rad", now, old);
  CHECK(now < 0.05f);
  CHECK(old > 1.0f); /* (the old defaults fling it: what the new ones prevent) */
  FOLD(now);
}

/* the cases a gate's control switch reaches (cw_test_env: frame 2 in a cart) */
void cw_test_late(void) {
  test_spring_collide();
  test_spring_resonance();
  test_spring_step_up();
  test_manual_refresh();
  test_refresh_once_a_tick();
  test_foot_high_sweep();
}

CW_TEST_SUITE("ik", { "crowd", test_crowd }, { "spring", test_spring }, { "skirt", test_skirt }, { "bind_rest", test_bind_rest }, { "pose", test_pose }, { "two_bone", test_two_bone }, { "aim", test_aim }, { "ccd_r186", test_ccd_r186 },
              { "fabrik", test_fabrik }, { "nodes", test_nodes }, { "foot_placement", test_foot_placement })
